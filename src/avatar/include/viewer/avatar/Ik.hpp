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
// (LLEyeMotion, at most 27 degrees). The look-at is a priority 1 motion
// there: it sets the torso's, neck's, head's and eyes' rotations, and gets
// only what animations of priority 1 or more leave of each joint (a
// quadruped's stand holding the torso keeps its front legs still). The
// rest goes on top of the animation, blended in by each goal's weight.

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
    // Any bone reaching for a goal with the `length` joints ending at it
    // (itself included: a wrist's chain of 3 is shoulder, elbow, wrist; 1
    // only turns it, and the pelvis also moves). Solved after the above, in
    // order. The headset's tracking comes as these.
    struct Chain
    {
        int joint = -1;
        int length = 1;
        Effector goal;
    };
    std::vector<Chain> chains;
    // Positions and rotations given in avatar space already, not the scene's.
    bool avatarSpace = false;
};

// The joints IK moves, found by name (-1: missing).
struct IkJoints
{
    int pelvis = -1, torso = -1, chest = -1, neck = -1, head = -1;
    int eyeLeft = -1, eyeRight = -1;
    // Shoulder, elbow, wrist; hip, knee, ankle.
    std::array<int, 3> leftArm{-1, -1, -1};
    std::array<int, 3> rightArm{-1, -1, -1};
    std::array<int, 3> leftLeg{-1, -1, -1};
    std::array<int, 3> rightLeg{-1, -1, -1};
    static IkJoints find(const Skeleton& skeleton);
};

// The pole a chain bends towards (avatar space): an elbow down, back and
// out from the shoulder, a knee forward and down from the hip; none for
// others.
std::optional<glm::vec3> chainPole(const IkJoints& joints, std::span<const int> chain,
                                   std::span<const JointFrame> frames);

// The goals applied to an animated pose (one entry per joint; local
// rotations are set on the joints they move). `claims`: per joint, the
// rotation weight animations of priority 1 or more hold on it
// (Animator::rotationClaims): the desktop look-at only gets what's left,
// as the official one (a priority 1 motion) does; none: all of it.
void applyIk(const BaseAvatar& base, const Shape& shape, const IkJoints& joints,
             const IkGoals& goals, std::vector<JointPose>& pose,
             std::span<const float> claims = {});

} // namespace viewer::avatar
