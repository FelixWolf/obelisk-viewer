#include "viewer/avatar/Physics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>

namespace viewer::avatar
{

namespace
{

// llphysicsmotion.cpp's constants.
constexpr float kMaxStep = 0.05f; // seconds
constexpr double kMaxGap = 1.0;   // seconds: longer ones aren't simulated
constexpr float kMaxVelocity = 100.0f;
constexpr float kModelScale = 100.0f;
constexpr float kJointTimeScale = 30.0f;
constexpr float kSmoothing = 3.0f;
constexpr float kRest = 0.5f;
// A joint faster than this teleported (meters a second).
constexpr double kMaxJointSpeed = 250.0;

struct MotionName
{
    std::string_view part;   // "Breast": whose mass, gravity and drag
    std::string_view motion; // "Breast_Physics_UpDown"
    std::string_view joint;
    glm::vec3 direction;
};

const std::array kMotions = {
    MotionName{"Breast", "InOut", "mChest", {-1.0f, 0.0f, 0.0f}},
    MotionName{"Breast", "UpDown", "mChest", {0.0f, 0.0f, 1.0f}},
    MotionName{"Breast", "LeftRight", "mChest", {0.0f, -1.0f, 0.0f}},
    MotionName{"Butt", "UpDown", "mPelvis", {0.0f, 0.0f, -1.0f}},
    MotionName{"Butt", "LeftRight", "mPelvis", {0.0f, -1.0f, 0.0f}},
    MotionName{"Belly", "UpDown", "mPelvis", {0.0f, 0.0f, -1.0f}},
};

} // namespace

PhysicsRig PhysicsRig::find(const BaseAvatar& base)
{
    std::map<std::string_view, const VisualParam*> byName;
    for(const auto& [id, p] : base.params)
    {
        byName.try_emplace(p.name, &p);
    }
    PhysicsRig rig;
    for(const MotionName& name : kMotions)
    {
        const std::string part = std::string(name.part) + "_Physics_";
        const std::string motion = part + std::string(name.motion) + "_";
        bool complete = true;
        auto param = [&](const std::string& paramName)
        {
            const auto it = byName.find(paramName);
            complete = complete && it != byName.end();
            return it != byName.end() ? it->second : nullptr;
        };
        auto id = [&](const std::string& paramName)
        {
            const VisualParam* p = param(paramName);
            return p ? p->id : -1;
        };
        Motion m;
        const int joint = base.skeleton.find(name.joint);
        m.direction = name.direction;
        m.mass = id(part + "Mass");
        m.gravity = id(part + "Gravity");
        m.drag = id(part + "Drag");
        m.spring = id(motion + "Spring");
        m.gain = id(motion + "Gain");
        m.damping = id(motion + "Damping");
        m.maxEffect = id(motion + "Max_Effect");
        if(const VisualParam* controller = param(motion + "Controller"))
        {
            for(const VisualParam::Driven& d : controller->driven)
            {
                if(base.params.contains(d.id))
                {
                    m.driven.push_back(d.id);
                }
            }
        }
        if(complete && joint >= 0)
        {
            m.joint = static_cast<size_t>(joint);
            rig.motions.push_back(std::move(m));
        }
    }
    return rig;
}

void BodyPhysics::reset()
{
    states_.clear();
    updated_.reset();
    weights_.clear();
}

void BodyPhysics::update(const PhysicsRig& rig, const BaseAvatar& base, const ParamWeights& weights,
                         std::span<const glm::mat4> joints, const glm::mat4& root,
                         const glm::dvec3& origin, double now)
{
    if(std::ranges::none_of(rig.motions, [&](const PhysicsRig::Motion& m)
                            { return weights[m.maxEffect] > 0.0f; }))
    {
        reset(); // most avatars: no physics wearable
        return;
    }
    const bool fresh = states_.size() != rig.motions.size() || !updated_;
    states_.resize(rig.motions.size());
    const double elapsed = fresh ? 0.0 : now - *updated_;
    if(!fresh && elapsed <= 0.0)
    {
        return;
    }
    updated_ = now;
    const Sex sex = weights.sex();
    const auto seconds = static_cast<float>(elapsed);

    for(size_t i = 0; i < rig.motions.size(); i++)
    {
        const PhysicsRig::Motion& m = rig.motions[i];
        State& s = states_[i];
        const float maxEffect = weights[m.maxEffect];
        const glm::mat4 joint = root * joints[m.joint];
        const glm::dvec3 position = origin + glm::dvec3(joint[3]);
        const glm::vec3 moved(position - s.jointPosition);
        s.jointPosition = position;

        if(fresh || elapsed > kMaxGap || glm::length(moved) > kMaxJointSpeed * elapsed)
        {
            // Just seen, or not for a while, or teleported: nothing to go by.
            s.jointVelocity = 0.0f;
            s.jointAcceleration = 0.0f;
            continue;
        }
        if(maxEffect <= 0.0f)
        {
            s.position = kRest;
            s.velocity = 0.0f;
            s.jointVelocity = 0.0f;
            s.jointAcceleration = 0.0f;
            for(const int id : m.driven)
            {
                weights_.erase(id);
            }
            continue;
        }

        // How the joint moves along the motion's direction (an axis of the
        // joint, so its scale doesn't turn it).
        const glm::vec3 direction = glm::normalize(glm::mat3(joint) * m.direction);
        const float jointSeconds = seconds * kJointTimeScale;
        const float jointVelocity = glm::dot(moved, direction) * kModelScale / jointSeconds;
        const float jointAcceleration =
            (jointVelocity - s.jointVelocity) / jointSeconds / kSmoothing +
            s.jointAcceleration * (kSmoothing - 1.0f) / kSmoothing;
        s.jointVelocity = jointVelocity;
        s.jointAcceleration = jointAcceleration;

        const float mass = std::max(weights[m.mass], 1e-3f);
        const float push = weights[m.gain] * jointAcceleration * mass +
                           direction.z * weights[m.gravity] * mass +
                           0.5f * weights[m.drag] * jointVelocity * std::fabs(jointVelocity);
        const float spring = weights[m.spring];
        const float damping = weights[m.damping];

        // In small steps, so a low frame rate doesn't overshoot.
        const int steps = static_cast<int>(seconds / kMaxStep) + 1;
        const float step = seconds / static_cast<float>(steps);
        for(int k = 0; k < steps; k++)
        {
            const float force = push - spring * (s.position - kRest) - damping * s.velocity;
            s.velocity = std::clamp(s.velocity + force / mass * step, -kMaxVelocity, kMaxVelocity);
            s.position += s.velocity * step;
            if((s.position < 0.0f && s.velocity < 0.0f) || (s.position > 1.0f && s.velocity > 0.0f))
            {
                s.velocity = 0.0f; // against its limit
            }
            s.position = std::clamp(s.position, 0.0f, 1.0f);
        }
        if(!std::isfinite(s.position) || !std::isfinite(s.velocity) ||
           !std::isfinite(s.jointVelocity) || !std::isfinite(s.jointAcceleration))
        {
            s.position = kRest;
            s.velocity = 0.0f;
            s.jointVelocity = 0.0f;
            s.jointAcceleration = 0.0f;
        }

        for(const int id : m.driven)
        {
            const VisualParam& p = base.params.at(id);
            if((static_cast<uint8_t>(p.sex) & static_cast<uint8_t>(sex)) == 0)
            {
                weights_.erase(id);
                continue;
            }
            const float along = kRest + (s.position - kRest) * maxEffect;
            weights_[id] = std::clamp(p.min + (p.max - p.min) * along, p.min, p.max);
        }
    }
}

void BodyPhysics::moveVolumes(const BaseAvatar& base, std::span<glm::mat4> joints) const
{
    for(const auto& [id, weight] : weights_)
    {
        const VisualParam& p = base.params.at(id);
        if(p.morphs.empty())
        {
            continue;
        }
        // Only moved: none of these parameters resizes one. Volumes have no
        // joints below them.
        for(const VisualParam::BoneDeformation& d : p.volumes)
        {
            const auto joint = static_cast<size_t>(d.joint);
            const int parent = base.skeleton.joints[joint].parent;
            const glm::vec3 offset = weight * d.offset;
            joints[joint][3] += parent < 0
                                    ? glm::vec4(offset, 0.0f)
                                    : joints[static_cast<size_t>(parent)] * glm::vec4(offset, 0.0f);
        }
    }
}

} // namespace viewer::avatar
