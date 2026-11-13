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

TEST_CASE("Automatic probes: only where the 64 m cell holds more than 128 objects")
{
    const auto flat = [](glm::vec2) { return 0.0f; };
    CHECK(placeAutomaticProbes(cluster(128), flat, 0).empty());

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

    // Big objects don't fit a 32 m cell: they count for the 64 m one only.
    auto objects = cluster(200);
    for(auto& o : objects)
    {
        o.radius = 20.0f;
    }
    CHECK(placeAutomaticProbes(objects, flat, 0).empty());

    // Ids follow the cell.
    CHECK(placeAutomaticProbes(cluster(150), flat, 0)[0].id == probes[0].id);
    CHECK(placeAutomaticProbes(cluster(150), flat, 7)[0].id != probes[0].id);
}
