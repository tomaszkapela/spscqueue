#include "names.hpp"
#include "payloads.hpp"
#include <spsc/spsc_queue.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <utility>

namespace
{

// Counts its copies.
struct MovePreferred
{
    int value;
    int& copies;
    MovePreferred(int v, int& c) noexcept : value(v), copies(c) {}
    MovePreferred(const MovePreferred& other) noexcept : value(other.value), copies(other.copies)
    {
        ++copies;
    }
    MovePreferred(MovePreferred&&) noexcept = default;
};

// Records its address: an alignment check on `this` here would fold to true.
struct alignas(1024) Aligned
{
    std::uintptr_t address = reinterpret_cast<std::uintptr_t>(this);
    std::byte payload[2048]{};
};

} // namespace

TEST(Core, CapacityOneRejectsOverflowAndReusesReleasedStorage)
{
    spsc::SpscQueue<int> queue(1);
    EXPECT_EQ(queue.capacity(), 1U);
    EXPECT_FALSE(queue.try_pop());
    EXPECT_TRUE(queue.try_push(17));
    EXPECT_FALSE(queue.try_push(23));
    EXPECT_EQ(queue.try_pop(), 17);
    EXPECT_TRUE(queue.try_push(23));
    EXPECT_EQ(queue.try_pop(), 23);
    EXPECT_FALSE(queue.try_pop());
}

TEST(Core, FullRejectionPreservesUniqueOwnership)
{
    spsc::SpscQueue<std::unique_ptr<int>> queue(1);
    EXPECT_TRUE(queue.try_push(std::make_unique<int>(17)));
    auto value = std::make_unique<int>(23);
    EXPECT_FALSE(queue.try_push(std::move(value)));
    ASSERT_NE(value, nullptr);
    // Deliberate: a rejected push must not move from its argument.
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    EXPECT_EQ(*value, 23);
    auto first = queue.try_pop();
    ASSERT_TRUE(first && (*first));
    EXPECT_EQ(*(*first), 17);
    const auto* identity = value.get();
    ASSERT_TRUE(queue.try_push(std::move(value)));
    EXPECT_EQ(value, nullptr);
    auto second = queue.try_pop();
    ASSERT_TRUE(second && (*second));
    EXPECT_EQ(second->get(), identity);
    EXPECT_EQ(*(*second), 23);
    EXPECT_FALSE(queue.try_pop());
}

TEST(Core, InvalidCapacitiesThrow)
{
    EXPECT_THROW((spsc::SpscQueue<int>(0)), std::invalid_argument);
    EXPECT_THROW((spsc::SpscQueue<int>(std::numeric_limits<std::size_t>::max())),
                 std::length_error);
    EXPECT_THROW((spsc::SpscQueue<int>(
                     static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()))),
                 std::length_error);
}

namespace
{

template <spsc::SlotLayout Layout>
void check_partial_drain_and_refill(std::size_t capacity)
{
    SCOPED_TRACE(test::layout_name(Layout));
    spsc::SpscQueue<int, Layout> queue(capacity);
    int produced = 0;
    int consumed = 0;
    for (int round = 0; round < 20; ++round)
    {
        while (queue.try_push(produced))
        {
            ++produced;
        }
        for (std::size_t i = 0; i < ((capacity + 1) / 2); ++i)
        {
            EXPECT_EQ(queue.try_pop(), consumed++);
        }
    }
    while (const auto result = queue.try_pop())
    {
        EXPECT_EQ(*result, consumed++);
    }
    EXPECT_EQ(produced, consumed);
}

template <spsc::SlotLayout Layout>
void check_against_deque_model(std::size_t capacity)
{
    SCOPED_TRACE(test::layout_name(Layout));
    std::mt19937 random(0x51C52026);
    spsc::SpscQueue<int, Layout> queue(capacity);
    std::deque<int> model;
    for (int step = 0; step < 20000; ++step)
    {
        SCOPED_TRACE(testing::Message() << "step=" << step);
        const bool push = (random() % 2) == 0;
        const int value = static_cast<int>(random() % 10000);
        if (push && (model.size() < capacity))
        {
            ASSERT_TRUE(queue.try_push(value));
            model.push_back(value);
        }
        else if (push)
        {
            ASSERT_FALSE(queue.try_push(value)); // Full.
        }
        else if (!(model.empty()))
        {
            ASSERT_EQ(queue.try_pop(), model.front());
            model.pop_front();
        }
        else
        {
            ASSERT_FALSE(queue.try_pop()); // Empty.
        }
    }
    while (!(model.empty()))
    {
        ASSERT_EQ(queue.try_pop(), model.front());
        model.pop_front();
    }
    EXPECT_FALSE(queue.try_pop());
}

template <spsc::SlotLayout Layout>
void check_over_aligned_storage()
{
    SCOPED_TRACE(test::layout_name(Layout));
    spsc::SpscQueue<Aligned, Layout> queue(3);
    for (int i = 0; i < 12; ++i)
    {
        ASSERT_TRUE(queue.try_emplace());
        const auto value = queue.try_pop(); // A copy that kept the slot's address.
        ASSERT_TRUE(value);
        EXPECT_EQ(value->address % alignof(Aligned), 0U);
    }
}

class Fifo : public testing::TestWithParam<std::size_t>
{
};

} // namespace

