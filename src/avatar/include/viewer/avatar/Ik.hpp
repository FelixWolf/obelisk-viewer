#pragma once

// Inverse kinematics for our own avatar's viewer-made motion: the head
// following the cursor (desktop) or the headset (VR), and hands reaching
// for the cursor (desktop) or following the controllers (VR). Only this
// viewer sees it: nothing is sent to the simulator.
//
// Hands (and the VR head, up the spine) use FABRIK (Aristidou and Lasenby,
// "Forward And Backward Reaching Inverse Kinematics"): the chain's joints
// are moved to reach the target keeping their bones' lengths, a pole
// bending the middle the natural way, then turned into rotations. The
// desktop head turns as the official viewer's look-at (llheadrotmotion.cpp
// LLHeadRotMotion): towards the point, at most 72 degrees, the torso
// taking 35% of it and the neck and head the rest, and the eyes follow
// (LLEyeMotion, at most 27 degrees). All on top of the animation: blended
// in by each goal's weight.

#include "viewer/avatar/BaseAvatar.hpp"
#include "viewer/avatar/Shape.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <optional>
#include <span>
#include <vector>

namespace viewer::avatar
{

// FABRIK: moves `points` (a chain; the first stays where it is) so the last
// reaches `target`, keeping the distances between neighbours. Out of reach,
// the chain points straight at it. With a pole, the middle joints bend
// towards it. Returns the end's distance from the target.
float fabrik(std::span<glm::vec3> points, const glm::vec3& target,
             const std::optional<glm::vec3>& pole = std::nullopt, int iterations = 16,
             float tolerance = 1e-4f);

// What the avatar's head and hands should do, in avatar space (relative to
// the avatar's root, as poseJoints: +X forward, +Y left, +Z up).
struct IkGoals
{
    struct Effector
    {
        glm::vec3 position{0.0f};
        std::optional<glm::quat> rotation; // the joint's, in avatar space; none: as animated
        float weight = 1.0f;               // 0: the animation, 1: the goal
    };
    // Desktop: the head looks at this point (unless `head` is set).
    std::optional<glm::vec3> lookAt;
    float lookAtWeight = 1.0f;
    // VR: the head where the headset is (the spine reaching for it).
    std::optional<Effector> head;
    std::optional<Effector> leftHand;
    std::optional<Effector> rightHand;
};

// The joints IK moves, found by name (-1: missing).
struct IkJoints
{
    int pelvis = -1, torso = -1, chest = -1, neck = -1, head = -1;
    int eyeLeft = -1, eyeRight = -1;
    // Shoulder, elbow, wrist.
    std::array<int, 3> leftArm{-1, -1, -1};
    std::array<int, 3> rightArm{-1, -1, -1};
    static IkJoints find(const Skeleton& skeleton);
};

// The goals applied to an animated pose (one entry per joint; local
// rotations are set on the joints they move).
void applyIk(const BaseAvatar& base, const Shape& shape, const IkJoints& joints,
             const IkGoals& goals, std::vector<JointPose>& pose);

} // namespace viewer::avatar
