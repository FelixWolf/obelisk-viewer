#include "viewer/avatar/BaseAvatar.hpp"

#include <doctest/doctest.h>

#include <algorithm>

using namespace viewer::avatar;

TEST_CASE("Base avatar: skeleton, meshes and attachment points load")
{
    const auto avatar = BaseAvatar::load(VIEWER_CHARACTER_DIR);
    INFO((avatar ? std::string() : avatar.error()));
    REQUIRE(avatar.has_value());
    CHECK(avatar->skeleton.joints.size() == 133 + 26); // bones and collision volumes
    CHECK(avatar->skeleton.joints[static_cast<size_t>(avatar->skeleton.find("BELLY"))].volume);
    CHECK(avatar->skeleton.find("mHead") >= 0);
    CHECK(avatar->pelvisBind.z == doctest::Approx(1.067f));
    CHECK(avatar->pelvisToFoot > 0.8f);
    CHECK(avatar->pelvisToFoot < 1.2f);
    CHECK(avatar->bodyHeight > 1.6f);
    CHECK(avatar->bodyHeight < 2.2f);
    CHECK(avatar->attachmentPoints.size() == 55);
    CHECK(avatar->attachmentPoints.at(32).hud); // Top Right, on the screen
    CHECK(avatar->attachmentPoints.at(32).position == glm::vec3(0.0f, -0.5f, 0.5f));
    // The head's points (skull, nose, jaw, ...) aren't shown in first person.
    CHECK_FALSE(avatar->attachmentPoints.at(2).visibleInFirstPerson);  // Skull
    CHECK_FALSE(avatar->attachmentPoints.at(17).visibleInFirstPerson); // Nose
    CHECK_FALSE(avatar->attachmentPoints.at(47).visibleInFirstPerson); // Jaw
    CHECK(avatar->attachmentPoints.at(1).visibleInFirstPerson);        // Chest
    CHECK(avatar->attachmentPoints.at(6).visibleInFirstPerson);        // Right Hand
    CHECK_FALSE(avatar->attachmentPoints.at(2).hud);
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
    // Priority 2 holds the pelvis from the look-at (priority 1), once in;
    // the head is free.
    CHECK(animator.rotationClaims()[static_cast<size_t>(pelvis)] == doctest::Approx(1.0f));
    CHECK(animator.rotationClaims()[static_cast<size_t>(avatar->skeleton.find("mHead"))] == 0.0f);

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
    // Gone, the joint keeps what they last gave it (an animation deformer's
    // positions stay): b at 1.25 s and a held at its end (stopped, it no
    // longer loops), half each.
    REQUIRE(pose[static_cast<size_t>(pelvis)].rotation.has_value());
    CHECK(angleAboutZ(*pose[static_cast<size_t>(pelvis)].rotation) ==
          doctest::Approx(quarter * (1.0f - 0.625f) / 2.0f).epsilon(0.03));
    REQUIRE(pose[static_cast<size_t>(pelvis)].position.has_value());
    CHECK(pose[static_cast<size_t>(pelvis)].position->z == doctest::Approx(0.1f).epsilon(0.01));
    CHECK_FALSE(pose[static_cast<size_t>(avatar->skeleton.find("mHead"))].rotation.has_value());
    animator.resetHeld();
    animator.evaluate(2.7, avatar->skeleton.joints.size(), pose);
    CHECK_FALSE(pose[static_cast<size_t>(pelvis)].rotation.has_value());
}

