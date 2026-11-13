// SPDX-License-Identifier: MIT

#ifndef SPSC_SPSC_QUEUE_HPP
#define SPSC_SPSC_QUEUE_HPP

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace spsc
{

enum class SlotLayout
{
    compact,
    cache_isolated
};

// A type with a nothrow copy and no move constructor qualifies: rvalues bind to the copy.
template <class T>
concept queue_element = std::is_object_v<T> && (!std::is_array_v<T>) && // a single object
                        std::same_as<T, std::remove_cv_t<T>> &&         // not const or volatile
                        std::destructible<T> &&                         // removal can't throw...
                        std::is_nothrow_move_constructible_v<T>;        // ...on either step

template <queue_element T, SlotLayout Layout = SlotLayout::compact>
class SpscQueue
{
public:
    explicit SpscQueue(std::size_t capacity)
        : capacity_(checked_capacity(capacity)), slots_(new Slot[num_slots()])
    {
    }

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;
    SpscQueue(SpscQueue&&) = delete;
    SpscQueue& operator=(SpscQueue&&) = delete;

    ~SpscQueue() noexcept
    {
        auto read = read_.load(std::memory_order_relaxed);
        const auto write = write_.load(std::memory_order_relaxed);
        while (read != write)
        {
            std::destroy_at(slots_[read].live());
            read = next(read);
        }
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    [[nodiscard]] bool try_push(const T& value) noexcept(std::is_nothrow_copy_constructible_v<T>)
        requires std::is_copy_constructible_v<T>
    {
        return try_emplace(value);
    }

    [[nodiscard]] bool try_push(T&& value) noexcept { return try_emplace(std::move(value)); }

    template <class... Args>
        requires std::constructible_from<T, Args...>
    [[nodiscard]] bool
    try_emplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>)
    {
        const auto write = write_.load(std::memory_order_relaxed);
        const auto next_write = next(write);
        if (next_write == cached_read_)
        {
            // Acquiring released space orders prior destruction before reconstruction.
            cached_read_ = read_.load(std::memory_order_acquire);
            if (next_write == cached_read_)
            {
                return false;
            }
        }
        std::construct_at(slots_[write].address(), std::forward<Args>(args)...);
        write_.store(next_write, std::memory_order_release);
        return true;
    }

    // Pushes elements of r until full, publishing them with one release store, and
    // returns the first one not pushed. r's iterator operations must not throw
    // either; that can't be checked, since few iterators declare them noexcept.
    template <std::ranges::input_range R>
        requires std::is_nothrow_constructible_v<T, std::ranges::range_reference_t<R>>
    // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward): used as an lvalue, as in std::ranges.
    [[nodiscard]] std::ranges::borrowed_iterator_t<R> try_push_range(R&& r) noexcept
    {
        auto first = std::ranges::begin(r);
        const auto last = std::ranges::end(r);
        const auto start = write_.load(std::memory_order_relaxed);
        auto write = start;
        for (; first != last; ++first)
        {
            const auto next_write = next(write);
            if (next_write == cached_read_)
            {
                cached_read_ = read_.load(std::memory_order_acquire);
                if (next_write == cached_read_)
                {
                    break;
                }
            }
            std::construct_at(slots_[write].address(), *first);
            write = next_write;
        }
        // write == start only if nothing was pushed: a whole lap doesn't fit.
        if (write != start)
        {
            write_.store(write, std::memory_order_release);
        }
        return first;
    }

    // On the producer: at least this many pushes will succeed. Elsewhere, an estimate.
    [[nodiscard]] std::size_t available_to_push() const noexcept { return capacity_ - occupied(); }

    [[nodiscard]] std::optional<T> try_pop() noexcept
    {
        std::optional<T> result;
        const auto read = read_.load(std::memory_order_relaxed);
        if (read == cached_write_)
        {
            // Acquiring publication orders completed construction before access.
            cached_write_ = write_.load(std::memory_order_acquire);
        }
        if (read != cached_write_)
        {
            T* element = slots_[read].live();
            result.emplace(std::move(*element));
            std::destroy_at(element);
            // Releasing the index orders destruction before the producer reuses the slot.
            read_.store(next(read), std::memory_order_release);
        }
        // A single return, so that GCC applies NRVO.
        return result;
    }

    // Calls f on up to max elements in their slots, in order, then frees them with
    // one release store. Returns how many. f must not use this queue.
    template <class F>
        requires std::is_nothrow_invocable_v<F&, T&&>
    // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward): called once per element.
    std::size_t consume(F&& f, std::size_t max = std::numeric_limits<std::size_t>::max()) noexcept
    {
        auto read = read_.load(std::memory_order_relaxed);
        std::size_t consumed = 0;
        for (; consumed < max; ++consumed)
        {
            if (read == cached_write_)
            {
                cached_write_ = write_.load(std::memory_order_acquire);
                if (read == cached_write_)
                {
                    break;
                }
            }
            T* element = slots_[read].live();
            std::invoke(f, std::move(*element)); // Exactly what the constraint checks.
            std::destroy_at(element);
            read = next(read);
        }
        if (consumed != 0)
        {
            read_.store(read, std::memory_order_release);
        }
        return consumed;
    }

    // On the consumer: at least this many pops will succeed. Elsewhere, an estimate.
    [[nodiscard]] std::size_t available_to_pop() const noexcept { return occupied(); }

private:
    static constexpr std::size_t cache_separation = std::hardware_destructive_interference_size;
    static constexpr std::size_t slot_alignment = (Layout == SlotLayout::cache_isolated)
                                                      ? std::max(alignof(T), cache_separation)
                                                      : alignof(T);

    struct alignas(slot_alignment) Slot
    {
        std::byte storage[sizeof(T)];

        // For construction: no T exists here yet.
        T* address() noexcept { return reinterpret_cast<T*>(storage); }
        // For the live element; std::launder requires one to exist.
        T* live() noexcept { return std::launder(address()); }
    };

    static std::size_t checked_capacity(std::size_t capacity)
    {
        if (capacity == 0)
        {
            throw std::invalid_argument("SPSC capacity must be positive");
        }
        // The slots must fit in PTRDIFF_MAX bytes, so that pointer differences work.
        constexpr auto max_slots =
            static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) / sizeof(Slot);
        if (capacity >= max_slots)
        {
            throw std::length_error("SPSC capacity is too large");
        }
        return capacity;
    }

    // One more than capacity_: the spare slot tells full from empty.
    [[nodiscard]] std::size_t num_slots() const noexcept { return capacity_ + 1; }

    // Slot indices run from 0 to capacity_; the last one wraps to 0.
    [[nodiscard]] std::size_t next(std::size_t index) const noexcept
    {
        return (index == capacity_) ? 0 : (index + 1);
    }

    // Always in [0, capacity_]. On the producer or the consumer it errs only in the
    // caller's favour: the other side's index may be stale, but coherence keeps it
    // within a lap. Relaxed: no slot is accessed.
    [[nodiscard]] std::size_t occupied() const noexcept
    {
        const auto read = read_.load(std::memory_order_relaxed);
        const auto write = write_.load(std::memory_order_relaxed);
        return (write >= read) ? (write - read) : (write + num_slots() - read);
    }

    // Immutable, read by both threads. capacity_ comes first: slots_ uses it.
    const std::size_t capacity_;
    const std::unique_ptr<Slot[]> slots_;

    // Producer only, on its own cache line.
    alignas(cache_separation) std::atomic<std::size_t> write_{0};
    std::size_t cached_read_ = 0;

    // Consumer only, on its own cache line.
    alignas(cache_separation) std::atomic<std::size_t> read_{0};
    std::size_t cached_write_ = 0;
};

} // namespace spsc

#endif // SPSC_SPSC_QUEUE_HPP
