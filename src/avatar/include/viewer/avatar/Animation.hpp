#pragma once

// Keyframe animations (.anim assets) and playing them on a skeleton.
// Format, timing and blending follow the official viewer's
// llkeyframemotion.cpp, llmotioncontroller.cpp and llpose.cpp
// (LLJointStateBlender); our own implementation. Constraints (IK) and hand
// poses are read past but not used yet.

#include "viewer/avatar/BaseAvatar.hpp"
#include "viewer/avatar/Shape.hpp"
#include "viewer/core/Uuid.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace viewer::avatar
{

struct Animation
{
    struct RotationKey
    {
        float time = 0.0f;
        glm::quat value{1.0f, 0.0f, 0.0f, 0.0f};
    };
    struct PositionKey
    {
        float time = 0.0f;
        glm::vec3 value{0.0f};
    };
    struct JointCurve
    {
        std::string name;
        int joint = -1;                     // in the skeleton, once bound
        int priority = 0;                   // the animation's, unless the joint has its own
        std::vector<RotationKey> rotations; // sorted by time
        std::vector<PositionKey> positions; // sorted by time
    };

    int basePriority = 0;
    float duration = 0.0f;
    std::string emote;
    bool loop = false;
    float loopIn = 0.0f;
    float loopOut = 0.0f;
    float easeIn = 0.0f;
    float easeOut = 0.0f;
    uint32_t handPose = 0;
    std::vector<JointCurve> joints;

    // Resolves joint names (and aliases) against a skeleton.
    void bind(const Skeleton& skeleton);
};

std::expected<Animation, std::string> parseAnimation(std::span<const uint8_t> data);

// A joint curve's value at a time (as LLKeyframeMotion's curves: linear,
// holding the first and last keys).
glm::quat sampleRotation(const Animation::JointCurve& curve, float time);
glm::vec3 samplePosition(const Animation::JointCurve& curve, float time);

// The animations playing on one avatar, blended into a pose.
class Animator
{
public:
    // Starts an animation unless it's already playing (a stopped one that
    // is still easing out starts over).
    void start(const Uuid& id, std::shared_ptr<const Animation> animation, double now);
    // Eases an animation out.
    void stop(const Uuid& id, double now);
    bool isPlaying(const Uuid& id) const;
    size_t count() const { return playing_.size(); }

    // The blended pose at `now`, one entry per joint; drops animations that
    // have finished easing out.
    void evaluate(double now, size_t jointCount, std::vector<JointPose>& pose);

private:
    struct Playing
    {
        Uuid id;
        std::shared_ptr<const Animation> animation;
        double start = 0.0;
        double stop = -1.0; // < 0: not stopped
        uint64_t order = 0; // later starts win ties
    };

    std::vector<Playing> playing_;
    uint64_t nextOrder_ = 0;
};

} // namespace viewer::avatar
