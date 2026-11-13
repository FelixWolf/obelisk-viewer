#pragma once

// The pointer: a ray into the scene (from the mouse now, from a VR
// controller later) and what it's on. Each frame it asks the renderer to
// pick along the ray (Scene::pick); the answer, a frame or two later, says
// which prim and face it's on and where. Pressing the button on something a
// script listens to touches it (ObjectGrab), letting go releases it
// (ObjectDeGrab), as the official viewer's left click (lltoolpie.cpp).

#include "ObjectView.hpp"

#include "viewer/render/Renderer.hpp"
#include "viewer/render/Scene.hpp"
#include "viewer/session/Session.hpp"
#include "viewer/world/World.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>

namespace viewer
{

class PointerController
{
public:
    struct Ray
    {
        glm::vec3 origin{0.0f}; // scene coordinates
        glm::vec3 direction{1.0f, 0.0f, 0.0f};
        // Where it meets the HUD (HUD space Y and Z), if it does.
        std::optional<glm::vec2> hudPoint;
    };

    // What the pointer is on.
    struct Hover
    {
        world::ObjectKey key;
        int face = 0;
        bool hud = false;
        bool touchable = false;  // a script listens for touches
        uint8_t clickAction = 0; // world::ClickAction
        render::PickResult hit;
    };

    // This frame's ray (nothing: the pointer is elsewhere, over the UI say):
    // asks the renderer to pick along it.
    void aim(render::Scene& scene, const std::optional<Ray>& ray);
    // Takes the renderer's answer, if one came back.
    void update(render::Renderer& renderer, const world::World& world, const ObjectView& objects);
    const std::optional<Hover>& hover() const { return hover_; }

    // The main button: touches what's hovered, if a script listens to it.
    // `originHandle` is the scene's origin region, `agentRegion` the one
    // touch positions are given in.
    void press(session::Session& session, const world::World& world, const ObjectView& objects,
               const AvatarView* avatars, uint64_t originHandle, uint64_t agentRegion);
    void release(session::Session& session);

private:
    uint64_t nextRequest_ = 1;
    uint64_t latestRequest_ = 0;
    bool aimed_ = false;
    std::optional<Hover> hover_;
    struct Grab
    {
        uint64_t region = 0;
        uint32_t localId = 0;
        session::Session::TouchSurface surface;
    };
    std::optional<Grab> grabbed_;
};

} // namespace viewer
