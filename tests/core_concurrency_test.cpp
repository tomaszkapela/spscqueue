#include "names.hpp"
#include <spsc/spsc_queue.hpp>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <latch>
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
