#include "ObjectMotion.hpp"

#include <algorithm>
#include <cmath>

namespace viewer
{

namespace
{

constexpr auto kQuietRegion = std::chrono::seconds(2);
constexpr float kRegionWidth = static_cast<float>(world::kRegionWidth);
constexpr float kCorrectionSeconds = 0.1f; // time constant of the fade
constexpr float kMaxCorrection = 2.0f;     // meters; more is a jump
// A still object updated this recently may have just stopped: its fade
// may still be running.
constexpr auto kRecentUpdate = std::chrono::seconds(1);
// Avatars: how quickly where they're drawn closes on where they are
// (lldrawable.cpp's OBJECT_DAMPING_TIME_CONSTANT, a half-life), and from
// how far they jump instead (MAX_INTERPOLATE_DISTANCE_SQUARED).
constexpr float kAvatarHalfLife = 0.06f; // seconds
constexpr float kAvatarMaxDamped = 10.0f; // meters

bool moving(const world::Object& o)
{
    return o.velocity != glm::vec3(0.0f) || o.acceleration != glm::vec3(0.0f) ||
           o.angularVelocity != glm::vec3(0.0f);
}

} // namespace

void ObjectMotion::update(const world::World* world, const session::Session* session,
                          Clock::time_point now)
{
    world_ = world;
    now_ = now;
    regions_.clear();
    if(!world)
    {
        tracks_.clear();
    }
    if(now - lastPrune_ > std::chrono::seconds(1))
    {
        lastPrune_ = now;
        std::erase_if(tracks_,
                      [&](const auto& t) { return now - t.second.used > std::chrono::seconds(2); });
    }
    if(!session)
    {
        return;
    }
    for(const auto& [circuit, region] : session->regions())
    {
        const auto stats = session->circuitStats(circuit);
        if(!stats)
        {
            continue;
        }
        RegionTiming timing;
        timing.halfRoundTrip = 0.5f * std::chrono::duration<float>(stats->roundTrip).count();
        if(stats->lastReceived != Clock::time_point{} && now - stats->lastReceived > kQuietRegion)
        {
            timing.predictUntil = stats->lastReceived + kQuietRegion;
        }
        regions_[region->handle] = timing;
    }
}

ObjectMotion::Kinematics ObjectMotion::kinematicsOf(const world::Object& o)
{
    return {{{o.position, o.rotation}, o.velocity, o.acceleration, o.angularVelocity},
            o.updatedAt,
            o.timeDilation};
}

world::Pose ObjectMotion::predict(const world::Object& object, const Kinematics& k) const
{
    RegionTiming timing;
    if(const auto it = regions_.find(object.region); it != regions_.end())
    {
        timing = it->second;
    }
    const Clock::time_point until = std::min(now_, timing.predictUntil);
    const float seconds =
        k.timeDilation *
        (std::chrono::duration<float>(until - k.updatedAt).count() + timing.halfRoundTrip);
    world::Pose pose = world::extrapolate(k.motion, object.parentId == 0, seconds);
    if(object.parentId != 0 || !world_)
    {
        return pose;
    }

    // Not into a region we know nothing about: stop at its edge.
    glm::vec3& p = pose.position;
    if(p.x < 0.0f || p.y < 0.0f || p.x >= kRegionWidth || p.y >= kRegionWidth)
    {
        const auto step = [](float v) { return v < 0.0f ? -1 : v >= kRegionWidth ? 1 : 0; };
        const int64_t x = static_cast<int64_t>(object.region >> 32) + step(p.x) * 256;
        const int64_t y = static_cast<int64_t>(object.region & 0xFFFFFFFFu) + step(p.y) * 256;
        const uint64_t next = static_cast<uint64_t>(x) << 32 | static_cast<uint64_t>(y);
        if(x < 0 || y < 0 || !world_->terrain(next))
        {
            p.x = std::clamp(p.x, 0.0f, kRegionWidth - 0.01f);
            p.y = std::clamp(p.y, 0.0f, kRegionWidth - 0.01f);
        }
    }
    // Avatars don't sink into the ground (their position is their middle).
    if(object.type == world::ObjectType::Avatar)
    {
        if(const world::RegionTerrain* terrain = world_->terrain(object.region))
        {
            const float ground = terrain->heightAt(p.x, p.y) + 0.5f * object.scale.z;
            // (Never higher than the simulator itself put it.)
            p.z = std::max(p.z, std::min(ground, k.motion.pose.position.z));
        }
    }
    return pose;
}

world::Pose ObjectMotion::shown(const Track& track, const world::Pose& predicted) const
{
    const float age = std::chrono::duration<float>(now_ - track.offsetSince).count();
    const float fade = std::exp(-std::max(age, 0.0f) / kCorrectionSeconds);
    if(fade < 0.01f)
    {
        return predicted;
    }
    return {
        predicted.position + track.offset * fade,
        glm::normalize(glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), track.rotationOffset, fade) *
                       predicted.rotation)};
}

