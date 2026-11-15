#include "viewer/avatar/Ik.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace viewer::avatar
{

namespace
{

constexpr float kHeadLimit = glm::half_pi<float>() * 0.8f; // HEAD_ROTATION_CONSTRAINT
constexpr float kTorsoShare = 0.35f;                       // TORSO_LAG
constexpr float kNeckShare = 0.5f;                         // NECK_LAG
constexpr float kMinLookDistance = 0.3f;                   // MIN_HEAD_LOOKAT_DISTANCE
constexpr float kEyeLimit = glm::half_pi<float>() * 0.3f;  // EYE_ROT_LIMIT_ANGLE

// The shortest rotation taking direction `from` to `to`.
glm::quat arc(const glm::vec3& from, const glm::vec3& to)
{
    const float lf = glm::length(from);
    const float lt = glm::length(to);
    if(lf < 1e-6f || lt < 1e-6f)
    {
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    const glm::vec3 a = from / lf;
    const glm::vec3 b = to / lt;
    const float c = glm::dot(a, b);
    if(c < -0.99999f)
    {
        // Opposite: half a turn about any perpendicular.
        glm::vec3 axis = glm::cross(glm::vec3(1.0f, 0.0f, 0.0f), a);
        if(glm::dot(axis, axis) < 1e-6f)
        {
            axis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), a);
        }
        return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
    }
    const glm::vec3 axis = glm::cross(a, b);
    return glm::normalize(glm::quat(1.0f + c, axis.x, axis.y, axis.z));
}

// `q` limited to `limit` radians from no rotation.
glm::quat constrain(const glm::quat& q, float limit)
{
    glm::quat r = q.w < 0.0f ? -q : q;
    const float angle = 2.0f * std::acos(std::clamp(r.w, -1.0f, 1.0f));
    if(angle <= limit || angle < 1e-6f)
    {
        return r;
    }
    return glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), r, limit / angle);
}

glm::quat nlerp(const glm::quat& a, const glm::quat& b, float t)
{
    const glm::quat bb = glm::dot(a, b) < 0.0f ? -b : b;
    return glm::normalize(a * (1.0f - t) + bb * t);
}

glm::quat localOf(const std::vector<JointPose>& pose, const Skeleton& skeleton, int joint)
{
    return pose[static_cast<size_t>(joint)].rotation.value_or(
        skeleton.joints[static_cast<size_t>(joint)].rotation);
}

glm::quat parentRotation(const std::vector<JointFrame>& frames, const Skeleton& skeleton, int joint)
{
    const int parent = skeleton.joints[static_cast<size_t>(joint)].parent;
    return parent < 0 ? glm::quat(1.0f, 0.0f, 0.0f, 0.0f)
                      : frames[static_cast<size_t>(parent)].rotation;
}

// Sets a chain's rotations so its joints lie on `targets` (FABRIK's
// result), each turned the least from where the animation had it; the
// last joint gets `endRotation` if given. Blended with the animation by
// `weight`.
void poseChain(const BaseAvatar& base, const Shape& shape, std::span<const int> chain,
               std::span<const glm::vec3> targets, const std::optional<glm::quat>& endRotation,
               float weight, std::vector<JointPose>& pose)
{
    const Skeleton& skeleton = base.skeleton;
    std::vector<glm::quat> animated;
    for(const int j : chain)
    {
        animated.push_back(localOf(pose, skeleton, j));
    }
    for(size_t i = 0; i + 1 < chain.size(); i++)
    {
        // Where the chain is now (the joints before already moved).
        const std::vector<JointFrame> frames = poseFrames(base, shape, pose);
        const JointFrame& joint = frames[static_cast<size_t>(chain[i])];
        const glm::vec3 now = frames[static_cast<size_t>(chain[i + 1])].position - joint.position;
        const glm::vec3 wanted = targets[i + 1] - targets[i];
        const glm::quat world = arc(now, wanted) * joint.rotation;
        pose[static_cast<size_t>(chain[i])].rotation =
            glm::normalize(glm::inverse(parentRotation(frames, skeleton, chain[i])) * world);
    }
    if(endRotation)
    {
        const std::vector<JointFrame> frames = poseFrames(base, shape, pose);
        pose[static_cast<size_t>(chain.back())].rotation = glm::normalize(
            glm::inverse(parentRotation(frames, skeleton, chain.back())) * *endRotation);
    }
    if(weight < 1.0f)
    {
        for(size_t i = 0; i < chain.size(); i++)
        {
            JointPose& p = pose[static_cast<size_t>(chain[i])];
            p.rotation = glm::slerp(animated[i], *p.rotation, std::clamp(weight, 0.0f, 1.0f));
        }
    }
}

