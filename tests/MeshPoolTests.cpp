#include "viewer/render/MeshPool.hpp"

#include <doctest/doctest.h>

using viewer::render::RangeAllocator;

TEST_CASE("range allocator hands out, reuses and merges ranges")
{
    RangeAllocator ranges(100);
    const auto a = ranges.allocate(30);
    const auto b = ranges.allocate(30);
    const auto c = ranges.allocate(30);
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(c);
    CHECK(*a == 0);
    CHECK(*b == 30);
    CHECK(*c == 60);
    CHECK(ranges.freeSpace() == 10);
    CHECK_FALSE(ranges.allocate(20));

    // The smallest range that fits: the 10 at the end, not the 30 freed.
    ranges.free(*b, 30);
    const auto d = ranges.allocate(10);
    REQUIRE(d);
    CHECK(*d == 90);
    ranges.free(*d, 10);

    // Freeing the neighbours merges everything back into one range.
    ranges.free(*a, 30);
    ranges.free(*c, 30);
    CHECK(ranges.freeSpace() == 100);
    const auto whole = ranges.allocate(100);
    REQUIRE(whole);
    CHECK(*whole == 0);
    CHECK_FALSE(ranges.allocate(1));
}