TEST_CASE("Animator: layers, restarts, loop ends and one-offs")
{
    const auto avatar = BaseAvatar::load(VIEWER_CHARACTER_DIR);
    REQUIRE(avatar.has_value());
    const size_t joints = avatar->skeleton.joints.size();
    const auto pelvis = static_cast<size_t>(avatar->skeleton.find("mPelvis"));
    const float quarter = 3.14159265f / 2.0f;
    auto make = [&](int32_t priority, float turn)
    {
        auto parsed = parseAnimation(makeAnimation(priority, "mPelvis", turn));
        REQUIRE(parsed.has_value());
        parsed->bind(avatar->skeleton);
        return *parsed;
    };
    auto share = [](const Animation& a) { return std::make_shared<const Animation>(a); };
    const viewer::Uuid a = *viewer::Uuid::parse("00000000-0000-0000-0000-00000000000a");
    const viewer::Uuid b = *viewer::Uuid::parse("00000000-0000-0000-0000-00000000000b");
    std::vector<JointPose> pose;
    auto angle = [&](Animator& animator, double now)
    {
        animator.evaluate(now, joints, pose);
        REQUIRE(pose[pelvis].rotation.has_value());
        return angleAboutZ(*pose[pelvis].rotation) / quarter;
    };

    // Priority 6 stays over a later priority 5; more is 6.
    CHECK(make(9, quarter).basePriority == 6);
    {
        Animator animator;
        animator.start(a, share(make(6, quarter)), 0.0);
        animator.start(b, share(make(5, -quarter)), 0.0);
        CHECK(angle(animator, 1.0) == doctest::Approx(0.5f).epsilon(0.01));
    }
    // Started again in full: plays on. Easing in: a new one.
    {
        Animator animator;
        animator.start(a, share(make(2, quarter)), 0.0);
        animator.start(a, share(make(2, quarter)), 1.0);
        CHECK(animator.count() == 1);
        CHECK(angle(animator, 1.5) == doctest::Approx(0.75f).epsilon(0.01));
        animator.stop(a, 1.6);
        animator.start(a, share(make(2, quarter)), 1.7);
        CHECK(animator.count() == 2);
        CHECK(animator.isPlaying(a));
        animator.stop(a, 1.8);
        CHECK_FALSE(animator.isPlaying(a));
    }
    // Loops, then plays what follows the loop once stopped.
    {
        Animation looping = make(2, quarter);
        looping.loopIn = 0.5f;
        looping.loopOut = 1.0f;
        looping.easeIn = 0.0f;
        Animator animator;
        animator.start(a, share(looping), 0.0);
        CHECK(angle(animator, 1.75) == doctest::Approx(0.375f).epsilon(0.02)); // at 0.75
        animator.stop(a, 1.75); // eases out from 2.5 (at 1.5) to 3.0 (at 2.0)
        CHECK(angle(animator, 2.5) == doctest::Approx(0.75f).epsilon(0.02));
        CHECK(animator.rotationClaims()[pelvis] == doctest::Approx(1.0f));
        CHECK(angle(animator, 2.75) == doctest::Approx(0.875f).epsilon(0.02));
        CHECK(animator.rotationClaims()[pelvis] == doctest::Approx(0.5f));
        animator.evaluate(3.1, joints, pose);
        CHECK(animator.count() == 0);
    }
    // A one-off: told of once when it ends, and its last moment isn't missed.
    {
        Animation once = make(2, quarter);
        once.loop = false;
        once.easeIn = 0.0f;
        once.easeOut = 0.0f;
        Animator animator;
        animator.start(a, share(once), 0.0);
        animator.evaluate(0.5, joints, pose);
        CHECK(animator.takeEnded().empty());
        CHECK(angle(animator, 2.5) == doctest::Approx(1.0f).epsilon(0.01));
        CHECK(animator.takeEnded() == std::vector{a});
        CHECK(animator.takeEnded().empty());
        animator.evaluate(2.6, joints, pose);
        CHECK(animator.count() == 0);
        CHECK(animator.takeEnded().empty());
        animator.start(a, share(once), 3.0); // again
        CHECK(angle(animator, 4.0) == doctest::Approx(0.5f).epsilon(0.01));
    }
}

TEST_CASE("Shape: the body's size follows its joints, and hover the slider")
{
    const auto avatar = BaseAvatar::load(VIEWER_CHARACTER_DIR);
    REQUIRE(avatar.has_value());
    // Hover is the last of the transmitted values: 55 of 100 is 0.2 m, and
    // the middle none.
    std::vector<uint8_t> values(avatar->transmitted.size(), 127);
    REQUIRE(avatar->transmitted.back() == 11001);
    const auto hover = [&](uint8_t value)
    {
        values.back() = value;
        return shapeSkeleton(*avatar, ParamWeights(*avatar, values)).hover;
    };
    CHECK(hover(255) == doctest::Approx(2.0f));
    CHECK(hover(140) == doctest::Approx(0.2f).epsilon(0.03));
    CHECK(hover(127) == 0.0f);

    const Shape shape = shapeSkeleton(*avatar, ParamWeights(*avatar));
    const BodySize size = bodySize(*avatar, shape);
    CHECK(size.height == doctest::Approx(shape.bodyHeight));
    // Longer shins (a rigged mesh's or a deformer's): the feet are further
    // from the pelvis, and it's that much taller.
    std::vector<JointPose> pose(avatar->skeleton.joints.size());
    const auto ankle = static_cast<size_t>(avatar->skeleton.find("mAnkleLeft"));
    pose[ankle].position = shape.jointPositions[ankle] - glm::vec3(0.0f, 0.0f, 0.4f);
    const BodySize longer = bodySize(*avatar, shape, pose);
    CHECK(longer.pelvisToFoot > size.pelvisToFoot + 0.3f);
    CHECK(longer.height - size.height == doctest::Approx(longer.pelvisToFoot - size.pelvisToFoot));
}

