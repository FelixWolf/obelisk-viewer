#include "AutomaticProbes.hpp"

#include <doctest/doctest.h>

#include <vector>

using namespace viewer;

namespace
{

// `count` small objects spread through the 32 m cell at (32..64, 32..64, 0..32).
std::vector<ProbeObject> cluster(int count)
{
    std::vector<ProbeObject> objects;
    for(int i = 0; i < count; i++)
    {
        const float t = static_cast<float>(i) / static_cast<float>(count);
        objects.push_back({glm::vec3(36.0f + 20.0f * t, 40.0f + 10.0f * t, 4.0f + 8.0f * t), 1.0f});
    }
    return objects;
}

} // namespace

TEST_CASE("Automatic probes: in every 32 m cell with objects up to its size")
{
    const auto flat = [](glm::vec2) { return 0.0f; };
    CHECK(placeAutomaticProbes({}, flat, 0).empty());
    // However few (one object makes the octree's 32 m node).
    CHECK(placeAutomaticProbes(cluster(1), flat, 0).size() == 1);

    const auto probes = placeAutomaticProbes(cluster(200), flat, 0);
    REQUIRE(probes.size() == 1);
    const auto& p = probes[0];
    // Centered on the objects' bounds (about 35..57, 39..51) sideways...
    CHECK(p.origin.x == doctest::Approx(46.0f).epsilon(0.01));
    CHECK(p.origin.y == doctest::Approx(45.0f).epsilon(0.01));
    // ...reaching their corners from where it ended up, and high enough for
    // its near clip (half the radius) to clear the ground (+2 m).
    CHECK(p.radius >= 8.0f);
    CHECK(p.origin.z >= 2.0f + 0.5f * p.radius - 1e-3f);
    CHECK(p.nearClip == doctest::Approx(0.5f * p.radius));
    CHECK_FALSE(p.manual);
    CHECK_FALSE(p.box.has_value());

    // Objects count as shrink-wrapped (a quarter of their radius): up to
    // 128 m they settle in or below the 32 m node; bigger ones stay above.
    auto objects = cluster(200);
    for(auto& o : objects)
    {
        o.radius = 20.0f;
    }
    CHECK(placeAutomaticProbes(objects, flat, 0).size() == 1);
    for(auto& o : objects)
    {
        o.radius = 40.0f;
    }
    CHECK(placeAutomaticProbes(objects, flat, 0).size() == 1);
    for(auto& o : objects)
    {
        o.radius = 130.0f;
    }
    CHECK(placeAutomaticProbes(objects, flat, 0).empty());

    // Ids follow the cell.
    CHECK(placeAutomaticProbes(cluster(150), flat, 0)[0].id == probes[0].id);
    CHECK(placeAutomaticProbes(cluster(150), flat, 7)[0].id != probes[0].id);
}

TEST_CASE("Automatic probes: a floor counts, and the probe moves into the space over it")
{
    const auto flat = [](glm::vec2) { return 0.0f; };
    // A 60 m floor (a box half a meter thick, its top at 100.25) and a
    // post standing on it.
    std::vector<ProbeObject> objects;
    ProbeObject floor;
    floor.center = glm::vec3(47.0f, 47.0f, 100.0f);
    floor.half = glm::vec3(30.0f, 30.0f, 0.25f);
    floor.radius = glm::length(floor.half);
    objects.push_back(floor);
    ProbeObject box; // a post off to one side, up to 116
    box.center = glm::vec3(35.0f, 35.0f, 108.0f);
    box.half = glm::vec3(0.5f, 0.5f, 8.0f);
    box.radius = glm::length(box.half);
    objects.push_back(box);

    const auto probes = placeAutomaticProbes(objects, flat, 0);
    REQUIRE(probes.size() == 1);
    const auto& p = probes[0];
    // Over the floor's middle...
    CHECK(p.origin.x == doctest::Approx(47.0f).epsilon(0.01));
    CHECK(p.origin.y == doctest::Approx(47.0f).epsilon(0.01));
    // ...and up from the bounds' middle (107.875): halfway between where
    // the downward rays meet the floor's top (100.25) and the bounds' top.
    CHECK(p.origin.z == doctest::Approx(108.125f).epsilon(0.001));
    // Reaching the floor's corners.
    CHECK(p.radius > 42.0f);
}
