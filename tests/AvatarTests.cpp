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
    CHECK(avatar->meshes[6].weights.empty());                                // eyes are rigid
    CHECK(avatar->skeleton.find("hip") == avatar->skeleton.find("mPelvis")); // alias
    REQUIRE(avatar->skinJoints.size() == avatar->meshes.size());
    CHECK(avatar->skinJoints[6].size() == 1);
    CHECK(avatar->skinJoints[2].size() <= 255);

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
    const auto joints = poseJoints(*avatar, shape);
    for(size_t m = 0; m < avatar->meshes.size(); m++)
    {
        if(avatar->meshes[m].weights.empty())
        {
            continue;
        }
        const ShapedMesh skinned = skinMesh(*avatar, m, shape.meshes[m], joints);
        float worst = 0.0f;
        for(size_t v = 0; v < skinned.positions.size(); v++)
        {
            const glm::vec3 expected = avatar->meshes[m].positions[v] - avatar->pelvisBind;
            worst = std::max(worst, glm::length(skinned.positions[v] - expected));
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

#include "viewer/avatar/Animation.hpp"

#include <cstring>

namespace
{

// A tiny .anim: one joint turning a quarter turn about Z over 2 seconds,
// with one position key.
std::vector<uint8_t> makeAnimation(int32_t priority, const char* joint, float turn)
{
    std::vector<uint8_t> out;
    auto put = [&](const auto& value)
    {
        const auto* p = reinterpret_cast<const uint8_t*>(&value);
        out.insert(out.end(), p, p + sizeof(value));
    };
    auto putString = [&](const char* s) { out.insert(out.end(), s, s + std::strlen(s) + 1); };
    auto quantize = [](float v, float lo, float hi)
    { return static_cast<uint16_t>(std::lround((v - lo) / (hi - lo) * 65535.0f)); };
    put(uint16_t{1});
    put(uint16_t{0});
    put(priority);
    put(2.0f);        // duration
    putString("");    // emote
    put(0.0f);        // loop in
    put(2.0f);        // loop out
    put(int32_t{1});  // loop
    put(0.5f);        // ease in
    put(0.5f);        // ease out
    put(uint32_t{0}); // hand pose
    put(uint32_t{1}); // joints
    putString(joint);
    put(int32_t{-1}); // the animation's priority
    put(int32_t{2});  // rotation keys
    const float s = std::sin(turn * 0.5f);
    for(const auto& [t, z] : {std::pair{0.0f, 0.0f}, std::pair{2.0f, s}})
    {
        put(quantize(t, 0.0f, 2.0f));
        put(quantize(0.0f, -1.0f, 1.0f));
        put(quantize(0.0f, -1.0f, 1.0f));
        put(quantize(z, -1.0f, 1.0f));
    }
    put(int32_t{1}); // position keys
    put(quantize(0.0f, 0.0f, 2.0f));
    put(quantize(0.0f, -5.0f, 5.0f));
    put(quantize(0.0f, -5.0f, 5.0f));
    put(quantize(0.1f, -5.0f, 5.0f));
    put(int32_t{0}); // constraints
    return out;
}

float angleAboutZ(const glm::quat& q)
{
    return 2.0f * std::atan2(q.z, q.w);
}

} // namespace

TEST_CASE("Animation: parse, sample, ease and blend")
{
    const auto avatar = BaseAvatar::load(VIEWER_CHARACTER_DIR);
    REQUIRE(avatar.has_value());
    const float quarter = 3.14159265f / 2.0f;

    auto parsed = parseAnimation(makeAnimation(2, "hip", quarter));
    INFO((parsed ? std::string() : parsed.error()));
    REQUIRE(parsed.has_value());
    CHECK(parsed->duration == doctest::Approx(2.0f));
    CHECK(parsed->loop);
    CHECK(parsed->basePriority == 2);
    REQUIRE(parsed->joints.size() == 1);
    CHECK(parsed->joints[0].priority == 2);
    parsed->bind(avatar->skeleton);
    const int pelvis = avatar->skeleton.find("mPelvis");
    CHECK(parsed->joints[0].joint == pelvis); // through the "hip" alias
    CHECK(angleAboutZ(sampleRotation(parsed->joints[0], 1.0f)) ==
          doctest::Approx(quarter / 2.0f).epsilon(0.01));
    CHECK(samplePosition(parsed->joints[0], 1.0f).z == doctest::Approx(0.1f).epsilon(0.01));

    CHECK_FALSE(parseAnimation(std::vector<uint8_t>{1, 0}).has_value());

    // A higher priority animation wins the joint outright.
    auto low = std::make_shared<Animation>(*parsed);
    auto high = parseAnimation(makeAnimation(4, "mPelvis", -quarter));
    REQUIRE(high.has_value());
    high->bind(avatar->skeleton);
    auto highShared = std::make_shared<Animation>(std::move(*high));

    Animator animator;
    const viewer::Uuid a = *viewer::Uuid::parse("00000000-0000-0000-0000-00000000000a");
    const viewer::Uuid b = *viewer::Uuid::parse("00000000-0000-0000-0000-00000000000b");
    animator.start(a, low, 0.0);
    std::vector<JointPose> pose;
    animator.evaluate(1.0, avatar->skeleton.joints.size(), pose);
    REQUIRE(pose[static_cast<size_t>(pelvis)].rotation.has_value());
    CHECK(angleAboutZ(*pose[static_cast<size_t>(pelvis)].rotation) ==
          doctest::Approx(quarter / 2.0f).epsilon(0.01));
    CHECK_FALSE(pose[static_cast<size_t>(avatar->skeleton.find("mHead"))].rotation.has_value());

    animator.start(b, highShared, 1.0);
    animator.evaluate(2.0, avatar->skeleton.joints.size(), pose); // b fully in, 1 s along
    CHECK(angleAboutZ(*pose[static_cast<size_t>(pelvis)].rotation) ==
          doctest::Approx(-quarter / 2.0f).epsilon(0.01));

    // Stopped animations ease out, then go.
    animator.stop(b, 2.0);
    animator.stop(a, 2.0);
    CHECK_FALSE(animator.isPlaying(a));
    animator.evaluate(2.25, avatar->skeleton.joints.size(), pose);
    CHECK(animator.count() == 2);
    animator.evaluate(2.6, avatar->skeleton.joints.size(), pose);
    CHECK(animator.count() == 0);
    CHECK_FALSE(pose[static_cast<size_t>(pelvis)].rotation.has_value());
}
