#include "ImpostorPlanner.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <vector>

using namespace viewer;

namespace
{

Uuid idOf(uint8_t n)
{
    Uuid id;
    id.bytes[15] = n;
    return id;
}

// `count` avatars along +x, 10 m apart from 10 m on, shown small.
std::vector<ImpostorPlanner::Candidate> row(int count, float pixels = 50.0f)
{
    std::vector<ImpostorPlanner::Candidate> c;
    for(int i = 0; i < count; i++)
    {
        c.push_back({idOf(static_cast<uint8_t>(i + 1)),
                     glm::vec3(10.0f + 10.0f * static_cast<float>(i), 0.0f, 1.0f), 1.2f, pixels});
    }
    return c;
}

} // namespace

TEST_CASE("Impostors: past the nearest few, a few pictures a frame")
{
    ImpostorPlanner planner;
    ImpostorPlanner::Options options;
    options.maxFull = 2;
    options.capturesPerFrame = 3;
    const glm::vec3 eye(0.0f, 0.0f, 1.0f);
    const auto avatars = row(8);

    planner.update(avatars, eye, options, 0.0);
    // The two nearest whole; of the other six, the three nearest first.
    CHECK_FALSE(planner.find(idOf(1)));
    CHECK_FALSE(planner.find(idOf(2)));
    for(const uint8_t n : {uint8_t{3}, uint8_t{4}, uint8_t{5}})
    {
        REQUIRE(planner.find(idOf(n)));
        CHECK(planner.find(idOf(n))->capture);
        CHECK(planner.find(idOf(n))->slot >= render::kImpostorBigSlots); // shown small
    }
    CHECK_FALSE(planner.find(idOf(6))); // whole until its picture is taken
    CHECK(planner.all().size() == 3);

    // The next frame: the rest, and the first three keep theirs.
    planner.update(avatars, eye, options, 0.01);
    CHECK(planner.all().size() == 6);
    CHECK_FALSE(planner.find(idOf(3))->capture);
    CHECK(planner.find(idOf(8))->capture);
    // Slots are each one's own.
    std::vector<uint32_t> slots;
    for(const auto& [id, plan] : planner.all())
    {
        slots.push_back(plan.slot);
    }
    std::ranges::sort(slots);
    CHECK(std::ranges::adjacent_find(slots) == slots.end());

    // Pictures grow old: taken again.
    planner.update(avatars, eye, options, 0.02);
    CHECK(std::ranges::none_of(planner.all(), [](const auto& e) { return e.second.capture; }));
    planner.update(avatars, eye, options, 2.0);
    CHECK(std::ranges::count_if(planner.all(), [](const auto& e) { return e.second.capture; }) ==
          3);

    // Everyone whole: no impostors.
    options.maxFull = 0;
    planner.update(avatars, eye, options, 2.1);
    CHECK(planner.all().empty());
}

TEST_CASE("Impostors: not up close, and taken again when the view moves around one")
{
    ImpostorPlanner planner;
    ImpostorPlanner::Options options;
    options.maxFull = 1;
    options.minDistance = 25.0f; // as in a headset
    const glm::vec3 eye(0.0f, 0.0f, 1.0f);
    const auto avatars = row(4); // at 10, 20, 30 and 40 m
    planner.update(avatars, eye, options, 0.0);
    CHECK_FALSE(planner.find(idOf(2))); // past the first, but within 25 m
    REQUIRE(planner.find(idOf(3)));
    REQUIRE(planner.find(idOf(4)));
    planner.update(avatars, eye, options, 0.01);
    CHECK_FALSE(planner.find(idOf(3))->capture);

    // A step sideways: 0.06 radians around the one at 30 m (over 0.05),
    // 0.045 around the one at 40 m.
    const glm::vec3 moved(0.0f, 1.8f, 1.0f);
    planner.update(avatars, moved, options, 0.02);
    CHECK(planner.find(idOf(3))->capture);
    CHECK_FALSE(planner.find(idOf(4))->capture);

    // The avatar walks: its card follows, picture unchanged.
    auto walked = avatars;
    walked[3].center.x += 0.5f;
    planner.update(walked, moved, options, 0.03);
    const auto* plan = planner.find(idOf(4));
    REQUIRE(plan);
    CHECK_FALSE(plan->capture);
    CHECK(plan->card.offset.x == doctest::Approx(0.5f));
}

TEST_CASE("Impostors: the picture frames the sphere, the card faces the eye")
{
    ImpostorPlanner planner;
    ImpostorPlanner::Options options;
    options.maxFull = 1;
    const glm::vec3 eye(0.0f, 0.0f, 1.0f);
    auto avatars = row(2, 400.0f); // shown big
    planner.update(avatars, eye, options, 0.0);
    const auto* plan = planner.find(idOf(2));
    REQUIRE(plan);
    CHECK(plan->slot < render::kImpostorBigSlots);
    const render::Impostor& card = plan->card;
    // The sphere's center is in the middle of the picture; its top and
    // side edges just inside.
    const glm::vec3 c = avatars[1].center;
    auto ndc = [&](const glm::vec3& p)
    {
        const glm::vec4 clip = card.captureViewProjection * glm::vec4(p, 1.0f);
        return glm::vec3(clip) / clip.w;
    };
    CHECK(ndc(c).x == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(ndc(c).y == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(std::abs(ndc(c + glm::vec3(0.0f, 0.0f, 1.2f)).y) < 1.0f);
    CHECK(std::abs(ndc(c + glm::vec3(0.0f, 1.2f, 0.0f)).x) < 1.0f);
    CHECK(ndc(c).z > 0.0f);
    CHECK(ndc(c).z < 1.0f);
    // Upright and square, facing the eye; a card corner is a picture corner.
    CHECK(card.up.z > 0.0f);
    CHECK(glm::dot(card.right, c - eye) == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(glm::length(card.right) == doctest::Approx(glm::length(card.up)));
    const glm::vec3 corner = ndc(card.center + card.right + card.up);
    CHECK(corner.x == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(corner.y == doctest::Approx(1.0f).epsilon(0.001));
}
