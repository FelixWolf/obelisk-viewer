#include "viewer/environment/DayCycle.hpp"
#include "viewer/environment/Sky.hpp"

#include <doctest/doctest.h>

#include <glm/gtc/constants.hpp>

#include <cmath>

using namespace viewer::environment;
namespace llsd = viewer::llsd;

namespace
{

llsd::Value array(std::initializer_list<double> values)
{
    llsd::Value v;
    for(const double x : values)
    {
        v.append(x);
    }
    return v;
}

// A sky whose sun is `elevation` radians above the eastern horizon.
llsd::Value skyFrame(float elevation, double hazeDensity)
{
    // A rotation about -Y lifts +X (east) up towards +Z.
    const glm::quat q = glm::angleAxis(elevation, glm::vec3(0.0f, -1.0f, 0.0f));
    llsd::Value sky;
    sky["type"] = "sky";
    sky["sun_rotation"] = array({q.x, q.y, q.z, q.w});
    sky["moon_rotation"] = array({0.0, 0.0, 0.0, 1.0});
    sky["haze_density"] = 99.0; // overridden by legacy_haze
    sky["legacy_haze"]["haze_density"] = hazeDensity;
    sky["legacy_haze"]["ambient"] = array({0.5, 0.5, 0.5});
    return sky;
}

llsd::Value keyframe(double position, const char* name)
{
    llsd::Value k;
    k["key_keyframe"] = position;
    k["key_name"] = name;
    return k;
}

llsd::Value environment()
{
    llsd::Value env;
    env["day_length"] = 1000;
    env["day_offset"] = 100;
    env["track_altitudes"] = array({1000.0, 2000.0, 3000.0});
    llsd::Value& day = env["day_cycle"];
    day["name"] = "Test day";
    day["frames"]["low"] = skyFrame(0.1f, 0.2);
    day["frames"]["high"] = skyFrame(1.0f, 0.6);
    day["frames"]["orbit"] = skyFrame(-1.0f, 0.0);
    day["frames"]["water"]["type"] = "water";

    llsd::Value water;
    water.append(keyframe(0.0, "water"));
    llsd::Value ground;
    ground.append(keyframe(0.75, "low")); // out of order on purpose
    ground.append(keyframe(0.25, "high"));
    llsd::Value second;
    second.append(keyframe(0.0, "orbit"));
    day["tracks"].append(water);
    day["tracks"].append(ground);
    day["tracks"].append(second);
    day["tracks"].append(llsd::Value(llsd::Array{}));
    day["tracks"].append(llsd::Value(llsd::Array{}));
    return env;
}

} // namespace

TEST_CASE("Day cycles: keyframes, time of day and altitude tracks")
{
    const auto cycle = DayCycle::fromEnvironment(environment());
    REQUIRE(cycle.has_value());
    CHECK(cycle->name == "Test day");
    CHECK(cycle->skies.size() == 3); // water frames aren't skies
    REQUIRE(cycle->tracks[0].size() == 2);
    CHECK(cycle->tracks[0][0].position == 0.25f); // sorted

    // (time + offset) mod length, over length.
    CHECK(cycle->position(400.0) == doctest::Approx(0.5));
    CHECK(cycle->position(1150.0) == doctest::Approx(0.25));

    // On a keyframe: that sky (legacy haze wins over the plain key).
    CHECK(cycle->skyAt(0.25, 0.0f).hazeDensity == doctest::Approx(0.6f));
    // Half way between 0.25 and 0.75.
    CHECK(cycle->skyAt(0.5, 0.0f).hazeDensity == doctest::Approx(0.4f));
    // Wrapping: 0.75 -> 0.25 (next day) is half a day; 0.0 is half way.
    CHECK(cycle->skyAt(0.0, 0.0f).hazeDensity == doctest::Approx(0.4f));
    CHECK(cycle->skyAt(0.9, 0.0f).hazeDensity == doctest::Approx(0.2f + 0.4f * 0.15f / 0.5f));

    // Above 1000 m the second track (which has a keyframe) applies; above
    // 2000 m the empty third falls back to it.
    CHECK(cycle->skyAt(0.5, 1500.0f).hazeDensity == doctest::Approx(0.0f));
    CHECK(cycle->skyAt(0.5, 2500.0f).hazeDensity == doctest::Approx(0.0f));

    // Not usable: no day cycle at all.
    CHECK_FALSE(DayCycle::fromEnvironment(llsd::Value(llsd::Map{})).has_value());
}

TEST_CASE("Sky lighting: sun direction, dimming low on the horizon, night")
{
    const auto cycle = DayCycle::fromEnvironment(environment());
    REQUIRE(cycle.has_value());

    const SkyLighting high = computeLighting(cycle->skies[1]);
    CHECK(high.sunUp);
    CHECK(high.sunDirection.z == doctest::Approx(std::sin(1.0f)));
    CHECK(high.sunDirection.x == doctest::Approx(std::cos(1.0f)));
    CHECK(high.lightDirection == high.sunDirection);

    // Low sun: a longer path through the air, so weaker and redder light.
    SkySettings low = cycle->skies[1];
    low.sunRotation = glm::angleAxis(0.1f, glm::vec3(0.0f, -1.0f, 0.0f));
    const SkyLighting lowLight = computeLighting(low);
    CHECK(lowLight.lightColor.b < high.lightColor.b);
    CHECK(lowLight.lightColor.r / lowLight.lightColor.b > high.lightColor.r / high.lightColor.b);

    // Sun and moon both down: no light from above, no glow.
    SkySettings night = cycle->skies[1];
    night.sunRotation = glm::angleAxis(-0.5f, glm::vec3(0.0f, -1.0f, 0.0f));
    night.moonRotation = night.sunRotation;
    const SkyLighting dark = computeLighting(night);
    CHECK_FALSE(dark.sunUp);
    CHECK(dark.lightDirection.z == doctest::Approx(-1.0f));
    CHECK(dark.lightColor == glm::vec3(0.0f));
    CHECK(dark.sunMoonGlow == 0.0f);
    // Clouds raise the ambient light.
    CHECK(dark.ambient.r > 0.5f);
}

