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
#include <optional>
#include <span>
#include <string>
#include <utility>
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
    // Starts an animation, unless it's playing in full already. One still
    // easing in or out eases out under the new one
    // (LLMotionController::startMotion).
    void start(const Uuid& id, std::shared_ptr<const Animation> animation, double now);
    // Eases an animation out, after what follows its loop (if anything).
    void stop(const Uuid& id, double now);
    bool isPlaying(const Uuid& id) const;
    size_t count() const { return playing_.size(); }

    // The blended pose at `now`, one entry per joint; drops animations that
    // have finished easing out. A joint no animation moves now keeps the
    // rotation and position the last one gave it, as the official viewer's
    // joints do (LLJointStateBlender starts from the joint as it is and only
    // blends joints some motion moves): animation "deformers", played once
    // and stopped, keep the skeleton as they left it.
    void evaluate(double now, size_t jointCount, std::vector<JointPose>& pose);
    // Forgets what the joints were left with (the official "reset skeleton").
    void resetHeld() { held_.clear(); }
    // The animations that came to their end (didn't loop) at the last
    // evaluate(); told once.
    std::vector<Uuid> takeEnded() { return std::exchange(ended_, {}); }
    // From the last evaluate(), per joint: the rotation weight (0 to 1)
    // animations of at least this priority hold on it, what the official
    // viewer's look-at (a MEDIUM, 1, priority motion, started before any
    // animation so they win ties) can't have.
    static constexpr int kLookAtPriority = 1;
    const std::vector<float>& rotationClaims() const { return claims_; }

private:
    struct Playing
    {
        Uuid id;
        std::shared_ptr<const Animation> animation;
        double start = 0.0;
        uint64_t order = 0;            // later starts win ties
        double told = -1.0;            // when told to stop; < 0: not
        double stop = -1.0;            // when it starts easing out
        bool ended = false;            // stopped by itself
        bool done = false;             // nothing left to show
        std::optional<double> updated; // last evaluated

        bool stopped() const { return told >= 0.0; }
    };
    static bool stopAtEnd(Playing& p, double now);
    static void stopPlaying(Playing& p, double now);
    static float weightAt(const Playing& p, double now);
    static float timeAt(const Playing& p, double now);

    std::vector<Playing> playing_;
    uint64_t nextOrder_ = 0;
    // Per joint: the rotation and position animations last gave it.
    std::vector<JointPose> held_;
    std::vector<float> claims_;
    std::vector<Uuid> ended_;
};

} // namespace viewer::avatar