// FABRIK on a chain of joints to an effector's position.
void reach(const BaseAvatar& base, const Shape& shape, std::span<const int> chain,
           const IkGoals::Effector& goal, const std::optional<glm::vec3>& pole,
           std::vector<JointPose>& pose)
{
    if(goal.weight <= 0.0f || std::ranges::any_of(chain, [](int j) { return j < 0; }))
    {
        return;
    }
    const std::vector<JointFrame> frames = poseFrames(base, shape, pose);
    std::vector<glm::vec3> points;
    for(const int j : chain)
    {
        points.push_back(frames[static_cast<size_t>(j)].position);
    }
    fabrik(points, goal.position, pole);
    poseChain(base, shape, chain, points, goal.rotation, goal.weight, pose);
}

// The desktop look-at (LLHeadRotMotion, LLEyeMotion).
void lookAt(const BaseAvatar& base, const Shape& shape, const IkJoints& j, const glm::vec3& target,
            float weight, std::vector<JointPose>& pose)
{
    const Skeleton& skeleton = base.skeleton;
    if(weight <= 0.0f || j.torso < 0 || j.chest < 0 || j.neck < 0 || j.head < 0)
    {
        return;
    }
    std::vector<JointFrame> frames = poseFrames(base, shape, pose);
    glm::vec3 look = target - frames[static_cast<size_t>(j.head)].position;
    const float distance = glm::length(look);
    if(distance < kMinLookDistance)
    {
        return;
    }
    look /= distance;
    // The head's rotation looking that way, upright: x forward, y left, z up.
    const glm::vec3 up(0.0f, 0.0f, 1.0f);
    glm::vec3 left = glm::cross(up, look);
    if(glm::dot(left, left) < 0.15f)
    {
        // Nearly straight up or down: towards the front a little.
        look = glm::normalize(glm::mix(look, glm::vec3(1.0f, 0.0f, 0.0f), 0.4f));
        left = glm::cross(up, look);
    }
    left = glm::normalize(left);
    const glm::vec3 headUp = glm::cross(look, left);
    const glm::quat target_ = constrain(glm::quat_cast(glm::mat3(look, left, headUp)), kHeadLimit);
    const glm::quat headWorld = nlerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), target_, weight);

    // The torso a part of the way, then the neck and head the rest
    // (relative to the chest), on top of what's animated.
    JointPose& torso = pose[static_cast<size_t>(j.torso)];
    torso.rotation =
        glm::normalize(nlerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), headWorld, kTorsoShare) *
                       localOf(pose, skeleton, j.torso));
    frames = poseFrames(base, shape, pose);
    const glm::quat rest =
        glm::normalize(glm::inverse(frames[static_cast<size_t>(j.chest)].rotation) * headWorld);
    const glm::quat neckTurn = nlerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), rest, kNeckShare);
    const glm::quat headTurn = nlerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), rest, 1.0f - kNeckShare);
    pose[static_cast<size_t>(j.neck)].rotation =
        glm::normalize(neckTurn * localOf(pose, skeleton, j.neck));
    pose[static_cast<size_t>(j.head)].rotation =
        glm::normalize(headTurn * localOf(pose, skeleton, j.head));

    // The eyes the rest of the way, a little.
    frames = poseFrames(base, shape, pose);
    for(const int eye : {j.eyeLeft, j.eyeRight})
    {
        if(eye < 0)
        {
            continue;
        }
        const JointFrame& e = frames[static_cast<size_t>(eye)];
        const glm::vec3 forward = e.rotation * glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::quat turn = constrain(arc(forward, target - e.position), kEyeLimit);
        const glm::quat world = nlerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), turn, weight) * e.rotation;
        pose[static_cast<size_t>(eye)].rotation =
            glm::normalize(glm::inverse(parentRotation(frames, skeleton, eye)) * world);
    }
}

} // namespace

