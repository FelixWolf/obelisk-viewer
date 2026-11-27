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
        bool attachment = false; // worn (a HUD's too)
        render::PickHit hit;
    };

    // This frame's ray (nothing: the pointer is elsewhere, over the UI say):
    // asks the renderer to pick along it.
    void aim(render::Scene& scene, const std::optional<Ray>& ray);
    // Takes the renderer's answer, if one came back.
    void update(render::Renderer& renderer, const world::World& world, const ObjectView& objects);
    // What it's on as seen (the hand cursor, the tooltip: the official
    // hover's pick).
    const std::optional<Hover>& hover() const { return hover_; }
    // What a press would touch: as seen, or what the transparent pick hits
    // (every face, whatever its alpha), as the official left click chooses
    // (lltoolpie.cpp handleMouseDown): the transparent one if both are the
    // same prim or either is nothing, if it's worn, or if clicking it does
    // something; else the seen one if it's worn or clickable; else the
    // transparent one. An invisible button in front of what shows is
    // pressed, not what's behind it.
    const std::optional<Hover>& pressed() const { return press_; }

    // The main button: touches what's hovered, if a script listens to it.
    // `originHandle` is the scene's origin region, `agentRegion` the one
    // touch positions are given in.
    void press(session::Session& session, const world::World& world, const ObjectView& objects,
               const AvatarView* avatars, uint64_t originHandle, uint64_t agentRegion);
    // Touches `target` (as seen earlier: the radial menu's "Touch") and lets
    // go at once, as the official pie menu's touch (handle_object_touch).
    void touch(session::Session& session, const world::World& world, const ObjectView& objects,
               const AvatarView* avatars, uint64_t originHandle, uint64_t agentRegion,
               const Hover& target);
    void release(session::Session& session);
    // Once a frame while pressed: where on the touched prim the pointer is
    // now, sent when it changes (ObjectGrabUpdate; off the prim: face -1),
    // as the official viewer's non-physical grab (lltoolgrab.cpp). `now` in
    // nanoseconds.
    void drag(session::Session& session, uint64_t originHandle, uint64_t agentRegion, uint64_t now);

private:
    void grab(session::Session& session, const world::World& world, const ObjectView& objects,
              const AvatarView* avatars, uint64_t originHandle, uint64_t agentRegion,
              const Hover& target);
    uint64_t nextRequest_ = 1;
    uint64_t latestRequest_ = 0;
    bool aimed_ = false;
    std::optional<Hover> hover_;
    std::optional<Hover> transparent_; // the transparent pick's
    std::optional<Hover> press_;
    // Where on the hovered prim the pointer is (positions in the agent's
    // region's coordinates; HUD hits stay in HUD space).
    static session::Session::TouchSurface surfaceOf(const Hover& hover, uint64_t originHandle,
                                                    uint64_t agentRegion);

    struct Grab
    {
        uint64_t region = 0;
        uint32_t localId = 0;
        world::ObjectKey key;
        Uuid object;
        protocol::Vector3 offset{};             // as ObjectGrab's
        protocol::Vector3 position{};           // the first touch's (region coordinates)
        session::Session::TouchSurface surface; // the last sent
        uint64_t sentAt = 0;                    // nanoseconds
    };
    std::optional<Grab> grabbed_;
};

} // namespace viewer
