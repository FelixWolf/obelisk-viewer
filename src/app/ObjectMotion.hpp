#pragma once

// Where objects are drawn this frame. Simulators send an object's position
// with its velocity and acceleration only now and then (and not at all
// while it moves as predicted); between updates we predict where it is
// (world::extrapolate), as the official viewer does (llviewerobject.cpp):
//   - motion counts from when the update arrived, scaled by the region's
//     time dilation, plus half the circuit's round trip (the update was
//     that old on arrival);
//   - if the region goes quiet (no packets for 2 s), prediction stops 2 s
//     after its last packet, so lag doesn't send objects flying off;
//   - avatars stay above the ground, and nothing is predicted into a
//     region we don't know about.
//
// Unlike the official viewer, a moving object doesn't jump when an update
// disagrees with our prediction: the difference fades out over about a
// tenth of a second (jumps hurt in VR). Larger differences (over 2 m) are
// teleports and jump at once.
//
// The App updates it once per frame; the views read through it, on the
// render thread only (it remembers moving objects between frames).

#include "viewer/session/Session.hpp"
#include "viewer/world/World.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <chrono>
#include <unordered_map>

namespace viewer
{

class ObjectMotion
{
public:
    using Clock = std::chrono::steady_clock;

    // `session` may be null (no network timing then).
    void update(const world::World* world, const session::Session* session, Clock::time_point now);

    world::Pose pose(const world::Object& object) const;
    // translate(position) * rotate(rotation), relative to the parent or region.
    glm::mat4 local(const world::Object& object) const;

    size_t tracked() const { return tracks_.size(); }

private:
    struct RegionTiming
    {
        float halfRoundTrip = 0.0f; // seconds
        Clock::time_point predictUntil = Clock::time_point::max();
    };
    // An update's motion, and when it arrived.
    struct Kinematics
    {
        world::Motion motion{};
        Clock::time_point updatedAt{};
        float timeDilation = 1.0f;
    };
    // A moving object: the update we predicted from before the latest, so
    // a new one's correction can be faded out.
    struct Track
    {
        Kinematics kinematics{};
        glm::vec3 offset{0.0f};                           // shown minus predicted, when it began
        glm::quat rotationOffset{1.0f, 0.0f, 0.0f, 0.0f}; // likewise
        Clock::time_point offsetSince{};
        Clock::time_point used{};
    };

    static Kinematics kinematicsOf(const world::Object& object);
    // Where the update puts the object now, limits applied.
    world::Pose predict(const world::Object& object, const Kinematics& k) const;
    world::Pose shown(const Track& track, const world::Pose& predicted) const;

    const world::World* world_ = nullptr;
    Clock::time_point now_{};
    std::unordered_map<uint64_t, RegionTiming> regions_;
    mutable std::unordered_map<world::ObjectKey, Track, world::ObjectKeyHash> tracks_;
    Clock::time_point lastPrune_{};
};

} // namespace viewer
