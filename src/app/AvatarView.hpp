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
#include "MeshManager.hpp"
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

// The avatar an object is attached to (through its link parents), if any.
const world::Object* attachedAvatar(const world::World& world, const world::Object& object);

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

    // Bake on Mesh: if `texture` is one of the placeholders that stand for
    // an avatar's bakes, that bake of this avatar (nullptr while it loads);
    // nothing for other textures.
    std::optional<const render::GpuTexture*> bakeFor(const world::World& world,
                                                     const world::Object& avatar,
                                                     const Uuid& texture,
                                                     TextureManager& textures) const;

    // Joint transforms (relative to the root) as posed this frame.
    const std::vector<glm::mat4>& posedJoints(const world::Object& avatar) const;

    // Reshapes avatars whose appearance changed, advances their animations
    // to `now` (seconds), applies the joint positions their rigged
    // attachments ask for, and draws them.
    void addTo(render::Scene& scene, const world::World& world, uint64_t originHandle,
               TextureManager& textures, AnimationManager& animations, MeshManager& meshes,
               double now, float drawDistance);

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

    // One of the avatar's bakes (texture entry index `slot`), nullptr while
    // it loads or if it has none.
    const render::GpuTexture* bake(const world::World& world, const world::Object& avatar, int slot,
                                   const char* name, TextureManager& textures) const;
    std::shared_ptr<const Body> build(const avatar::ParamWeights& weights) const;
    const Body& bodyOf(const world::Object& avatar) const;
    // Joint position (and scale lock) overrides from rigged attachments, by
    // avatar: per joint, the one from the mesh with the lowest id (as the
    // official viewer picks among conflicting ones).
    struct Override
    {
        Uuid mesh;
        glm::vec3 position{0.0f};
        bool lockScale = false;
    };
    using Overrides = std::unordered_map<int, Override>;
    void collectOverrides(const world::World& world, MeshManager& meshes);

    void animate(Avatar& a, const world::World& world, const Uuid& id, AnimationManager& animations,
                 double now);

    avatar::BaseAvatar base_;
    render::Renderer& renderer_;
    std::shared_ptr<const Body> defaultBody_; // until an avatar's appearance arrives
    std::vector<glm::mat4> defaultJoints_;    // its rest pose
    std::unordered_map<Uuid, Avatar> avatars_;
    std::unordered_map<Uuid, Overrides> overrides_; // rebuilt every frame
    std::string appearanceUrl_;
    uint64_t frame_ = 0;
    size_t drawn_ = 0;
};

} // namespace viewer
