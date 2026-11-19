#include "names.hpp"
#include "payloads.hpp"
#include <spsc/spsc_queue.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string>

namespace
{

struct Calls
{
    int allocations = 0;
    int deallocations = 0;
    std::size_t allocated = 0;   // the count passed to allocate
    std::size_t deallocated = 0; // the count passed to deallocate
    std::size_t bytes = 0;       // allocated times the rebound value type's size
    std::size_t alignment = 0;   // the rebound value type's alignment
};

// Stateful: records its calls; can return storage misaligned by 16 bytes, and can
// report a small max_size.
template <class T>
struct CountingAllocator
{
    // NOLINTNEXTLINE(readability-identifier-naming): the name the allocator requirements use.
    using value_type = T;

    explicit CountingAllocator(Calls* calls, bool misalign = false,
                               std::size_t max = std::numeric_limits<std::size_t>::max()) noexcept
        : calls(calls), misalign(misalign), max(max)
    {
    }
    template <class U>
    explicit CountingAllocator(const CountingAllocator<U>& other) noexcept
        : calls(other.calls), misalign(other.misalign), max(other.max)
    {
    }

    T* allocate(std::size_t count)
    {
        ++(calls->allocations);
        calls->allocated = count;
        calls->bytes = count * sizeof(T);
        calls->alignment = alignof(T);
        auto* raw = static_cast<std::byte*>(
            ::operator new((count * sizeof(T)) + 64, std::align_val_t(alignof(T))));
        return reinterpret_cast<T*>(raw + (misalign ? 16 : 0));
    }
    void deallocate(T* pointer, std::size_t count) noexcept
    {
        ++(calls->deallocations);
        calls->deallocated = count;
        auto* raw = reinterpret_cast<std::byte*>(pointer) - (misalign ? 16 : 0);
        ::operator delete(raw, std::align_val_t(alignof(T)));
    }
    [[nodiscard]] std::size_t max_size() const noexcept { return max; }
    template <class U>
    bool operator==(const CountingAllocator<U>& other) const noexcept
    {
        return calls == other.calls;
    }

    Calls* calls;
    bool misalign;
    std::size_t max;
};

// Stores the distance from its own address, like boost::interprocess::offset_ptr,
// so it stays valid only if the queue recomputes the slots' address on each access.
template <class T>
class OffsetPtr
{
public:
    // NOLINTBEGIN(readability-identifier-naming): the names pointer_traits uses.
    using element_type = T;
    using difference_type = std::ptrdiff_t;
    // NOLINTEND(readability-identifier-naming)

    OffsetPtr() = default;
    explicit OffsetPtr(T* pointer) noexcept { set(pointer); }
    OffsetPtr(const OffsetPtr& other) noexcept { set(other.get()); }
    OffsetPtr& operator=(const OffsetPtr& other) noexcept
    {
        set(other.get());
        return *this;
    }
    ~OffsetPtr() = default;

    [[nodiscard]] T* get() const noexcept
    {
        if (offset_ == 0)
        {
            return nullptr;
        }
        // NOLINTNEXTLINE(performance-no-int-to-ptr): an offset pointer is address arithmetic.
        return reinterpret_cast<T*>(reinterpret_cast<std::intptr_t>(this) + offset_);
    }
    T* operator->() const noexcept { return get(); }

private:
    // 0 means null: the pointee is never the pointer itself.
    void set(T* pointer) noexcept
    {
        offset_ = (pointer == nullptr) ? 0
                                       : (reinterpret_cast<std::intptr_t>(pointer) -
                                          reinterpret_cast<std::intptr_t>(this));
    }

    std::intptr_t offset_ = 0;
};

template <class T>
struct OffsetAllocator
{
    // NOLINTBEGIN(readability-identifier-naming): the names the allocator requirements use.
    using value_type = T;
    using pointer = OffsetPtr<T>;
    // NOLINTEND(readability-identifier-naming)

    OffsetAllocator() = default;
    template <class U>
    explicit OffsetAllocator(const OffsetAllocator<U>& /*other*/) noexcept
    {
    }
    pointer allocate(std::size_t count) { return pointer(std::allocator<T>().allocate(count)); }
    void deallocate(const pointer& p, std::size_t count) noexcept
    {
        std::allocator<T>().deallocate(p.get(), count);
    }
    template <class U>
    bool operator==(const OffsetAllocator<U>& /*other*/) const noexcept
    {
        return true;
    }
};

// Records the requests that reach it.
struct RecordingResource : std::pmr::memory_resource
{
    std::size_t bytes = 0;
    std::size_t alignment = 0;

private:
    void* do_allocate(std::size_t size, std::size_t align) override
    {
        bytes = size;
        alignment = align;
        return std::pmr::new_delete_resource()->allocate(size, align);
    }
    void do_deallocate(void* memory, std::size_t size, std::size_t align) override
    {
        std::pmr::new_delete_resource()->deallocate(memory, size, align);
    }
    [[nodiscard]] bool do_is_equal(const memory_resource& other) const noexcept override
    {
        return this == &other;
    }
};

struct alignas(1024) OverAligned
{
    int value;
};

constexpr std::size_t rounded_up(std::size_t value, std::size_t multiple)
{
    return ((value + (multiple - 1)) / multiple) * multiple;
}

// The documented layout: compact slots are sizeof(T); cache_isolated slots are
// padded to a cache block (or T's alignment, if larger). The array is rounded up
// to whole blocks.
template <class T, spsc::SlotLayout Layout>
void check_slot_storage(std::size_t capacity)
{
    SCOPED_TRACE(test::layout_name(Layout));
    constexpr std::size_t block = std::hardware_destructive_interference_size;
    constexpr std::size_t alignment = std::max(block, alignof(T));
    constexpr std::size_t slot =
        (Layout == spsc::SlotLayout::compact) ? sizeof(T) : rounded_up(sizeof(T), alignment);

    Calls calls;
    {
        const spsc::SpscQueue<T, CountingAllocator<T>, Layout> queue(capacity,
                                                                     CountingAllocator<T>(&calls));
    }
    EXPECT_EQ(calls.allocations, 1);
    EXPECT_EQ(calls.deallocations, 1);
    EXPECT_EQ(calls.deallocated, calls.allocated);
    EXPECT_EQ(calls.alignment, alignment);
    EXPECT_EQ(calls.bytes, rounded_up((capacity + 1) * slot, alignment));
}

class Allocator : public testing::TestWithParam<std::size_t>
{
};

} // namespace

