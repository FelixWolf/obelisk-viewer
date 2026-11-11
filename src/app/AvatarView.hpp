#pragma once

// Draws avatars with the default body (head, upper and lower body, hair,
// skirt, eyes, eyelashes), shaped by their visual parameters, posed by the
// animations they play and textured with their server-side bakes; and tells
// ObjectView where attachment points are.
//
// Each avatar's body is morphed on the CPU when its appearance changes and
// skinned on the GPU every frame to its animated skeleton.
//
// Placement as the official viewer (llvoavatar.cpp updateCharacter): the
// avatar's root (where the pelvis is) sits `pelvisToFoot` above the feet,
// and the feet half a body height below the object's position, plus the
// hover offset.
//
// Not yet: rigged mesh, the official viewer's procedural motions (head and
// eye movement, breathing, hand poses).

#include "AnimationManager.hpp"
#include "TextureManager.hpp"

#include "viewer/avatar/Animation.hpp"
#include "viewer/avatar/BaseAvatar.hpp"
#include "viewer/avatar/Shape.hpp"
#include "viewer/core/Uuid.hpp"
#include "viewer/render/Renderer.hpp"
#include "viewer/render/Scene.hpp"
#include "viewer/world/World.hpp"

#include <array>
#include <memory>
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

    const avatar::BaseAvatar& base() const { return base_; }

    // Where baked textures come from (the login reply's
    // agent_appearance_service); empty while logged out.
    void setAppearanceServiceUrl(std::string url) { appearanceUrl_ = std::move(url); }

    // The avatar root's frame (no scale), in scene coordinates.
    glm::mat4 rootFrame(const world::Object& avatar, const world::World& world,
                        uint64_t originHandle) const;
    // An attachment point's frame on an avatar (as posed this frame);
    // nothing for unknown points.
    std::optional<glm::mat4> attachmentFrame(const world::Object& avatar, const world::World& world,
                                             int point, uint64_t originHandle) const;

    // Reshapes avatars whose appearance changed, advances their animations
    // to `now` (seconds) and draws them.
    void addTo(render::Scene& scene, const world::World& world, uint64_t originHandle,
               TextureManager& textures, AnimationManager& animations, double now,
               float drawDistance);

    size_t drawn() const { return drawn_; }

private:
    // A shaped body: shared by every avatar with the same appearance (all
    // without one use the default).
    struct Body
    {
        avatar::Shape shape;
        std::vector<render::Mesh> meshes; // one per avatar::kBodyParts entry
    };
    struct Avatar
    {
        std::shared_ptr<const Body> body;
        uint64_t appearanceSerial = 0;
        avatar::Animator animator;
        std::unordered_map<Uuid, int32_t> started; // animation -> sequence
        std::vector<avatar::JointPose> pose;
        std::vector<glm::mat4> joints; // posed this frame, relative to the root
        std::array<std::vector<glm::mat4>, std::size(avatar::kBodyParts)> palettes;
        uint64_t seen = 0; // frame last updated
    };

    std::shared_ptr<const Body> build(const avatar::ParamWeights& weights) const;
    const Body& bodyOf(const world::Object& avatar) const;
    void animate(Avatar& a, const world::World& world, const Uuid& id, AnimationManager& animations,
                 double now);

    avatar::BaseAvatar base_;
    render::Renderer& renderer_;
    std::shared_ptr<const Body> defaultBody_; // until an avatar's appearance arrives
    std::vector<glm::mat4> defaultJoints_;    // its rest pose
    std::unordered_map<Uuid, Avatar> avatars_;
    std::string appearanceUrl_;
    uint64_t frame_ = 0;
    size_t drawn_ = 0;
};

} // namespace viewer