TEST_CASE("Sky settings: probe ambiance tells skies made for PBR from older ones")
{
    const llsd::Value older = skyFrame(1.0f, 0.6);
    CHECK(SkySettings::fromLlsd(older).probeAmbiance == 0.0f);

    llsd::Value pbr = skyFrame(1.0f, 0.6);
    pbr["reflection_probe_ambiance"] = 1.0;
    pbr["gamma"] = 2.0;
    const SkySettings s = SkySettings::fromLlsd(pbr);
    CHECK(s.probeAmbiance == doctest::Approx(1.0f));
    CHECK(s.gamma == doctest::Approx(2.0f));
    CHECK(blend(SkySettings::fromLlsd(older), s, 0.5f).probeAmbiance == doctest::Approx(0.5f));

    // Classic mode: older skies only, by the key being there at all (an
    // ambiance of 0 set in a PBR sky isn't one); a blend takes the target's.
    CHECK(SkySettings::fromLlsd(older).classic);
    CHECK_FALSE(s.classic);
    llsd::Value zero = skyFrame(1.0f, 0.6);
    zero["reflection_probe_ambiance"] = 0.0;
    CHECK_FALSE(SkySettings::fromLlsd(zero).classic);
    CHECK_FALSE(blend(SkySettings::fromLlsd(older), s, 0.2f).classic);
    CHECK(blend(s, SkySettings::fromLlsd(older), 0.8f).classic);
}

TEST_CASE("Water settings: read from a frame, the official defaults otherwise")
{
    llsd::Value frame;
    frame["type"] = "water";
    frame["water_fog_color"] = array({0.1, 0.2, 0.3});
    frame["water_fog_density"] = 4.0;
    frame["underwater_fog_mod"] = 0.5;
    frame["wave1_direction"] = array({0.5, -1.0});
    frame["normal_scale"] = array({1.0, 2.0, 3.0});
    frame["normal_map"] = viewer::Uuid::parse("11111111-2222-3333-4444-555555555555").value();
    const WaterSettings w = WaterSettings::fromLlsd(frame);
    CHECK(w.fogColor == glm::vec3(0.1f, 0.2f, 0.3f));
    CHECK(w.fogDensity == doctest::Approx(4.0f));
    CHECK(w.wave1 == glm::vec2(0.5f, -1.0f));
    CHECK(w.normalScale == glm::vec3(1.0f, 2.0f, 3.0f));
    CHECK(w.normalMap.toString() == "11111111-2222-3333-4444-555555555555");
    CHECK(w.fresnelOffset == doctest::Approx(0.5f)); // default
    CHECK(w.scaleBelow == doctest::Approx(0.2f));
    // Under the water: the density to the fog mod's power.
    CHECK(w.fogDensityFor(false) == doctest::Approx(4.0f));
    CHECK(w.fogDensityFor(true) == doctest::Approx(2.0f));
    WaterSettings negative = w;
    negative.fogDensity = -2.0f; // a fractional power of it isn't a number
    CHECK(negative.fogDensityFor(true) == doctest::Approx(1.0f));
}

TEST_CASE("Water settings: the water track blends its keyframes, normal maps cross-fading")
{
    llsd::Value env = environment();
    llsd::Value& day = env["day_cycle"];
    day["frames"]["clear"]["type"] = "water";
    day["frames"]["clear"]["water_fog_density"] = 1.0;
    day["frames"]["murky"]["type"] = "water";
    day["frames"]["murky"]["water_fog_density"] = 3.0;
    day["frames"]["murky"]["normal_map"] =
        viewer::Uuid::parse("11111111-2222-3333-4444-555555555555").value();
    llsd::Value water;
    water.append(keyframe(0.5, "murky"));
    water.append(keyframe(0.0, "clear"));
    day["tracks"][0] = water;
    const auto cycle = DayCycle::fromEnvironment(env);
    REQUIRE(cycle);
    REQUIRE(cycle->waterTrack.size() == 2);
    const WaterSettings quarter = cycle->waterAt(0.25);
    CHECK(quarter.fogDensity == doctest::Approx(2.0f));
    CHECK(quarter.normalBlend == doctest::Approx(0.5f));
    CHECK(quarter.nextNormalMap.toString() == "11111111-2222-3333-4444-555555555555");
    // Round past the end of the day, back to the first.
    CHECK(cycle->waterAt(0.875).fogDensity == doctest::Approx(1.5f));
    // No water keyframes: the defaults.
    CHECK(DayCycle{}.waterAt(0.3).fogDensity == doctest::Approx(2.0f));
}
