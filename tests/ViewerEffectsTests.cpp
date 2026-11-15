#include "ViewerEffects.hpp"

#include <doctest/doctest.h>

#include <vector>

using viewer::ViewerEffects;
using Effect = viewer::session::Session::AttentionEffect;

TEST_CASE("Viewer effects: sent when they change, throttled, kept alive and cleared")
{
    ViewerEffects effects;
    std::vector<Effect> sent;
    const ViewerEffects::Send send = [&](const Effect& e) { sent.push_back(e); };
    const viewer::Uuid me = viewer::Uuid::random();
    auto look = [&](double x)
    { return ViewerEffects::Target{ViewerEffects::kLookFreeLook, me, {x, 0.0, 0.0}}; };

    effects.update(send, look(2.0), std::nullopt, 0.0);
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].effectType == viewer::session::Session::kLookAtEffect);
    CHECK(sent[0].targetType == ViewerEffects::kLookFreeLook);
    CHECK(sent[0].duration == doctest::Approx(2.0f));
    const viewer::Uuid id = sent[0].id;

    effects.update(send, look(2.01), std::nullopt, 0.1); // hardly moved
    effects.update(send, look(2.5), std::nullopt, 0.2);  // moved, but too soon
    CHECK(sent.size() == 1);
    effects.update(send, look(2.5), std::nullopt, 0.3); // a quarter second on
    REQUIRE(sent.size() == 2);
    CHECK(sent[1].id == id);                            // the same effect, updated
    effects.update(send, look(2.5), std::nullopt, 1.9); // about to run out
    CHECK(sent.size() == 3);

    // Pressing: a point-at and a different look; letting go clears both.
    const viewer::Uuid box = viewer::Uuid::random();
    ViewerEffects::Target grab{ViewerEffects::kPointGrab, box, {0.1, 0.2, 0.3}};
    ViewerEffects::Target select{ViewerEffects::kLookSelect, box, {0.1, 0.2, 0.3}};
    effects.update(send, select, grab, 2.0);
    REQUIRE(sent.size() == 5);
    CHECK(sent[3].targetType == ViewerEffects::kLookSelect);
    CHECK(sent[4].effectType == viewer::session::Session::kPointAtEffect);
    CHECK(sent[4].target == box);
    CHECK(sent[4].duration > 1e30f); // for ever
    effects.update(send, std::nullopt, std::nullopt, 3.0);
    REQUIRE(sent.size() == 7);
    // Cleared as the official viewer clears: "none", no target or position.
    CHECK(sent[5].targetType == ViewerEffects::kNone);
    CHECK(sent[5].target.isNull());
    CHECK(sent[5].position == std::array<double, 3>{0.0, 0.0, 0.0});
    CHECK(sent[5].duration == 0.0f);
    CHECK(sent[6].targetType == ViewerEffects::kNone);
    CHECK(sent[6].duration == 0.0f);
    effects.update(send, std::nullopt, std::nullopt, 4.0); // cleared once
    CHECK(sent.size() == 7);
}

TEST_CASE("Viewer effects: others' look-at and point-at unpack as they're packed")
{
    const viewer::Uuid them = viewer::Uuid::random();
    Effect e;
    e.target = viewer::Uuid::random();
    e.targetType = ViewerEffects::kLookFreeLook;
    e.position = {1.25, -0.5, 2.0};
    const auto data = viewer::session::Session::packAttention(them, e);
    const auto unpacked = ViewerEffects::unpack(data);
    REQUIRE(unpacked);
    CHECK(unpacked->first == them);
    CHECK(unpacked->second.object == e.target);
    CHECK(unpacked->second.type == ViewerEffects::kLookFreeLook);
    CHECK(unpacked->second.position == e.position);
    CHECK_FALSE(ViewerEffects::unpack(std::span(data).first(56))); // the wrong size
}
