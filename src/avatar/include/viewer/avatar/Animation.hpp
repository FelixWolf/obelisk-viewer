#pragma once

// Keyframe animations (.anim assets) and playing them on a skeleton.
// Format, timing and blending follow the official viewer's
// llkeyframemotion.cpp, llmotioncontroller.cpp and llpose.cpp
// (LLJointStateBlender); our own implementation. Constraints (IK) are in
// Constraints.cpp, hand poses follow llhandmotion.cpp and emotes llemote.cpp.

#include "viewer/avatar/BaseAvatar.hpp"
#include "viewer/avatar/Shape.hpp"
#include "viewer/core/Uuid.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

    // Keeps a point on a body part on another one or on the ground, turning
    // the joints between (llkeyframemotion.cpp applyConstraint).
    struct Constraint
    {
        int chainLength = 0; // joints that turn; 0: moves the part's own joint
        bool plane = false;  // only as far as the target's plane
        bool ground = false; // the target
        std::string source;  // collision volumes
        std::string target;
        glm::vec3 sourceOffset{0.0f}; // in units of the volume's scale
        glm::vec3 targetOffset{0.0f};
        float easeInStart = 0.0f;
        float easeInStop = 0.0f;
        float easeOutStart = 0.0f;
        float easeOutStop = 0.0f;
        // Once bound: the volumes, and the chain from the source's joint up
        // (chainLength + 1 joints). Empty: can't be used.
        int sourceJoint = -1;
        int targetJoint = -1;
        std::vector<int> chain;
    };

    int basePriority = 0;
    int handPriority = 0; // the hand pose's
    float duration = 0.0f;
    std::string emote;
    bool loop = false;
    float loopIn = 0.0f;
    float loopOut = 0.0f;
    float easeIn = 0.0f;
    float easeOut = 0.0f;
    uint32_t handPose = 0;
    std::vector<JointCurve> joints;
    std::vector<Constraint> constraints;
    // An emote (no joints): the visual parameter it fades in and out.
    int emoteParam = 0;

    // Resolves joint names (and aliases) against a skeleton.
    void bind(const Skeleton& skeleton);
};

std::expected<Animation, std::string> parseAnimation(std::span<const uint8_t> data);

// The built in emotes (facial expressions), by id or name (any case);
// nullptr for anything else.
std::shared_ptr<const Animation> emoteAnimation(const Uuid& id);
std::shared_ptr<const Animation> emoteAnimation(std::string_view name);

// What constraints need to know of the avatar.
struct ConstraintContext
{
    const BaseAvatar& base;
    const Shape& shape;
    // Fills in what rigged mesh changes of the pose.
    std::function<void(std::vector<JointPose>&)> complete;
    glm::mat4 root{1.0f}; // the avatar's frame in the scene
    uint64_t origin = 0;  // which scene coordinates those are
    // The ground at a scene position; none: no ground.
    struct Ground
    {
        float height = 0.0f;
        glm::vec3 normal{0.0f, 0.0f, 1.0f};
    };
    std::function<std::optional<Ground>(const glm::vec3&)> ground;
};

// A joint curve's value at a time (as LLKeyframeMotion's curves: linear,
// holding the first and last keys).
glm::quat sampleRotation(const Animation::JointCurve& curve, float time);
glm::vec3 samplePosition(const Animation::JointCurve& curve, float time);

// The closed mouth's visual parameter: what emotes fade against.
inline constexpr int kClosedMouthParam = 300;
// Relaxed hands': how they are unless an animation says otherwise.
inline constexpr int kRelaxedHandsParam = 101;

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
    // Without a context, constraints don't apply.
    void evaluate(double now, size_t jointCount, std::vector<JointPose>& pose,
                  const ConstraintContext* context = nullptr);
    // From the last evaluate(): the visual parameters for hand poses and
    // facial expressions, by id (none: zero, all but the closed mouth).
    bool animated() const { return expressed_.has_value(); } // evaluated at all
    const std::map<int, float>& expression() const { return expression_; }
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
        uint64_t order = 0;                           // later starts win ties
        double told = -1.0;                           // when told to stop; < 0: not
        double stop = -1.0;                           // when it starts easing out
        bool ended = false;                           // stopped by itself
        bool done = false;                            // nothing left to show
        std::optional<double> updated = std::nullopt; // last evaluated
        struct ConstraintState
        {
            bool active = false;
            float weight = 1.0f;
            glm::vec3 root{0.0f}; // where the avatar was then
            glm::vec3 normal{0.0f, 0.0f, 1.0f};
            std::optional<glm::vec3> ground; // scene coordinates
            uint64_t origin = 0;             // whose
        };
        std::vector<ConstraintState> constraints = {};

        bool stopped() const { return told >= 0.0; }
    };
    // What an animation gives a joint.
    struct State
    {
        const Playing* from;
        int priority;
        uint64_t order;
        float weight;
        std::optional<glm::quat> rotation;
        std::optional<glm::vec3> position;
    };
    using States = std::vector<std::vector<State>>; // per joint
    void blend(const States& states, std::vector<JointPose>& pose) const;
    // Constraints.cpp: changes what `p` gives its constraint's joints.
    static void constrain(Playing& p, size_t constraint, float time, float seconds,
                          const ConstraintContext& context, std::vector<JointPose>& pose,
                          States& states);
    void express(double now, uint32_t hand, float mouth, const std::map<int, float>& emotes);
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
    std::map<int, float> expression_;
    std::optional<double> expressed_; // when expression_ was
};

} // namespace viewer::avatar