TEST_P(Fifo, PartialDrainRefillAndRepeatedWrapPreserveFifo)
{
    check_partial_drain_and_refill<spsc::SlotLayout::compact>(GetParam());
    check_partial_drain_and_refill<spsc::SlotLayout::cache_isolated>(GetParam());
}

TEST_P(Fifo, SeededOperationsMatchIndependentDequeModel)
{
    check_against_deque_model<spsc::SlotLayout::compact>(GetParam());
    check_against_deque_model<spsc::SlotLayout::cache_isolated>(GetParam());
}

INSTANTIATE_TEST_SUITE_P(Core, Fifo, testing::Values(1, 2, 3, 8, 127), test::capacity_name);

TEST(Core, OverAlignedElementsHaveValidStorageAcrossWraparound)
{
    check_over_aligned_storage<spsc::SlotLayout::compact>();
    check_over_aligned_storage<spsc::SlotLayout::cache_isolated>();
}

TEST(Core, LifetimesBalanceThroughFailedConstructionExtractionAndNonemptyDestruction)
{
    test::Counts counts;
    {
        spsc::SpscQueue<test::Tracked> queue(2);
        EXPECT_EQ(counts.live, 0);
        counts.fail_construction = true;
        EXPECT_THROW((void)queue.try_emplace(counts, 99), std::runtime_error);
        EXPECT_FALSE(queue.try_pop());
        EXPECT_EQ(counts.live, 0);
        counts.fail_construction = false;
        ASSERT_TRUE(queue.try_emplace(counts, 17));
        ASSERT_TRUE(queue.try_emplace(counts, 23));
        EXPECT_FALSE(queue.try_emplace(counts, 42));
        EXPECT_EQ(counts.live, 2);
        {
            const auto value = queue.try_pop();
            ASSERT_TRUE(value);
            EXPECT_EQ(value->value, 17);
            EXPECT_EQ(counts.live, 2); // One queued value and the returned value.
        }
        EXPECT_EQ(counts.live, 1);
    }
    EXPECT_EQ(counts.live, 0);
    EXPECT_EQ(counts.constructed, counts.destroyed);
    EXPECT_GE(counts.moves, 1); // Extraction moves; NRVO may elide more.
}

TEST(Core, DestructionAfterWraparoundDestroysEachQueuedElementOnce)
{
    test::Counts counts;
    {
        spsc::SpscQueue<test::Tracked> queue(3); // Slots 0 to 3.
        for (int i = 0; i < 3; ++i)
        {
            ASSERT_TRUE(queue.try_emplace(counts, i));
        }
        ASSERT_TRUE(queue.try_pop());
        ASSERT_TRUE(queue.try_pop());
        ASSERT_TRUE(queue.try_emplace(counts, 3));
        ASSERT_TRUE(queue.try_emplace(counts, 4)); // The write index wraps past slot 3.
        EXPECT_EQ(counts.live, 3);                 // Slots 2, 3 and 0: read is 2, write is 1.
    }
    EXPECT_EQ(counts.live, 0);
    EXPECT_EQ(counts.constructed, counts.destroyed);
}

TEST(Core, ExtractionMovesEvenWhenACopyIsAvailable)
{
    int copies = 0;
    spsc::SpscQueue<MovePreferred> queue(1);
    ASSERT_TRUE(queue.try_emplace(17, copies));
    const auto value = queue.try_pop();
    ASSERT_TRUE(value);
    EXPECT_EQ(value->value, 17);
    EXPECT_EQ(copies, 0);
}

TEST(Core, NothrowCopyOnlyElementsAreExtractedByCopy)
{
    spsc::SpscQueue<test::LegacyCopyOnly> queue(1);
    ASSERT_TRUE(queue.try_emplace(17));
    auto value = queue.try_pop();
    ASSERT_TRUE(value);
    EXPECT_EQ(value->value, 17);
}
