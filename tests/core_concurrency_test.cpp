#include "names.hpp"
#include <spsc/spsc_queue.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <span>
#include <string>
#include <thread>
#include <tuple>

namespace
{

// A sequence number plus derived words: a lost, reordered or torn packet shows.
using Packet = std::array<std::uint64_t, 4>;
Packet make_packet(std::uint64_t i) { return {i, ~i, i ^ 0x12345678, 0xfeedbeef}; }

constexpr std::uint64_t count = 100000;

template <spsc::SlotLayout Layout>
void transfer_one_at_a_time(std::size_t capacity, bool slow_producer)
{
    SCOPED_TRACE(test::layout_name(Layout));
    spsc::SpscQueue<Packet, Layout> queue(capacity);
    std::atomic<bool> corrupted{false}; // Set by the consumer, to stop the producer.
    std::latch start(2);
    std::uint64_t received = 0;
    std::jthread producer(
        [&]
        {
            start.arrive_and_wait();
            for (std::uint64_t i = 0; i < count; ++i)
            {
                const Packet packet = make_packet(i);
                while (!(queue.try_push(packet)))
                {
                    if (corrupted.load(std::memory_order_relaxed))
                    {
                        return;
                    }
                    std::this_thread::yield();
                }
                if (slow_producer && ((i % 7) == 0))
                {
                    std::this_thread::yield();
                }
            }
        });
    std::jthread consumer(
        [&]
        {
            start.arrive_and_wait();
            while (received < count)
            {
                const auto packet = queue.try_pop();
                if (!packet)
                {
                    std::this_thread::yield();
                    continue;
                }
                if ((*packet) != make_packet(received))
                {
                    corrupted.store(true, std::memory_order_relaxed);
                    return;
                }
                ++received;
                if ((!slow_producer) && ((received % 7) == 0))
                {
                    std::this_thread::yield();
                }
            }
        });
    producer.join();
    consumer.join();
    EXPECT_FALSE(corrupted.load());
    EXPECT_EQ(received, count);
    EXPECT_FALSE(queue.try_pop());
}

// Capacity, and whether the producer is the slower side.
class Transfer : public testing::TestWithParam<std::tuple<std::size_t, bool>>
{
};

std::string transfer_name(const testing::TestParamInfo<Transfer::ParamType>& info)
{
    const auto [capacity, slow_producer] = info.param;
    return "capacity_" + std::to_string(capacity) +
           (slow_producer ? "_slow_producer" : "_slow_consumer");
}

} // namespace

TEST_P(Transfer, OneAtATimePreservesSequenceAndPayload)
{
    const auto [capacity, slow_producer] = GetParam();
    transfer_one_at_a_time<spsc::SlotLayout::compact>(capacity, slow_producer);
    transfer_one_at_a_time<spsc::SlotLayout::cache_isolated>(capacity, slow_producer);
}

INSTANTIATE_TEST_SUITE_P(Concurrent, Transfer,
                         testing::Combine(testing::Values(1, 3, 127), testing::Bool()),
                         transfer_name);

namespace
{

// Batches sized by the available counts: a shortfall breaks their guarantee.
template <spsc::SlotLayout Layout>
void transfer_in_batches(std::size_t capacity)
{
    SCOPED_TRACE(test::layout_name(Layout));
    spsc::SpscQueue<Packet, Layout> queue(capacity);
    std::atomic<bool> failed{false}; // A wrong packet or a shortfall; stops both threads.
    std::latch start(2);
    std::uint64_t received = 0;
    std::jthread producer(
        [&]
        {
            start.arrive_and_wait();
            std::array<Packet, 17> batch{};
            std::uint64_t sent = 0;
            std::uint64_t size = 1; // Cycles through 1 to 17.
            while ((sent < count) && !(failed.load(std::memory_order_relaxed)))
            {
                const std::uint64_t free = queue.available_to_push();
                const auto n = std::min({size, free, count - sent});
                if (n == 0)
                {
                    std::this_thread::yield();
                    continue;
                }
                for (std::uint64_t i = 0; i < n; ++i)
                {
                    batch[i] = make_packet(sent + i);
                }
                const auto chunk = std::span(batch).first(n);
                if (queue.try_push_range(chunk) != chunk.end())
                {
                    failed.store(true, std::memory_order_relaxed);
                }
                sent += n;
                size = (size % 17) + 1;
            }
        });
    std::jthread consumer(
        [&]
        {
            start.arrive_and_wait();
            std::uint64_t max = 1; // Cycles through 1 to 13.
            while ((received < count) && !(failed.load(std::memory_order_relaxed)))
            {
                const std::uint64_t available = queue.available_to_pop();
                const auto n = std::min(available, max);
                if (n == 0)
                {
                    std::this_thread::yield();
                    continue;
                }
                const auto consumed = queue.consume(
                    [&](Packet&& p) noexcept
                    {
                        if (p != make_packet(received))
                        {
                            failed.store(true, std::memory_order_relaxed);
                        }
                        ++received;
                    },
                    n);
                if (consumed != n)
                {
                    failed.store(true, std::memory_order_relaxed);
                }
                max = (max % 13) + 1;
            }
        });
    producer.join();
    consumer.join();
    EXPECT_FALSE(failed.load());
    EXPECT_EQ(received, count);
    EXPECT_FALSE(queue.try_pop());
}

class BatchedTransfer : public testing::TestWithParam<std::size_t>
{
};

} // namespace

TEST_P(BatchedTransfer, KeepsOrderAndHonoursAvailableCounts)
{
    transfer_in_batches<spsc::SlotLayout::compact>(GetParam());
    transfer_in_batches<spsc::SlotLayout::cache_isolated>(GetParam());
}

INSTANTIATE_TEST_SUITE_P(Concurrent, BatchedTransfer, testing::Values(1, 3, 127),
                         test::capacity_name);
