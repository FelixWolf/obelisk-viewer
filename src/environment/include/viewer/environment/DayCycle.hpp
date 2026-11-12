#pragma once

// A region's (or parcel's) environment: a day cycle of sky keyframes, as
// the ExtEnvironment capability returns it (llenvironment.cpp,
// llsettingsdaycycle.cpp).
//
// A day cycle has five tracks: water (0) and four skies (1 to 4), for
// altitudes from the ground up to track_altitudes. Each track lists
// keyframes at positions 0..1 through the day, naming frames in "frames".
// The position now is (seconds since 1970 + day offset) modulo the day
// length, over the day length; the sky blends between the keyframes on
// either side of it, wrapping around midnight.

#include "viewer/environment/Sky.hpp"
#include "viewer/llsd/Value.hpp"

#include <array>
#include <expected>
#include <string>
#include <vector>

namespace viewer::environment
{

struct Keyframe
{
    float position = 0.0f; // 0..1 through the day
    size_t sky = 0;        // index into DayCycle::skies
};

struct DayCycle
{
    static constexpr size_t kSkyTracks = 4;

    std::string name;
    std::vector<SkySettings> skies;
    // Sky tracks 1..4 (index 0 is track 1), keyframes sorted by position.
    std::array<std::vector<Keyframe>, kSkyTracks> tracks;
    double dayLength = 14400.0; // seconds
    double dayOffset = 57600.0; // seconds
    // Where tracks 2, 3 and 4 begin (meters); track 1 is below the first.
    std::array<float, 3> trackAltitudes{1000.0f, 2000.0f, 3000.0f};

    // The environment map of an ExtEnvironment reply ("environment").
    static std::expected<DayCycle, std::string> fromEnvironment(const llsd::Value& environment);

    // 0..1 through the day at this time (seconds since 1970, UTC).
    double position(double unixSeconds) const;
    // The sky at that position for something at this altitude. A cycle
    // without sky keyframes gives the default sky.
    SkySettings skyAt(double position, float altitude) const;
};

} // namespace viewer::environment
