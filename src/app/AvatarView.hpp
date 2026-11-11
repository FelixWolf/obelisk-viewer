#pragma once

// Draws avatars with the default body (head, upper and lower body, hair,
// skirt, eyes, eyelashes), shaped by their visual parameters and textured
// with their server-side bakes, and tells ObjectView where attachment
// points are.
//
// Placement as the official viewer (llvoavatar.cpp updateCharacter): the
// avatar's root (where the pelvis is) sits `pelvisToFoot` above the feet,
// and the feet half a body height below the object's position, plus the
// hover offset.
//
// Not yet: animation (bodies are in the rest pose), rigged mesh.

#include "TextureManager.hpp"

#include "viewer/avatar/BaseAvatar.hpp"
#include "viewer/avatar/Shape.hpp"
#include "viewer/core/Uuid.hpp"
#include "viewer/render/Renderer.hpp"
#include "viewer/render/Scene.hpp"
#include "viewer/world/World.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace viewer
{

class AvatarView
{
public:
    AvatarView(avatar::BaseAvatar base, render::Renderer& renderer);

    // Where baked textures come from (the login reply's
    // agent_appearance_service); empty while logged out.
    void setAppearanceServiceUrl(std::string url) { appearanceUrl_ = std::move(url); }

    // The avatar root's frame (no scale), in scene coordinates.
    glm::mat4 rootFrame(const world::Object& avatar, const world::World& world,
                        uint64_t originHandle) const;
    // An attachment point's frame on an avatar; nothing for unknown points.
    std::optional<glm::mat4> attachmentFrame(const world::Object& avatar, const world::World& world,
                                             int point, uint64_t originHandle) const;

    // Shapes avatars whose appearance changed, then draws them.
    void addTo(render::Scene& scene, const world::World& world, uint64_t originHandle,
               TextureManager& textures, float drawDistance);

    size_t drawn() const { return drawn_; }

private:
    struct Body
    {
        avatar::Shape shape;
        std::vector<render::Mesh> meshes; // one per avatar::kBodyParts entry
        uint64_t appearanceSerial = 0;
        uint64_t seen = 0; // frame last drawn or placed
    };

    Body build(const avatar::ParamWeights& weights) const;
    const Body& bodyOf(const world::Object& avatar) const;

    avatar::BaseAvatar base_;
    render::Renderer& renderer_;
    Body defaultBody_; // until an avatar's appearance arrives
    std::unordered_map<Uuid, Body> bodies_;
    std::string appearanceUrl_;
    uint64_t frame_ = 0;
    size_t drawn_ = 0;
};

} // namespace viewer