TEST_CASE("Animator: resetting the skeleton")
{
    const auto avatar = BaseAvatar::load(VIEWER_CHARACTER_DIR);
    REQUIRE(avatar.has_value());
    const size_t joints = avatar->skeleton.joints.size();
    const auto pelvis = static_cast<size_t>(avatar->skeleton.find("mPelvis"));
    auto parsed = parseAnimation(makeAnimation(2, "mPelvis", 1.0f));
    REQUIRE(parsed.has_value());
    parsed->bind(avatar->skeleton);
    const auto shared = std::make_shared<const Animation>(*parsed);
    const viewer::Uuid a = *viewer::Uuid::parse("00000000-0000-0000-0000-00000000000a");
    const viewer::Uuid b = *viewer::Uuid::parse("00000000-0000-0000-0000-00000000000b");
    Animator animator;
    std::vector<JointPose> pose;
    // Others': what's easing out goes, the rest plays on.
    animator.start(a, shared, 0.0);
    animator.start(b, shared, 0.0);
    animator.evaluate(1.0, joints, pose);
    animator.stop(b, 1.0);
    animator.resetHeld();
    animator.flush();
    CHECK(animator.count() == 1);
    CHECK(animator.isPlaying(a));
    animator.evaluate(1.1, joints, pose);
    CHECK(pose[pelvis].rotation.has_value());
    // Ours: all but those kept, at once.
    animator.start(b, shared, 1.1);
    animator.stopAll(std::vector{b});
    CHECK(animator.isPlaying(b));
    CHECK_FALSE(animator.isPlaying(a));
    animator.stopAll({});
    animator.resetHeld();
    animator.evaluate(1.2, joints, pose);
    CHECK_FALSE(pose[pelvis].rotation.has_value());
    CHECK_FALSE(pose[pelvis].position.has_value());
}

