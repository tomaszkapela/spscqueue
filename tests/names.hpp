#ifndef SPSC_TESTS_NAMES_HPP
#define SPSC_TESTS_NAMES_HPP

#include <spsc/spsc_queue.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

namespace test
{

// For traces in helpers that run once per layout.
constexpr const char* layout_name(spsc::SlotLayout layout)
{
    return (layout == spsc::SlotLayout::compact) ? "compact" : "cache_isolated";
}

// Names a case after its capacity, as in Core/Fifo.Name/capacity_127.
inline std::string capacity_name(const testing::TestParamInfo<std::size_t>& info)
{
    return "capacity_" + std::to_string(info.param);
}

} // namespace test

#endif // SPSC_TESTS_NAMES_HPP