float fabrik(std::span<glm::vec3> points, const glm::vec3& target,
             const std::optional<glm::vec3>& pole, int iterations, float tolerance)
{
    const size_t n = points.size();
    if(n < 2)
    {
        return n == 1 ? glm::length(points[0] - target) : 0.0f;
    }
    std::vector<float> lengths(n - 1);
    float total = 0.0f;
    for(size_t i = 0; i + 1 < n; i++)
    {
        lengths[i] = glm::length(points[i + 1] - points[i]);
        total += lengths[i];
    }
    const glm::vec3 root = points[0];
    if(glm::length(target - root) >= total)
    {
        // Out of reach: straight at it.
        const glm::vec3 dir = glm::normalize(target - root);
        for(size_t i = 1; i < n; i++)
        {
            points[i] = points[i - 1] + dir * lengths[i - 1];
        }
        return glm::length(points[n - 1] - target);
    }
    // Bend towards the pole first, so the solution settles on that side.
    if(pole && n >= 3)
    {
        const glm::vec3 axis = glm::normalize(target - root);
        for(size_t i = 1; i + 1 < n; i++)
        {
            glm::vec3 side = *pole - root;
            side -= axis * glm::dot(side, axis);
            if(glm::dot(side, side) > 1e-8f)
            {
                points[i] += glm::normalize(side) * lengths[i - 1] * 0.5f;
            }
        }
    }
    for(int k = 0; k < iterations; k++)
    {
        // Backward: the end on the target, each joint towards the next.
        points[n - 1] = target;
        for(size_t i = n - 1; i-- > 0;)
        {
            const glm::vec3 d = points[i] - points[i + 1];
            const float l = glm::length(d);
            points[i] = points[i + 1] + (l > 1e-8f ? d / l : glm::vec3(0.0f)) * lengths[i];
        }
        // Forward: the root back in place, each joint towards the previous.
        points[0] = root;
        for(size_t i = 0; i + 1 < n; i++)
        {
            const glm::vec3 d = points[i + 1] - points[i];
            const float l = glm::length(d);
            points[i + 1] = points[i] + (l > 1e-8f ? d / l : glm::vec3(0.0f)) * lengths[i];
        }
        // Keep the middle joints on the pole's side of the root-to-end line.
        if(pole)
        {
            for(size_t i = 1; i + 1 < n; i++)
            {
                const glm::vec3 a = points[i - 1];
                const glm::vec3 b = points[i + 1];
                const glm::vec3 axis = b - a;
                const float al = glm::length(axis);
                if(al < 1e-6f)
                {
                    continue;
                }
                const glm::vec3 u = axis / al;
                const glm::vec3 center = a + u * glm::dot(points[i] - a, u);
                glm::vec3 now = points[i] - center;
                glm::vec3 want = *pole - center;
                want -= u * glm::dot(want, u);
                const float radius = glm::length(now);
                if(radius > 1e-6f && glm::dot(want, want) > 1e-8f)
                {
                    points[i] = center + glm::normalize(want) * radius;
                }
            }
        }
        if(glm::length(points[n - 1] - target) <= tolerance)
        {
            break;
        }
    }
    return glm::length(points[n - 1] - target);
}

IkJoints IkJoints::find(const Skeleton& skeleton)
{
    IkJoints j;
    j.pelvis = skeleton.find("mPelvis");
    j.torso = skeleton.find("mTorso");
    j.chest = skeleton.find("mChest");
    j.neck = skeleton.find("mNeck");
    j.head = skeleton.find("mHead");
    j.eyeLeft = skeleton.find("mEyeLeft");
    j.eyeRight = skeleton.find("mEyeRight");
    j.leftArm = {skeleton.find("mShoulderLeft"), skeleton.find("mElbowLeft"),
                 skeleton.find("mWristLeft")};
    j.rightArm = {skeleton.find("mShoulderRight"), skeleton.find("mElbowRight"),
                  skeleton.find("mWristRight")};
    return j;
}

void applyIk(const BaseAvatar& base, const Shape& shape, const IkJoints& joints,
             const IkGoals& goals, std::vector<JointPose>& pose)
{
    if(pose.size() != base.skeleton.joints.size())
    {
        pose.resize(base.skeleton.joints.size());
    }
    if(goals.head)
    {
        // VR: the spine reaching for the headset.
        const std::array<int, 4> spine{joints.torso, joints.chest, joints.neck, joints.head};
        reach(base, shape, spine, *goals.head, std::nullopt, pose);
    }
    else if(goals.lookAt)
    {
        lookAt(base, shape, joints, *goals.lookAt, goals.lookAtWeight, pose);
    }
    // Elbows bend down, back and out (avatar space: +X forward, +Y left).
    const std::vector<JointFrame> frames = poseFrames(base, shape, pose);
    auto elbowPole = [&](const std::array<int, 3>& arm, float side)
    { return frames[static_cast<size_t>(arm[0])].position + glm::vec3(-0.3f, side * 0.4f, -1.0f); };
    if(goals.leftHand && joints.leftArm[0] >= 0)
    {
        reach(base, shape, joints.leftArm, *goals.leftHand, elbowPole(joints.leftArm, 1.0f), pose);
    }
    if(goals.rightHand && joints.rightArm[0] >= 0)
    {
        reach(base, shape, joints.rightArm, *goals.rightHand, elbowPole(joints.rightArm, -1.0f),
              pose);
    }
}

} // namespace viewer::avatar