TEST_CASE("Animator: constraints reach their targets")
{
    const auto avatar = BaseAvatar::load(VIEWER_CHARACTER_DIR);
    REQUIRE(avatar.has_value());
    const Skeleton& skeleton = avatar->skeleton;
    const Shape shape = shapeSkeleton(*avatar, ParamWeights(*avatar));
    const viewer::Uuid id = *viewer::Uuid::parse("00000000-0000-0000-0000-00000000000a");

    Animation a;
    a.loop = true;
    a.duration = 1.0f;
    a.loopOut = 1.0f;
    a.basePriority = 3;
    auto curve = [&](const char* name, const glm::quat& rotation)
    { a.joints.push_back({name, -1, 3, {{0.0f, rotation}}, {}}); };
    const glm::vec3 up(0.0f, 0.0f, 1.0f);
    curve("mWristRight", glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    curve("mElbowRight", glm::angleAxis(0.9f, up));
    curve("mShoulderRight", glm::angleAxis(0.3f, up));
    Animation::Constraint c;
    c.chainLength = 2;
    c.source = "R_HAND";
    c.target = "BELLY";
    c.targetOffset = glm::vec3(1.0f, -0.9f, 0.0f);
    c.easeInStop = 0.0f;
    c.easeOutStart = c.easeOutStop = 10.0f;
    a.constraints.push_back(c);
    a.bind(skeleton);
    REQUIRE(a.constraints[0].chain.size() == 3);
    CHECK(a.constraints[0].chain[0] == skeleton.find("mWristRight"));
    CHECK(a.constraints[0].chain[2] == skeleton.find("mShoulderRight"));

    ConstraintContext context{*avatar, shape, {}, glm::mat4(1.0f), 0, {}};
    std::vector<JointPose> pose;
    auto volume = [&](const char* name, const glm::vec3& offset)
    {
        const auto frames = poseFrames(*avatar, shape, pose);
        const JointFrame& f = frames[static_cast<size_t>(skeleton.find(name))];
        return f.position + f.rotation * (f.scale * offset);
    };
    auto distance = [&](Animator& animator, double now, const ConstraintContext* with)
    {
        animator.evaluate(now, skeleton.joints.size(), pose, with);
        return glm::length(volume("R_HAND", {}) - volume("BELLY", c.targetOffset));
    };
    {
        Animator animator;
        animator.start(id, std::make_shared<const Animation>(a), 0.0);
        CHECK(distance(animator, 0.4, nullptr) > 0.1f);
        CHECK(distance(animator, 0.5, &context) < 0.01f);
    }
    // A newer animation of the same priority holding the elbow: left alone.
    {
        Animation over = a;
        over.constraints.clear();
        std::erase_if(over.joints, [](const auto& j) { return j.name != "mElbowRight"; });
        Animator animator;
        animator.start(id, std::make_shared<const Animation>(a), 0.0);
        animator.start(*viewer::Uuid::parse("00000000-0000-0000-0000-00000000000b"),
                       std::make_shared<const Animation>(over), 0.0);
        CHECK(distance(animator, 0.5, &context) > 0.1f);
    }
    // To the ground, 1 m up in the scene where the avatar's frame is: the
    // foot kept where it was once eased in, as the avatar moves.
    {
        Animation down;
        down.loop = true;
        down.duration = down.loopOut = 1.0f;
        const glm::vec3 side(0.0f, 1.0f, 0.0f);
        down.joints.push_back(
            {"mAnkleLeft", -1, 3, {{0.0f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)}}, {}});
        down.joints.push_back({"mKneeLeft", -1, 3, {{0.0f, glm::angleAxis(0.8f, side)}}, {}});
        down.joints.push_back({"mHipLeft", -1, 3, {{0.0f, glm::angleAxis(-0.4f, side)}}, {}});
        Animation::Constraint foot;
        foot.chainLength = 2;
        foot.source = "L_FOOT";
        foot.target = "GROUND";
        foot.ground = true;
        foot.easeInStop = 0.1f;
        foot.easeOutStart = foot.easeOutStop = 10.0f;
        down.constraints.push_back(foot);
        down.bind(skeleton);
        REQUIRE(down.constraints[0].chain.size() == 3);
        Animator animator;
        animator.start(id, std::make_shared<const Animation>(down), 0.0);
        animator.evaluate(0.0, skeleton.joints.size(), pose);
        const float height = volume("L_FOOT", {}).z + 0.1f;
        context.root[3] = glm::vec4(5.0f, 0.0f, 1.0f, 1.0f);
        context.ground = [&](const glm::vec3&)
        { return std::optional(ConstraintContext::Ground{height + 1.0f}); };
        animator.evaluate(0.05, skeleton.joints.size(), pose, &context);
        animator.evaluate(0.09, skeleton.joints.size(), pose, &context);
        animator.evaluate(0.5, skeleton.joints.size(), pose, &context);
        const glm::vec3 at = volume("L_FOOT", {});
        CHECK(at.z == doctest::Approx(height).epsilon(0.01));
        context.root[3].x += 0.05f;
        animator.evaluate(0.6, skeleton.joints.size(), pose, &context);
        CHECK(volume("L_FOOT", {}).x == doctest::Approx(at.x - 0.05f).epsilon(0.1));
        // Moved away: found again where it is now.
        context.root[3].x += 1.0f;
        animator.evaluate(0.7, skeleton.joints.size(), pose, &context);
        CHECK(glm::length(volume("L_FOOT", {}) - at) < 0.02f);
    }
}

