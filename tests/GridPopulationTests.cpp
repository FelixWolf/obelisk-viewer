#include "GridPopulation.hpp"

#include <doctest/doctest.h>

using namespace viewer;
using namespace std::chrono_literals;

namespace
{

constexpr uint64_t handle(uint32_t x, uint32_t y)
{
    return static_cast<uint64_t>(x * 256) << 32 | static_cast<uint64_t>(y * 256);
}

} // namespace

TEST_CASE("Grid population: regions found on the map, the people in each counted")
{
    GridPopulation p;
    const auto t0 = GridPopulation::Clock::time_point{} + 1h;

    // Unknown: where it is is asked first, then who's in it.
    CHECK_FALSE(p.people("Mauve", t0));
    auto r = p.requests(t0);
    REQUIRE(r.regions.size() == 1);
    CHECK(r.regions.front() == "mauve");
    CHECK(r.agents.empty());
    CHECK(p.requests(t0 + 1s).regions.empty()); // not again while the answer comes

    const GridPopulation::MapBlock found[] = {{"Mauve", 1000, 1001}, {"Mauvelous", 1002, 1001}};
    p.mapBlocks(found);
    CHECK_FALSE(p.people("mauve", t0 + 2s));
    r = p.requests(t0 + 2s);
    REQUIRE(r.agents.size() == 1);
    CHECK(r.agents.front() == handle(1000, 1001));
    CHECK(p.requests(t0 + 3s).agents.empty()); // not again while the answer comes

    // Its spots (by their positions), in any answer.
    const uint32_t x = 1000 * 256;
    const uint32_t y = 1001 * 256;
    const GridPopulation::Spot spots[] = {{x + 10, y + 20, 3}, {x + 200, y + 250, 2}};
    p.agentLocations(spots, t0 + 3s);
    CHECK(p.people("MAUVE", t0 + 4s) == 5);

    // Asked, and nothing came: nobody's there.
    CHECK_FALSE(p.people("Mauvelous", t0 + 4s));
    CHECK(p.requests(t0 + 4s).agents.size() == 1);
    CHECK_FALSE(p.people("Mauvelous", t0 + 6s));
    CHECK(p.people("Mauvelous", t0 + 10s) == 0);

    // Kept five minutes, then asked again when wanted (the old count shows
    // meanwhile), and replaced by the new answer.
    CHECK(p.people("Mauve", t0 + 4min) == 5);
    CHECK(p.requests(t0 + 4min).agents.empty());
    CHECK(p.people("Mauve", t0 + 6min) == 5);
    r = p.requests(t0 + 6min);
    REQUIRE(r.agents.size() == 1);
    CHECK(r.regions.empty()); // found: not looked up again
    const GridPopulation::Spot fewer[] = {{x + 1, y + 1, 1}};
    p.agentLocations(fewer, t0 + 6min + 1s);
    CHECK(p.people("Mauve", t0 + 6min + 2s) == 1);

    // A name not found is asked again after a while; one there's none of
    // isn't.
    CHECK_FALSE(p.people("Nowhere", t0 + 7min));
    CHECK(p.requests(t0 + 7min).regions.size() == 1);
    CHECK(p.requests(t0 + 7min + 10s).regions.empty());
    CHECK(p.requests(t0 + 7min + 31s).regions.size() == 1);
    CHECK_FALSE(p.people("Gone", t0 + 8min));
    REQUIRE(p.requests(t0 + 8min).regions.size() == 1);
    const GridPopulation::MapBlock gone[] = {{"gone", 0, 0}};
    p.mapBlocks(gone);
    CHECK(p.requests(t0 + 8min + 1s).regions.empty()); // (Nowhere, asked at 7:31, waits)
    CHECK_FALSE(p.people("Gone", t0 + 8min + 1s));

    // The search ignores case, the answer doesn't: the region as it's
    // named, and what was asked at 0, 0, in either order.
    CHECK_FALSE(p.people("continental", t0 + 8min));
    CHECK_FALSE(p.people("Harbor", t0 + 8min));
    const GridPopulation::MapBlock continental[] = {{"continental", 0, 0},
                                                    {"Continental", 989, 1383}};
    p.mapBlocks(continental);
    const GridPopulation::MapBlock harbor[] = {{"Harbor", 990, 1383}, {"harbor", 0, 0}};
    p.mapBlocks(harbor);
    const GridPopulation::Spot there[] = {{989 * 256 + 5, 1383 * 256 + 5, 4}};
    p.agentLocations(there, t0 + 8min);
    CHECK(p.people("continental", t0 + 8min) == 4);
    CHECK_FALSE(p.people("HARBOR", t0 + 8min)); // found, not counted yet

    p.clear();
    CHECK_FALSE(p.people("Mauve", t0 + 9min));
}
