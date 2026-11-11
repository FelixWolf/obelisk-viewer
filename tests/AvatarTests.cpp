#include "viewer/avatar/BaseAvatar.hpp"

#include <doctest/doctest.h>

#include <algorithm>

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

#include "viewer/avatar/Shape.hpp"

TEST_CASE("Visual parameters: morphs, transmitted order and defaults")
{
    const auto avatar = BaseAvatar::load(VIEWER_CHARACTER_DIR);
    REQUIRE(avatar.has_value());
    MESSAGE("params: " << avatar->params.size() << ", transmitted: " << avatar->transmitted.size());
    CHECK(avatar->transmitted.size() > 200);
    CHECK(std::is_sorted(avatar->transmitted.begin(), avatar->transmitted.end()));
    CHECK_FALSE(avatar->meshes[0].morphs.empty()); // head
    size_t morphParams = 0;
    size_t boneParams = 0;
    size_t drivers = 0;
    for(const auto& [id, p] : avatar->params)
    {
        morphParams += !p.morphs.empty();
        boneParams += !p.bones.empty();
        drivers += !p.driven.empty();
    }
    MESSAGE("morph params: " << morphParams << ", bone params: " << boneParams
                             << ", drivers: " << drivers);
    CHECK(morphParams > 100);
    CHECK(boneParams > 20);
    CHECK(drivers > 20);

    // The male param (80) is transmitted; a byte of 255 makes it 1.
    std::vector<uint8_t> values(avatar->transmitted.size(), 0);
    const auto male = std::find(avatar->transmitted.begin(), avatar->transmitted.end(), 80);
    REQUIRE(male != avatar->transmitted.end());
    values[static_cast<size_t>(male - avatar->transmitted.begin())] = 255;
    CHECK(ParamWeights(*avatar, values).sex() == Sex::Male);
    CHECK(ParamWeights(*avatar).sex() == Sex::Female);
}

TEST_CASE("Shape: weight-zero skeleton skins meshes back to the rest pose")
{
    const auto avatar = BaseAvatar::load(VIEWER_CHARACTER_DIR);
    REQUIRE(avatar.has_value());

    // With every parameter at 0 nothing deforms, so skinning must only move
    // the meshes down by the pelvis bind position.
    ParamWeights weights(*avatar);
    for(const auto& [id, p] : avatar->params)
    {
        weights.set(id, 0.0f);
    }
    const Shape shape = shapeAvatar(*avatar, weights);
    CHECK(shape.pelvisToFoot == doctest::Approx(avatar->pelvisToFoot));
    CHECK(shape.bodyHeight == doctest::Approx(avatar->bodyHeight));
    for(size_t m = 0; m < avatar->meshes.size(); m++)
    {
        if(avatar->meshes[m].weights.empty())
        {
            continue;
        }
        float worst = 0.0f;
        for(size_t v = 0; v < shape.meshes[m].positions.size(); v++)
        {
            const glm::vec3 expected = avatar->meshes[m].positions[v] - avatar->pelvisBind;
            worst = std::max(worst, glm::length(shape.meshes[m].positions[v] - expected));
        }
        INFO("mesh " << std::string(kBodyParts[m].file));
        CHECK(worst < 2e-3f); // joint positions are rounded, pivots aren't
    }

    // Defaults give a plausible body.
    const Shape defaults = shapeSkeleton(*avatar, ParamWeights(*avatar));
    MESSAGE("default height " << defaults.bodyHeight << ", pelvis to foot "
                              << defaults.pelvisToFoot);
    CHECK(defaults.bodyHeight > 1.5f);
    CHECK(defaults.bodyHeight < 2.3f);
}