TEST_CASE("Animator: hand poses and emotes")
{
    const auto avatar = BaseAvatar::load(VIEWER_CHARACTER_DIR);
    REQUIRE(avatar.has_value());
    const viewer::Uuid id = *viewer::Uuid::parse("00000000-0000-0000-0000-00000000000a");
    std::vector<JointPose> pose;
    auto expression = [&](const Animator& animator, int param)
    {
        const auto it = animator.expression().find(param);
        return it != animator.expression().end() ? it->second : 0.0f;
    };
    Animation fist = *parseAnimation(makeAnimation(2, "mPelvis", 1.0f));
    fist.bind(avatar->skeleton);
    fist.handPose = 3;
    fist.emote = "Express_Smile";
    fist.easeIn = 0.0f;
    const size_t joints = avatar->skeleton.joints.size();

    Animator animator;
    animator.evaluate(0.0, joints, pose);
    CHECK(expression(animator, kRelaxedHandsParam) == 1.0f);
    CHECK(expression(animator, kClosedMouthParam) == 1.0f);
    animator.start(id, std::make_shared<const Animation>(fist), 0.0);
    CHECK(animator.count() == 2); // and its smile
    animator.evaluate(0.1, joints, pose);
    CHECK(expression(animator, 103) == doctest::Approx(0.5f));
    CHECK(expression(animator, kRelaxedHandsParam) == doctest::Approx(0.5f));
    animator.evaluate(1.0, joints, pose);
    CHECK(expression(animator, 103) == 1.0f);
    CHECK(expression(animator, kRelaxedHandsParam) == 0.0f);
    CHECK(expression(animator, 318) == doctest::Approx(1.0f));
    CHECK(expression(animator, kClosedMouthParam) == doctest::Approx(0.0f));
    // The emote is over; a newer animation holding all the joints has the
    // hands.
    Animation over = fist;
    over.handPose = 1;
    over.emote.clear();
    animator.start(*viewer::Uuid::parse("00000000-0000-0000-0000-00000000000b"),
                   std::make_shared<const Animation>(over), 3.0);
    animator.evaluate(3.5, joints, pose);
    animator.evaluate(3.6, joints, pose);
    CHECK(expression(animator, kRelaxedHandsParam) == 1.0f);
    CHECK(expression(animator, 103) == 0.0f);
    CHECK(expression(animator, 318) == 0.0f);
    CHECK(expression(animator, kClosedMouthParam) == 1.0f);

    // Every emote's parameter morphs the head.
    for(const char* name : {"express_smile", "Express_Open_Mouth", "EXPRESS_WORRY_EMOTE"})
    {
        const auto emote = emoteAnimation(std::string_view(name));
        REQUIRE(emote);
        CHECK_FALSE(avatar->params.at(emote->emoteParam).morphs.empty());
    }
    CHECK(
        emoteAnimation(*viewer::Uuid::parse("b7c7c833-e3d3-c4e3-9fc0-131237446312"))->emoteParam ==
        318);
    CHECK_FALSE(emoteAnimation(std::string_view("Closed_Mouth")));
    for(const int hand : {101, 102, 103, 666, 667, 668, 669, 670, 671, 766, 672, 791, 792})
    {
        CHECK_FALSE(avatar->params.at(hand).morphs.empty());
    }
}

TEST_CASE("Shape: morphs that follow clothing go by their bake's morph mask")
{
    const auto avatar = BaseAvatar::load(VIEWER_CHARACTER_DIR);
    REQUIRE(avatar.has_value());
    // avatar_lad.xml <morph_masks>: the loose shirt morph by the upper bake.
    const VisualParam* loose = nullptr;
    for(const auto& [id, p] : avatar->params)
    {
        if(p.name == "Displace_Loose_Upperbody")
        {
            loose = &p;
        }
    }
    REQUIRE(loose);
    CHECK(loose->maskBake == 9);
    CHECK_FALSE(loose->maskInvert);

    ParamWeights weights(*avatar);
    for(const auto& [id, p] : avatar->params)
    {
        weights.set(id, 0.0f);
    }
    const Shape bare = shapeAvatar(*avatar, weights);
    weights.set(loose->id, 1.0f);
    const Shape unmasked = shapeAvatar(*avatar, weights);
    const std::vector<uint8_t> none(4, 0);
    const std::vector<uint8_t> all(4, 255);
    const Shape nothing = shapeAvatar(*avatar, weights, std::vector<MorphMask>{{9, 2, 2, none}});
    const Shape everything = shapeAvatar(*avatar, weights, std::vector<MorphMask>{{9, 2, 2, all}});
    // Another bake's mask doesn't touch it.
    const Shape other = shapeAvatar(*avatar, weights, std::vector<MorphMask>{{10, 2, 2, none}});

    const size_t upper = 2; // kBodyParts: upperBodyMesh
    REQUIRE(std::string(kBodyParts[upper].type) == "upperBodyMesh");
    size_t moved = 0;
    for(size_t v = 0; v < bare.meshes[upper].positions.size(); v++)
    {
        if(unmasked.meshes[upper].positions[v] != bare.meshes[upper].positions[v])
        {
            moved++;
        }
        CHECK(nothing.meshes[upper].positions[v] == bare.meshes[upper].positions[v]);
        CHECK(everything.meshes[upper].positions[v] == unmasked.meshes[upper].positions[v]);
        CHECK(other.meshes[upper].positions[v] == unmasked.meshes[upper].positions[v]);
    }
    CHECK(moved > 0);
}
