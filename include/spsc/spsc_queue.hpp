// SPDX-License-Identifier: MIT

#ifndef SPSC_SPSC_QUEUE_HPP
#define SPSC_SPSC_QUEUE_HPP

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <optional>
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
