#pragma once

// Draws avatars with the default body (head, upper and lower body, hair,
// skirt, eyes, eyelashes) in its rest pose, textured with their server-side
// bakes, and tells ObjectView where attachment points are.
//
// Placement as the official viewer (llvoavatar.cpp updateCharacter): the
// avatar's root (where the pelvis is) sits `pelvisToFoot` above the feet,
// and the feet half a body height below the object's position.
//
// Not yet: shape (visual parameters), animation, rigged mesh.

#include "TextureManager.hpp"

#include "viewer/avatar/BaseAvatar.hpp"
#include "viewer/render/Renderer.hpp"
#include "viewer/render/Scene.hpp"
#include "viewer/world/World.hpp"

#include <optional>
#include <string>
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
    glm::mat4 rootFrame(const world::Object& avatar, uint64_t originHandle) const;
    // An attachment point's frame on an avatar; nothing for unknown points.
    std::optional<glm::mat4> attachmentFrame(const world::Object& avatar, int point,
                                             uint64_t originHandle) const;

    void addTo(render::Scene& scene, const world::World& world, uint64_t originHandle,
               TextureManager& textures, float drawDistance);

    size_t drawn() const { return drawn_; }

private:
    avatar::BaseAvatar base_;
    std::vector<glm::mat4> restPose_;
    std::vector<render::Mesh> meshes_; // one per avatar::kBodyParts entry
    std::string appearanceUrl_;
    size_t drawn_ = 0;
};

} // namespace viewer
