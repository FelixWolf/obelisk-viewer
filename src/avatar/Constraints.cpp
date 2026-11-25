// Animations' constraints: a point on a body part kept on another one, or on
// the ground. What applies, when and how much follows llkeyframemotion.cpp
// applyConstraint; the chain is solved with FABRIK (Ik.hpp) on the same
// frame's pose, where the official viewer runs a spring simulation on the
// last frame's.

#include "viewer/avatar/Animation.hpp"
#include "viewer/avatar/Ik.hpp"

#include <algorithm>
#include <cmath>

namespace viewer::avatar
{

namespace
{

constexpr float kMovedAway = 0.25f; // meters

// The official clamp_rescale to 0 to 1.
float ramp(float time, float start, float stop)
{
    return stop > start ? std::clamp((time - start) / (stop - start), 0.0f, 1.0f)
                        : (time >= stop ? 1.0f : 0.0f);
}

glm::quat shortestArc(const glm::vec3& from, const glm::vec3& to)
{
    const float lengths = glm::length(from) * glm::length(to);
    if(lengths < 1e-12f)
    {
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    const float cosine = glm::dot(from, to) / lengths;
    if(cosine < -0.99999f)
    {
        const glm::vec3 side = std::fabs(from.z) < 0.9f * glm::length(from)
                                   ? glm::vec3(0.0f, 0.0f, 1.0f)
                                   : glm::vec3(1.0f, 0.0f, 0.0f);
        return glm::angleAxis(3.14159265f, glm::normalize(glm::cross(from, side)));
    }
    const glm::vec3 axis = glm::cross(from, to) / lengths;
    return glm::normalize(glm::quat(1.0f + cosine, axis.x, axis.y, axis.z));
}

} // namespace

void Animator::constrain(Playing& p, size_t constraint, float time, float seconds,
                         const ConstraintContext& context, std::vector<JointPose>& pose,
                         States& states)
{
    const Animation::Constraint& c = p.animation->constraints[constraint];
    p.constraints.resize(p.animation->constraints.size());
    Playing::ConstraintState& state = p.constraints[constraint];
    if(time < c.easeInStart)
    {
        return;
    }
    if(time > c.easeOutStop)
    {
        state.active = false;
        return;
    }

    // What the animation gives a joint.
    auto given = [&](int joint) -> State*
    {
        for(State& s : states[static_cast<size_t>(joint)])
        {
            if(s.from == &p)
            {
                return &s;
            }
        }
        return nullptr;
    };
    const Skeleton& skeleton = context.base.skeleton;
    auto parentRotation = [&](const std::vector<JointFrame>& frames, int joint)
    {
        const int parent = skeleton.joints[static_cast<size_t>(joint)].parent;
        return parent < 0 ? glm::quat(1.0f, 0.0f, 0.0f, 0.0f)
                          : frames[static_cast<size_t>(parent)].rotation;
    };
    // A point on a volume.
    auto on = [](const std::vector<JointFrame>& frames, int volume, const glm::vec3& offset)
    {
        const JointFrame& f = frames[static_cast<size_t>(volume)];
        return f.position + f.rotation * (f.scale * offset);
    };

    // The chain as the animation has it, whatever else plays.
    const size_t length = c.chain.size() - 1;
    if(length > 0)
    {
        for(const int joint : c.chain)
        {
            const State* s = given(joint);
            if(!s || !s->rotation)
            {
                return;
            }
            pose[static_cast<size_t>(joint)].rotation = s->rotation;
        }
    }
    std::vector<JointFrame> frames = poseFrames(context.base, context.shape, pose);
    const glm::vec3 source = on(frames, c.sourceJoint, c.sourceOffset);

    // The target. Ground stays where it was found once eased in, unless the
    // avatar is moved away from it.
    const glm::vec3 root(context.root[3]);
    if(!state.active || time < c.easeInStop ||
       (c.ground &&
        (state.origin != context.origin || glm::length(root - state.root) > kMovedAway)))
    {
        if(!state.active || time < c.easeInStop)
        {
            state.weight = 1.0f;
        }
        state.active = true;
        if(c.ground)
        {
            state.ground.reset();
            state.origin = context.origin;
            state.root = root;
            const glm::vec3 at(context.root * glm::vec4(source, 1.0f));
            if(const auto ground = context.ground ? context.ground(at) : std::nullopt)
            {
                state.ground = glm::vec3(at.x, at.y, ground->height) + c.targetOffset;
                state.normal = ground->normal;
            }
        }
    }
    glm::vec3 target(0.0f);
    glm::vec3 normal(0.0f);
    if(c.ground)
    {
        if(!state.ground)
        {
            return;
        }
        const glm::mat4 toAvatar = glm::inverse(context.root);
        target = glm::vec3(toAvatar * glm::vec4(*state.ground, 1.0f));
        normal = glm::vec3(toAvatar * glm::vec4(state.normal, 0.0f));
    }
    else
    {
        target = on(frames, c.targetJoint, c.targetOffset);
        normal = target - frames[static_cast<size_t>(c.targetJoint)].position;
        if(glm::dot(normal, normal) < 1e-12f)
        {
            normal = frames[static_cast<size_t>(c.sourceJoint)].rotation * -c.sourceOffset;
        }
    }
    if(c.plane)
    {
        normal = glm::dot(normal, normal) > 1e-12f ? glm::normalize(normal) : glm::vec3(0.0f);
        target = source + normal * glm::dot(target - source, normal);
    }

    // Less of it out of reach, smoothly.
    auto joint = [&](size_t i) -> const JointFrame&
    { return frames[static_cast<size_t>(c.chain[i])]; };
    bool reached = true;
    if(length > 0)
    {
        float total = glm::length(source - joint(0).position);
        for(size_t i = 0; i < length; i++)
        {
            total += glm::length(joint(i).position - joint(i + 1).position);
        }
        const glm::vec3 away = target - joint(length).position;
        reached = glm::dot(away, away) * 0.95f <= total * total;
    }
    const float halving = reached ? 0.3f : 0.1f;
    state.weight = glm::mix(state.weight, reached ? 1.0f : 0.0f,
                            1.0f - std::exp2(-std::max(seconds, 0.0f) / halving));
    const float weight =
        state.weight * (c.easeOutStop == 0.0f
                            ? 1.0f
                            : std::min(ramp(time, c.easeInStart, c.easeInStop),
                                       1.0f - ramp(time, c.easeOutStart, c.easeOutStop)));
    const glm::vec3 move = (target - source) * weight;
    if(weight <= 0.0f)
    {
        return;
    }

    if(length == 0)
    {
        State* s = given(c.chain[0]);
        if(s && s->position)
        {
            const int parent = skeleton.joints[static_cast<size_t>(c.chain[0])].parent;
            const glm::vec3 scale =
                parent < 0 ? glm::vec3(1.0f) : frames[static_cast<size_t>(parent)].scale;
            *s->position += glm::inverse(parentRotation(frames, c.chain[0])) * move / scale;
        }
        return;
    }

    // The source's joint keeps its rotation, so it moves as the source does.
    // Between, joints start a share of the way along.
    std::vector<glm::vec3> points(length + 1);
    for(size_t i = 0; i <= length; i++)
    {
        points[i] = joint(length - i).position;
    }
    const glm::vec3 end = points.back() + move;
    const glm::quat endRotation = joint(0).rotation;
    // The end and the root in one place: no direction to reach in.
    if(glm::length(end - points.front()) < 1e-4f)
    {
        return;
    }
    fabrik(points, end);

    // The chain's rotations, from the root down.
    for(size_t i = length; i > 0; i--)
    {
        const int j = c.chain[i];
        const glm::vec3 now = joint(i - 1).position - joint(i).position;
        const glm::vec3 wanted = points[length - i + 1] - points[length - i];
        const glm::quat turned = shortestArc(now, wanted) * joint(i).rotation;
        const glm::quat local = glm::normalize(glm::inverse(parentRotation(frames, j)) * turned);
        given(j)->rotation = local;
        pose[static_cast<size_t>(j)].rotation = local;
        frames = poseFrames(context.base, context.shape, pose);
    }
    given(c.chain[0])->rotation =
        glm::normalize(glm::inverse(parentRotation(frames, c.chain[0])) * endRotation);
}

} // namespace viewer::avatar
