#ifndef SPSC_TESTS_PAYLOADS_HPP
#define SPSC_TESTS_PAYLOADS_HPP

#include <stdexcept>

namespace test
{

struct Counts
{
    int live = 0;
    int constructed = 0;
    int destroyed = 0;
    int moves = 0;
    bool fail_construction = false;
};

// Copy-only: rvalues bind to the nothrow copy, so extraction copies.
struct LegacyCopyOnly
{
    int value;
    explicit LegacyCopyOnly(int v) noexcept : value(v) {}
    LegacyCopyOnly(const LegacyCopyOnly&) noexcept = default;
};

// Reports its constructions, moves and destructions to the caller's Counts.
struct Tracked
{
    Counts& counts;
    int value;

    Tracked(Counts& c, int v) : counts(c), value(v)
    {
        if (counts.fail_construction)
        {
            throw std::runtime_error("construction failed");
        }
        ++(counts.live);
        ++(counts.constructed);
    }
    Tracked(Tracked&& other) noexcept : counts(other.counts), value(other.value)
    {
        ++(counts.moves);
        ++(counts.live);
        ++(counts.constructed);
    }
    Tracked(const Tracked&) = delete;
    Tracked& operator=(const Tracked&) = delete;
    ~Tracked() noexcept
    {
        --(counts.live);
        ++(counts.destroyed);
    }
};

} // namespace test

#endif // SPSC_TESTS_PAYLOADS_HPP