TEST_P(Allocator, SlotArrayIsWholeAlignedCacheBlocks)
{
    check_slot_storage<int, spsc::SlotLayout::compact>(GetParam());
    check_slot_storage<int, spsc::SlotLayout::cache_isolated>(GetParam());
}

// 24 bytes don't divide a block, 300 bytes span two, and 1024-byte alignment
// exceeds the cache separation.
TEST_P(Allocator, SlotArrayFollowsTheElementsSizeAndAlignment)
{
    check_slot_storage<std::array<char, 24>, spsc::SlotLayout::compact>(GetParam());
    check_slot_storage<std::array<char, 24>, spsc::SlotLayout::cache_isolated>(GetParam());
    check_slot_storage<std::array<std::byte, 300>, spsc::SlotLayout::compact>(GetParam());
    check_slot_storage<std::array<std::byte, 300>, spsc::SlotLayout::cache_isolated>(GetParam());
    check_slot_storage<OverAligned, spsc::SlotLayout::compact>(GetParam());
    check_slot_storage<OverAligned, spsc::SlotLayout::cache_isolated>(GetParam());
}

INSTANTIATE_TEST_SUITE_P(Core, Allocator, testing::Values(1, 2, 3, 8, 127), test::capacity_name);

TEST(AllocatorUse, ElementsLeftInTheQueueAreDestroyedBeforeTheStorageIsReturned)
{
    Calls calls;
    test::Counts counts;
    {
        spsc::SpscQueue<test::Tracked, CountingAllocator<test::Tracked>> queue(
            15, CountingAllocator<test::Tracked>(&calls));
        for (int i = 0; i < 40; ++i)
        {
            ASSERT_TRUE(queue.try_emplace(counts, i));
            ASSERT_TRUE(queue.try_pop());
        }
        ASSERT_TRUE(queue.try_emplace(counts, 99)); // left for the destructor
        EXPECT_EQ(calls.allocations, 1);
        EXPECT_EQ(calls.deallocations, 0);
    }
    EXPECT_EQ(calls.deallocations, 1);
    EXPECT_EQ(counts.constructed, counts.destroyed);
}

TEST(AllocatorUse, MisalignedStorageThrowsAndIsReturned)
{
    Calls calls;
    using Queue = spsc::SpscQueue<int, CountingAllocator<int>>;
    EXPECT_THROW(Queue(4, CountingAllocator<int>(&calls, true)), std::bad_alloc);
    EXPECT_EQ(calls.allocations, 1);
    EXPECT_EQ(calls.deallocations, 1);
    EXPECT_EQ(calls.deallocated, calls.allocated);
}

TEST(AllocatorUse, CapacityBeyondTheAllocatorsMaxSizeThrowsBeforeAllocating)
{
    // max_size counts blocks: two blocks hold this many int slots, the spare included.
    constexpr std::size_t fits =
        ((2 * std::hardware_destructive_interference_size) / sizeof(int)) - 1;
    using Queue = spsc::SpscQueue<int, CountingAllocator<int>>;
    Calls calls;
    EXPECT_NO_THROW(Queue(fits, CountingAllocator<int>(&calls, false, 2)));
    EXPECT_EQ(calls.allocations, 1);
    EXPECT_THROW(Queue(fits + 1, CountingAllocator<int>(&calls, false, 2)), std::length_error);
    EXPECT_EQ(calls.allocations, 1);
}

TEST(AllocatorUse, PolymorphicAllocatorRequestsAlignedWholeBlocks)
{
    RecordingResource resource;
    {
        spsc::SpscQueue<std::pmr::string, std::pmr::polymorphic_allocator<std::pmr::string>> queue(
            3, &resource);
        ASSERT_TRUE(queue.try_push(std::pmr::string(40, 'x')));
        const auto popped = queue.try_pop();
        ASSERT_TRUE(popped);
        EXPECT_EQ(*popped, std::pmr::string(40, 'x'));
        // The element keeps its own allocator: the queue's doesn't reach it.
        EXPECT_NE(popped->get_allocator().resource(), &resource);
    }
    EXPECT_GE(resource.alignment, std::hardware_destructive_interference_size);
    EXPECT_EQ(resource.bytes % resource.alignment, 0U);
    EXPECT_GE(resource.bytes, 4 * sizeof(std::pmr::string));
}

TEST(AllocatorUse, OffsetPointersWork)
{
    spsc::SpscQueue<std::string, OffsetAllocator<std::string>, spsc::SlotLayout::cache_isolated>
        queue(3);
    for (int i = 0; i < 10; ++i)
    {
        ASSERT_TRUE(queue.try_push(std::string(40, 'a')));
        ASSERT_TRUE(queue.try_push(std::to_string(i)));
        EXPECT_EQ(queue.try_pop(), std::string(40, 'a'));
        EXPECT_EQ(queue.try_pop(), std::to_string(i));
    }
}
