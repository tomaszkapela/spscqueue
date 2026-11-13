// Compile-time checks, built into spsc_core_tests.

#include "payloads.hpp"
#include <spsc/spsc_queue.hpp>

#include <atomic>
#include <cstddef>
#include <iterator>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// The README's lock-free claim; the header itself doesn't require it.
static_assert(std::atomic<std::size_t>::is_always_lock_free);

namespace
{

struct ThrowingMoveOnly
{
    ThrowingMoveOnly() = default;
    ThrowingMoveOnly(const ThrowingMoveOnly&) = delete;
    ThrowingMoveOnly(ThrowingMoveOnly&&) noexcept(false) {}
};

struct DeletedMove
{
    DeletedMove() = default;
    DeletedMove(const DeletedMove&) noexcept = default;
    DeletedMove(DeletedMove&&) = delete;
};

// Visitors for consume().
struct NothrowVisitor
{
    void operator()(int&&) const noexcept {}
};
struct ThrowingVisitor
{
    void operator()(int&&) const {}
};

// Element types:
static_assert(spsc::queue_element<int>);
static_assert(spsc::queue_element<std::unique_ptr<int>>);
static_assert(spsc::queue_element<test::LegacyCopyOnly>);
static_assert(!spsc::queue_element<ThrowingMoveOnly>);
static_assert(!spsc::queue_element<DeletedMove>);
static_assert(!spsc::queue_element<int&>);
static_assert(!spsc::queue_element<int[3]>);
static_assert(!spsc::queue_element<void>);
static_assert(!spsc::queue_element<const int>);

// Exception specifications follow the selected element constructor:
static_assert(noexcept(std::declval<spsc::SpscQueue<int>&>().try_push(1)));
static_assert(noexcept(std::declval<spsc::SpscQueue<std::string>&>().try_push(std::string())));
static_assert(!noexcept(std::declval<spsc::SpscQueue<std::string>&>().try_push(
    std::declval<const std::string&>()))); // Copying a string may throw.
static_assert(noexcept(std::declval<spsc::SpscQueue<test::Tracked>&>().try_pop()));
static_assert(!noexcept(
    std::declval<spsc::SpscQueue<test::Tracked>&>().try_emplace(std::declval<test::Counts&>(), 1)));

// Batch operations accept only nonthrowing construction and visitors. Concepts,
// because outside a template an invalid requires-expression is an error, not false.
template <class Queue, class R>
concept can_push_range = requires(Queue& q, R r) {
    { q.try_push_range(r) } noexcept;
};
template <class Queue, class F>
concept can_consume = requires(Queue& q, F f) {
    { q.consume(f) } noexcept;
};
static_assert(can_push_range<spsc::SpscQueue<std::string>,
                             std::ranges::subrange<std::move_iterator<std::string*>>>);
static_assert(!can_push_range<spsc::SpscQueue<std::string>,
                              std::span<const std::string>>); // Copies may throw.
// A temporary vector's iterators would dangle, so the result is std::ranges::dangling.
static_assert(std::is_same_v<
              decltype(std::declval<spsc::SpscQueue<int>&>().try_push_range(std::vector<int>{})),
              std::ranges::dangling>);
static_assert(can_consume<spsc::SpscQueue<int>, NothrowVisitor>);
static_assert(!can_consume<spsc::SpscQueue<int>, ThrowingVisitor>);

// The queue itself is neither copyable nor movable:
static_assert(!std::is_copy_constructible_v<spsc::SpscQueue<int>>);
static_assert(!std::is_move_constructible_v<spsc::SpscQueue<int>>);

} // namespace
