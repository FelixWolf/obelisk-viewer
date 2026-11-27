#include "RegionMap.hpp"

#include <doctest/doctest.h>

using namespace viewer;

TEST_CASE("Region map: a region's tile on the map server")
{
    CHECK(mapTileUrl("https://map.secondlife.com/", 1000, 1001) ==
          "https://map.secondlife.com/map-1-1000-1001-objects.jpg");
    CHECK(mapTileUrl("http://grid.example:8002", 7, 9) ==
          "http://grid.example:8002/map-1-7-9-objects.jpg");
}

TEST_CASE("Region map: where a position is on the map, north up")
{
    const glm::uvec2 centre{1000, 1000};
    const glm::ivec2 span{5, 3};
    // The middle of the middle region is the middle of the map.
    auto at = mapPlace(centre, span, {1000 * 256 + 128, 1000 * 256 + 128});
    REQUIRE(at);
    CHECK(at->x == doctest::Approx(0.5));
    CHECK(at->y == doctest::Approx(0.5));
    // The south-west corner of the map is its bottom left.
    at = mapPlace(centre, span, {998 * 256, 999 * 256});
    REQUIRE(at);
    CHECK(at->x == doctest::Approx(0.0));
    CHECK(at->y == doctest::Approx(1.0));
    // North of ours is up; east of it, right.
    at = mapPlace(centre, span, {1001 * 256 + 128, 1001 * 256 + 128});
    REQUIRE(at);
    CHECK(at->x == doctest::Approx(3.5 / 5.0));
    CHECK(at->y == doctest::Approx(1.0 / 6.0));
    // Beyond the regions shown: off the map.
    CHECK_FALSE(mapPlace(centre, span, {1003 * 256 + 1, 1000 * 256}));
    CHECK_FALSE(mapPlace(centre, span, {1000 * 256, 998 * 256}));
}
