#include "viewer/avatar/BaseAvatar.hpp"
#include "viewer/avatar/Ik.hpp"
#include "viewer/avatar/Shape.hpp"

#include <doctest/doctest.h>

#include <vector>

using namespace viewer::avatar;

TEST_CASE("IK: FABRIK reaches what it can, keeping the bones' lengths")
{
    std::vector<glm::vec3> chain{{0, 0, 0}, {1, 0, 0}, {2, 0, 0}};
    CHECK(fabrik(chain, glm::vec3(1.0f, 1.0f, 0.0f)) < 1e-3f);
    CHECK(chain[0] == glm::vec3(0.0f)); // the root stays
    CHECK(glm::length(chain[1] - chain[0]) == doctest::Approx(1.0f).epsilon(1e-3));
    CHECK(glm::length(chain[2] - chain[1]) == doctest::Approx(1.0f).epsilon(1e-3));

    // Out of reach: straight at it.
    std::vector<glm::vec3> far{{0, 0, 0}, {1, 0, 0}, {2, 0, 0}};
    fabrik(far, glm::vec3(0.0f, 10.0f, 0.0f));
    CHECK(far[2].y == doctest::Approx(2.0f));
    CHECK(far[1].y == doctest::Approx(1.0f));

    // A pole decides which way the middle bends.
    for(const float side : {1.0f, -1.0f})
    {
        std::vector<glm::vec3> arm{{0, 0, 0}, {1, 0, 0}, {2, 0, 0}};
        CHECK(fabrik(arm, glm::vec3(1.5f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, side)) < 1e-3f);
        CHECK(arm[1].z * side > 0.1f);
    }
}

namespace
{

struct Rig
{
    std::unique_ptr<BaseAvatar> base;
    Shape shape;
    IkJoints joints;
};

Rig rig()
{
    auto loaded = BaseAvatar::load(VIEWER_CHARACTER_DIR);
    REQUIRE(loaded);
    Rig r{std::make_unique<BaseAvatar>(std::move(*loaded)), {}, {}};
    r.shape = shapeSkeleton(*r.base, ParamWeights(*r.base));
    r.joints = IkJoints::find(r.base->skeleton);
    return r;
}

glm::vec3 at(const Rig& r, const std::vector<JointPose>& pose, int joint)
{
    return poseFrames(*r.base, r.shape, pose)[static_cast<size_t>(joint)].position;
}

} // namespace

TEST_CASE("IK: a hand reaches for a point, the elbow bending down; weight eases it")
{
    const Rig r = rig();
    REQUIRE(r.joints.rightArm[2] >= 0);
    std::vector<JointPose> rest(r.base->skeleton.joints.size());
    const glm::vec3 shoulder = at(r, rest, r.joints.rightArm[0]);
    const glm::vec3 wrist = at(r, rest, r.joints.rightArm[2]);
    // In front of the right shoulder, within reach.
    const glm::vec3 target = shoulder + glm::vec3(0.3f, -0.1f, 0.0f);

    std::vector<JointPose> pose = rest;
    IkGoals goals;
    goals.rightHand = IkGoals::Effector{target, std::nullopt, 1.0f};
    applyIk(*r.base, r.shape, r.joints, goals, pose);
    CHECK(glm::length(at(r, pose, r.joints.rightArm[2]) - target) < 0.01f);
    CHECK(at(r, pose, r.joints.rightArm[1]).z < shoulder.z); // the elbow below the shoulder
    CHECK(glm::length(at(r, pose, r.joints.rightArm[0]) - shoulder) < 1e-4f);
    // The left arm is as it was.
    CHECK(glm::length(at(r, pose, r.joints.leftArm[2]) - at(r, rest, r.joints.leftArm[2])) < 1e-5f);

    // Half weight: somewhere between.
    std::vector<JointPose> half = rest;
    goals.rightHand->weight = 0.5f;
    applyIk(*r.base, r.shape, r.joints, goals, half);
    const glm::vec3 between = at(r, half, r.joints.rightArm[2]);
    CHECK(glm::length(between - target) > 0.01f);
    CHECK(glm::length(between - wrist) > 0.01f);

    // A rotation for the hand (VR): the wrist takes it.
    std::vector<JointPose> turned = rest;
    const glm::quat palm = glm::angleAxis(0.5f, glm::vec3(0.0f, 0.0f, 1.0f));
    goals.rightHand = IkGoals::Effector{target, palm, 1.0f};
    applyIk(*r.base, r.shape, r.joints, goals, turned);
    const glm::quat wristWorld =
        poseFrames(*r.base, r.shape, turned)[static_cast<size_t>(r.joints.rightArm[2])].rotation;
    CHECK(std::abs(glm::dot(wristWorld, palm)) > 0.9999f);
}

