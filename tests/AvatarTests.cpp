#include "viewer/avatar/BaseAvatar.hpp"

#include <doctest/doctest.h>

using namespace viewer::avatar;

TEST_CASE("Base avatar: skeleton, meshes and attachment points load")
{
    const auto avatar = BaseAvatar::load(VIEWER_CHARACTER_DIR);
    INFO((avatar ? std::string() : avatar.error()));
    REQUIRE(avatar.has_value());
    CHECK(avatar->skeleton.joints.size() == 133);
    CHECK(avatar->skeleton.find("mHead") >= 0);
    CHECK(avatar->pelvisBind.z == doctest::Approx(1.067f));
    CHECK(avatar->pelvisToFoot > 0.8f);
    CHECK(avatar->pelvisToFoot < 1.2f);
    CHECK(avatar->bodyHeight > 1.6f);
    CHECK(avatar->bodyHeight < 2.2f);
    CHECK(avatar->attachmentPoints.size() == 47);       // 55 minus the 8 HUD points
    CHECK(avatar->attachmentPoints.at(40).joint == -1); // Avatar Center: the root
    CHECK(avatar->attachmentPoints.at(2).name == "Skull");
    REQUIRE(avatar->meshes.size() == std::size(kBodyParts));
    CHECK(avatar->meshes[2].positions.size() == 2211); // upper body
    CHECK_FALSE(avatar->meshes[2].jointNames.empty());
    CHECK(avatar->meshes[6].weights.empty()); // eyes are rigid

    // In the rest pose, the head is about pelvis-to-head above the pelvis.
    const auto pose = avatar->skeleton.restPose();
    const glm::vec3 head(pose[static_cast<size_t>(avatar->skeleton.find("mHead"))][3]);
    CHECK(head.z > 0.5f);
    CHECK(head.z < 0.8f);
}

TEST_CASE("Attachment points from object state")
{
    CHECK(attachmentPointFromState(0x20) == 2);
    CHECK(attachmentPointFromState(0x01) == 16);
    CHECK(attachmentPointFromState(0x13) == 49);
}
