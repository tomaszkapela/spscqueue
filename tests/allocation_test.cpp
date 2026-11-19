#include "names.hpp"
#include <spsc/spsc_queue.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <new>

// This binary replaces the aligned global operator new, so that it can see what
// the queue's constructor asks for. The replacement is program-wide, which is
// why it has a binary of its own.

namespace
{

// What the replacement saw while recording.
struct Requests
{
    bool recording = false;
    std::size_t count = 0;
    std::size_t size = 0;
    std::size_t alignment = 0;
};

Requests& requests()
{
    static Requests seen;
    return seen;
}

struct alignas(1024) OverAligned
{
    int value;
};

// Checks the slot array's allocation, not the slot layout: aligned to a cache
// block (or more, for over-aligned elements), whole blocks, and room for every slot.
template <class T, spsc::SlotLayout Layout>
void check_slot_allocation(std::size_t capacity)
{
    SCOPED_TRACE(test::layout_name(Layout));
    auto& seen = requests();
    seen = Requests{.recording = true};
    {
        const spsc::SpscQueue<T, Layout> queue(capacity); // on the stack: only the slots allocate
        seen.recording = false;
    }
    ASSERT_EQ(seen.count, 1U);
    const std::size_t elements = (capacity + 1) * sizeof(T); // every slot holds at least a T
    EXPECT_GE(seen.alignment, std::max(std::hardware_destructive_interference_size, alignof(T)));
    EXPECT_EQ(seen.size % seen.alignment, 0U);
    EXPECT_GE(seen.size, elements);
    if constexpr (Layout == spsc::SlotLayout::compact)
    {
        // Compact slots are exactly sizeof(T), so the rounding adds less than a block.
        EXPECT_LT(seen.size - elements, seen.alignment);
    }
}

class Allocation : public testing::TestWithParam<std::size_t>
{
};

} // namespace

// Replaceable allocation functions ([replacement.functions]). They can't use
// operator new themselves, hence aligned_alloc and free. The library's own
// declarations name their parameters differently (libc++: __sz, __p).
// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void* operator new(std::size_t size, std::align_val_t alignment)
{
    const auto align = static_cast<std::size_t>(alignment);
    auto& seen = requests();
    if (seen.recording)
    {
        ++seen.count;
        seen.size = size;
        seen.alignment = align;
    }
    // aligned_alloc wants a size that's a multiple of the alignment.
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory,cppcoreguidelines-no-malloc)
    void* const memory = std::aligned_alloc(align, ((size + (align - 1)) / align) * align);
    if (memory == nullptr)
    {
        throw std::bad_alloc();
    }
    return memory;
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void operator delete(void* memory, std::align_val_t /*alignment*/) noexcept
{
    std::free(memory); // NOLINT(cppcoreguidelines-owning-memory,cppcoreguidelines-no-malloc)
}

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
void operator delete(void* memory, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept
{
    std::free(memory); // NOLINT(cppcoreguidelines-owning-memory,cppcoreguidelines-no-malloc)
}

TEST_P(Allocation, SlotArrayStartsAndEndsOnACacheBlock)
{
    check_slot_allocation<int, spsc::SlotLayout::compact>(GetParam());
    check_slot_allocation<int, spsc::SlotLayout::cache_isolated>(GetParam());
}

// 24 bytes don't divide a block, 300 bytes span two, and 1024-byte alignment
// exceeds the cache separation.
TEST_P(Allocation, FollowsTheElementsSizeAndAlignment)
{
    check_slot_allocation<std::array<char, 24>, spsc::SlotLayout::compact>(GetParam());
    check_slot_allocation<std::array<char, 24>, spsc::SlotLayout::cache_isolated>(GetParam());
    check_slot_allocation<std::array<std::byte, 300>, spsc::SlotLayout::compact>(GetParam());
    check_slot_allocation<std::array<std::byte, 300>, spsc::SlotLayout::cache_isolated>(GetParam());
    check_slot_allocation<OverAligned, spsc::SlotLayout::compact>(GetParam());
    check_slot_allocation<OverAligned, spsc::SlotLayout::cache_isolated>(GetParam());
}

INSTANTIATE_TEST_SUITE_P(Core, Allocation, testing::Values(1, 2, 3, 8, 127), test::capacity_name);