world::Pose ObjectMotion::damped(const world::Object& avatar) const
{
    const bool recent = moving(avatar) || now_ - avatar.updatedAt <= kRecentUpdate;
    const world::Pose target = recent ? predict(avatar, kinematicsOf(avatar))
                                      : world::Pose{avatar.position, avatar.rotation};
    Track& track = tracks_[world::ObjectKey{avatar.region, avatar.localId}];
    track.used = now_;
    if(track.drawnAt == Clock::time_point{})
    {
        track.drawn = target.position; // first seen
    }
    else if(track.drawnAt != now_) // once a frame
    {
        const float seconds = std::chrono::duration<float>(now_ - track.drawnAt).count();
        if(glm::length(target.position - track.drawn) > kAvatarMaxDamped)
        {
            track.drawn = target.position;
        }
        else
        {
            track.drawn = glm::mix(track.drawn, target.position,
                                   1.0f - std::exp2(-seconds / kAvatarHalfLife));
        }
    }
    track.drawnAt = now_;
    // (Its rotation AvatarView eases itself.)
    return {track.drawn, target.rotation};
}

world::Pose ObjectMotion::pose(const world::Object& object) const
{
    if(object.updatedAt == Clock::time_point{})
    {
        return {object.position, object.rotation};
    }
    if(object.type == world::ObjectType::Avatar && object.parentId == 0)
    {
        return damped(object);
    }
    const bool isMoving = moving(object);
    if(!isMoving && now_ - object.updatedAt > kRecentUpdate)
    {
        return {object.position, object.rotation}; // the common case: it sits still
    }

    const Kinematics latest = kinematicsOf(object);
    const world::Pose predicted = predict(object, latest);
    const world::ObjectKey key{object.region, object.localId};
    auto it = tracks_.find(key);
    if(it == tracks_.end())
    {
        if(!isMoving)
        {
            return predicted;
        }
        it = tracks_.emplace(key, Track{.kinematics = latest}).first;
    }
    Track& track = it->second;
    track.used = now_;
    if(track.kinematics.updatedAt != latest.updatedAt)
    {
        // A new update: start from where the old one had it shown.
        const world::Pose before = shown(track, predict(object, track.kinematics));
        track.offset = before.position - predicted.position;
        track.rotationOffset = glm::normalize(before.rotation * glm::inverse(predicted.rotation));
        if(glm::length(track.offset) > kMaxCorrection)
        {
            track.offset = glm::vec3(0.0f);
            track.rotationOffset = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        }
        track.offsetSince = now_;
        track.kinematics = latest;
    }
    return shown(track, predicted);
}

glm::mat4 ObjectMotion::local(const world::Object& object) const
{
    const world::Pose p = pose(object);
    return glm::translate(glm::mat4(1.0f), p.position) * glm::mat4_cast(p.rotation);
}

} // namespace viewer