TEST_CASE("IK: the head looks at a point, up to a limit; the eyes follow")
{
    const Rig r = rig();
    std::vector<JointPose> rest(r.base->skeleton.joints.size());
    const glm::vec3 head = at(r, rest, r.joints.head);
    auto facing = [&](const std::vector<JointPose>& pose, int joint)
    {
        return poseFrames(*r.base, r.shape, pose)[static_cast<size_t>(joint)].rotation *
               glm::vec3(1.0f, 0.0f, 0.0f);
    };

    // To the left and a little down: the head turns that way, the torso
    // some of it.
    std::vector<JointPose> pose = rest;
    IkGoals goals;
    goals.lookAt = head + glm::vec3(2.0f, 2.0f, -0.5f);
    applyIk(*r.base, r.shape, r.joints, goals, pose);
    const glm::vec3 look = facing(pose, r.joints.head);
    CHECK(glm::dot(look, glm::normalize(glm::vec3(2.0f, 2.0f, -0.5f))) > 0.99f);
    const glm::vec3 torso = facing(pose, r.joints.torso);
    CHECK(torso.y > 0.05f);
    CHECK(torso.y < look.y);
    CHECK(facing(pose, r.joints.eyeLeft).y > 0.5f);

    // Straight behind: no more than 72 degrees round.
    std::vector<JointPose> behind = rest;
    goals.lookAt = head + glm::vec3(-3.0f, 0.5f, 0.0f);
    applyIk(*r.base, r.shape, r.joints, goals, behind);
    const glm::vec3 turned = facing(behind, r.joints.head);
    CHECK(std::acos(std::clamp(turned.x, -1.0f, 1.0f)) <= glm::radians(72.0f) + 0.01f);

    // Too near (inside the head): nothing.
    std::vector<JointPose> near = rest;
    goals.lookAt = head + glm::vec3(0.1f, 0.0f, 0.0f);
    applyIk(*r.base, r.shape, r.joints, goals, near);
    CHECK(glm::length(facing(near, r.joints.head) - facing(rest, r.joints.head)) < 1e-5f);
}

TEST_CASE("IK: in VR the spine reaches for the headset, the head taking its rotation")
{
    const Rig r = rig();
    std::vector<JointPose> rest(r.base->skeleton.joints.size());
    const glm::vec3 head = at(r, rest, r.joints.head);
    std::vector<JointPose> pose = rest;
    IkGoals goals;
    const glm::quat turn = glm::angleAxis(0.4f, glm::vec3(0.0f, 0.0f, 1.0f));
    goals.head = IkGoals::Effector{head + glm::vec3(0.1f, 0.05f, -0.05f), turn, 1.0f};
    applyIk(*r.base, r.shape, r.joints, goals, pose);
    CHECK(glm::length(at(r, pose, r.joints.head) - goals.head->position) < 0.01f);
    const glm::quat headWorld =
        poseFrames(*r.base, r.shape, pose)[static_cast<size_t>(r.joints.head)].rotation;
    CHECK(std::abs(glm::dot(headWorld, turn)) > 0.9999f);
}

TEST_CASE("IK: bone numbers as the official viewer's, and chain goals by them")
{
    const Rig r = rig();
    const Skeleton& s = r.base->skeleton;
    // Bones in avatar_skeleton.xml's order from the pelvis, volumes left out.
    CHECK(s.boneNumber(s.find("mPelvis")) == 0);
    CHECK(s.boneNumber(s.find("PELVIS")) == -1); // a collision volume
    for(const char* name : {"mHead", "mWristLeft", "mAnkleRight", "mFaceRoot"})
    {
        const int joint = s.find(name);
        REQUIRE(joint >= 0);
        CHECK(s.jointOfBone(s.boneNumber(joint)) == joint);
    }

    // A wrist's chain of 3 (shoulder, elbow, wrist) reaching, the elbow
    // bending down (the pole), the wrist taking the rotation.
    std::vector<JointPose> rest(s.joints.size());
    const int wrist = r.joints.leftArm[2];
    const glm::vec3 shoulder = at(r, rest, r.joints.leftArm[0]);
    IkGoals goals;
    goals.avatarSpace = true;
    const glm::quat turn = glm::angleAxis(0.5f, glm::vec3(1.0f, 0.0f, 0.0f));
    goals.chains.push_back({wrist, 3, {shoulder + glm::vec3(0.3f, 0.25f, -0.1f), turn, 1.0f}});
    std::vector<JointPose> pose = rest;
    applyIk(*r.base, r.shape, r.joints, goals, pose);
    CHECK(glm::length(at(r, pose, wrist) - goals.chains[0].goal.position) < 0.01f);
    CHECK(at(r, pose, r.joints.leftArm[1]).z < shoulder.z);
    const glm::quat wristWorld =
        poseFrames(*r.base, r.shape, pose)[static_cast<size_t>(wrist)].rotation;
    CHECK(std::abs(glm::dot(wristWorld, turn)) > 0.9999f);
    // The collar above the chain stays as it was.
    const int collar = s.joints[static_cast<size_t>(r.joints.leftArm[0])].parent;
    CHECK(!pose[static_cast<size_t>(collar)].rotation);
}
