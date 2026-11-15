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
// Body parts whose bake an attachment shows (Bake on Mesh) are hidden, as
// llvoavatar.cpp updateMeshVisibility does.
//
// Animated objects get a skeleton of their own (the default shape, no
// body), posed by their animations; their rigged mesh is skinned to it.
//
// Not yet: the official viewer's procedural motions (head and eye movement,
// breathing, hand poses).

#include "AnimationManager.hpp"
#include "MeshManager.hpp"
#include "ObjectMotion.hpp"
#include "TextureManager.hpp"

#include "viewer/avatar/Animation.hpp"
#include "viewer/avatar/BaseAvatar.hpp"
#include "viewer/avatar/Ik.hpp"
#include "viewer/avatar/Shape.hpp"
#include "viewer/core/Uuid.hpp"
#include "viewer/render/Renderer.hpp"
#include "viewer/render/Scene.hpp"
#include "viewer/world/World.hpp"

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace viewer
{

// The avatar an object is attached to (through its link parents), if any.
const world::Object* attachedAvatar(const world::World& world, const world::Object& object);

// Whose skeleton an object's rigged mesh follows: its animated link set's
// root (worn or not), else the avatar it's attached to; and its link set's
// root (the prim attached to the avatar, if worn).
struct SkeletonOwner
{
    const world::Object* owner = nullptr;
    const world::Object* root = nullptr;
    bool animated = false; // owner is an animated object, not an avatar
};
SkeletonOwner skeletonOwner(const world::World& world, const world::Object& object);

class AvatarView
{
public:
    // `motion` (where avatars are this frame) must outlive the view.
    AvatarView(avatar::BaseAvatar base, render::Renderer& renderer, const ObjectMotion& motion);

    const avatar::BaseAvatar& base() const { return base_; }

    // Viewer-made motion for an avatar (ours, and others' look-at and
    // point-at): IK goals in scene coordinates (positions, and rotations
    // for VR), applied on top of its animation from the next pose; none:
    // just the animation.
    void setIk(const Uuid& avatar, std::optional<avatar::IkGoals> goals);
    // First person for an avatar (ours; none: nobody): its head (and every
    // joint below it) scaled to nothing from the next pose, which shrinks
    // the body's head and rigged mesh on it alike, and the attachments on
    // points not visible in first person hidden.
    void setFirstPerson(std::optional<Uuid> avatar) { firstPerson_ = avatar; }
    bool hiddenInFirstPerson(const world::Object& avatar, int point) const;
    bool anyFirstPerson() const { return firstPerson_.has_value(); }
    // Between the avatar's eyes as posed (before the head shrinks), scene
    // coordinates: where a first person camera goes. Steady: where they'd
    // be on the animated pelvis without the rest of the animation or IK
    // moving the head (for a headset, which moves the view itself: the
    // head's sway, or its IK following the view, would shake it).
    std::optional<glm::vec3> eyePosition(const world::Object& avatar, const world::World& world,
                                         uint64_t originHandle, bool steady = false) const;
    // Unanimated, from its head joint to between its eyes (avatar axes):
    // where to put the head for the eyes to be somewhere.
    std::optional<glm::vec3> eyesFromHead(const world::Object& avatar) const;
    // Unanimated, how far ahead of the pelvis the eyes are (the steady
    // eyePosition is straight above it).
    float eyesForward(const world::Object& avatar) const;
    // A joint's position as posed (scene coordinates); nothing for an
    // avatar not posed yet or an unknown joint.
    std::optional<glm::vec3> jointPosition(const world::Object& avatar, const world::World& world,
                                           std::string_view joint, uint64_t originHandle) const;

    // Where baked textures come from (the login reply's
    // agent_appearance_service); empty while logged out.
    void setAppearanceServiceUrl(std::string url) { appearanceUrl_ = std::move(url); }

    // The avatar root's frame (no scale), in scene coordinates.
    glm::mat4 rootFrame(const world::Object& avatar, const world::World& world,
                        uint64_t originHandle) const;
    // An attachment point's frame on an avatar (as posed this frame);
    // nothing for unknown points. HUD points' frames are in HUD space
    // (render::Hud), not the scene's.
    std::optional<glm::mat4> attachmentFrame(const world::Object& avatar, const world::World& world,
                                             int point, uint64_t originHandle) const;
    bool isHudPoint(int point) const;
    // HUD points spread sideways with the HUD's width over its height, as
    // the official mScreen joint is scaled (1, aspect, 1).
    void setHudAspect(float aspect) { hudAspect_ = aspect; }

    // Bake on Mesh: if `texture` is one of the placeholders that stand for
    // an avatar's bakes, that bake of this avatar (nullptr while it loads
    // or if it has none); nothing for other textures. Any of a worn face's
    // maps may be one: `linear` samples it as plain data (a normal or
    // metallic-roughness map). `texels`: how many across it's shown at
    // (TextureManager::texelsFor).
    std::optional<const render::GpuTexture*> bakeFor(const world::World& world,
                                                     const world::Object& avatar,
                                                     const Uuid& texture, TextureManager& textures,
                                                     float texels, bool linear = false) const;
    // The bake a placeholder stands for (its own id, for what its alpha
    // is), or nothing.
    std::optional<Uuid> bakeId(const world::World& world, const world::Object& avatar,
                               const Uuid& texture) const;

    // Joint transforms (relative to the root) as posed this frame.
    const std::vector<glm::mat4>& posedJoints(const world::Object& avatar) const;

    // A rigged mesh's skin resolved against the skeleton, once per skin:
    // per skin joint, the skeleton's joint (-1 if it has none) and the
    // inverse bind matrix times the bind shape matrix. A skin joint's
    // matrix this frame is then posedJoints()[joint] * bind.
    struct SkinBinding
    {
        std::vector<int> joints;
        std::vector<glm::mat4> bind;
    };
    const SkinBinding& binding(const std::shared_ptr<const assets::MeshSkin>& skin) const;
    // How large the avatar is as posed this frame (the diagonal of its
    // joints' box), for choosing its rigged mesh's level of detail.
    float extent(const world::Object& avatar) const;

    // Reshapes avatars whose appearance changed, advances their animations
    // to `now` (seconds), applies the joint positions their rigged
    // attachments ask for, and draws them.
    void addTo(render::Scene& scene, const world::World& world, uint64_t originHandle,
               TextureManager& textures, AnimationManager& animations, MeshManager& meshes,
               double now, float drawDistance);

    // Forgets every avatar (a session ended).
    void clear();

    size_t drawn() const { return drawn_; }
    size_t animatedObjects() const { return animatedObjects_; }

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
        float extent = 0.0f;
        glm::vec3 eyes{0.0f};         // between the eyes, avatar space (before first person)
        glm::vec3 steadyEyes{0.0f};   // as eyePosition's steady ones, avatar space
        glm::vec3 eyesFromHead{0.0f}; // unanimated: between the eyes, from mHead
        float eyesForward = 0.0f;     // unanimated: how far the eyes are ahead of the pelvis
        bool hasEyes = false;
        uint64_t seen = 0; // frame last updated
    };

    // One of the avatar's bakes (texture entry index `slot`), nullptr while
    // it loads or if it has none.
    const render::GpuTexture* bake(const world::World& world, const world::Object& avatar, int slot,
                                   const char* name, TextureManager& textures, float texels,
                                   bool linear = false) const;
    std::shared_ptr<const Body> build(const avatar::ParamWeights& weights) const;
    const Body& bodyOf(const world::Object& avatar) const;
    // Joint position (and scale lock) overrides from rigged attachments, by
    // avatar: per joint, the one from the mesh with the highest id (as the
    // official viewer picks among conflicting ones); and the pelvis offset
    // that raises the whole avatar, chosen the same way.
    struct Override
    {
        Uuid mesh;
        glm::vec3 position{0.0f};
        bool lockScale = false;
    };
    struct Overrides
    {
        std::unordered_map<int, Override> joints;
        Uuid pelvisOffsetMesh;
        float pelvisOffset = 0.0f;
    };
    // What each avatar's attachments change about it: joint positions, and
    // the bakes they use (Bake on Mesh), whose body parts are then hidden.
    void collectAttachments(const world::World& world, MeshManager& meshes);

    void animate(Avatar& a, const world::World& world, const Uuid& id, AnimationManager& animations,
                 double now);
    // Animates and poses a skeleton (an avatar's or an animated object's).
    // `root` is the avatar's frame (rootFrame), for IK goals.
    void pose(Avatar& a, const avatar::Shape& shape, const world::World& world, const Uuid& id,
              AnimationManager& animations, double now, const glm::mat4& root);

    avatar::BaseAvatar base_;
    avatar::IkJoints ikJoints_;
    std::unordered_map<Uuid, avatar::IkGoals> ikGoals_; // scene coordinates
    std::optional<Uuid> firstPerson_;
    std::vector<int> headJoints_; // mHead and below
    render::Renderer& renderer_;
    const ObjectMotion& motion_;
    std::shared_ptr<const Body> defaultBody_; // until an avatar's appearance arrives
    std::vector<glm::mat4> defaultJoints_;    // its rest pose
    std::unordered_map<Uuid, Avatar> avatars_;
    std::unordered_map<Uuid, Overrides> overrides_; // by skeleton owner
    // What overrides_ and bakesInUse_ were collected from: redone when the
    // world's objects or the mesh skins change.
    std::optional<std::pair<uint64_t, uint64_t>> collectedAt_;
    std::chrono::steady_clock::time_point collectedTime_{};
    static constexpr auto kCollectInterval = std::chrono::milliseconds(250);
    std::unordered_map<Uuid, uint64_t> bakesInUse_; // bit per texture entry index, by avatar
    // By skin; holds the skin, so its address isn't reused.
    mutable std::unordered_map<const assets::MeshSkin*,
                               std::pair<std::shared_ptr<const assets::MeshSkin>, SkinBinding>>
        bindings_;
    std::string appearanceUrl_;
    float hudAspect_ = 1.0f;
    // Avatars out of sight are posed once in this many frames.
    static constexpr uint64_t kUnseenPosePeriod = 8;
    // This frame's avatar root and attachment point frames (once posed),
    // kept: every attachment asks for its avatar's.
    struct PointKey
    {
        Uuid avatar;
        int point = 0;
        bool operator==(const PointKey&) const = default;
    };
    struct PointKeyHash
    {
        size_t operator()(const PointKey& k) const
        {
            return std::hash<Uuid>()(k.avatar) ^
                   (static_cast<size_t>(k.point) * 0x9E3779B97F4A7C15ull);
        }
    };
    mutable std::unordered_map<Uuid, glm::mat4> rootFrames_;
    mutable std::unordered_map<PointKey, std::optional<glm::mat4>, PointKeyHash> attachmentFrames_;
    glm::mat4 computeRootFrame(const world::Object& avatar, const world::World& world,
                               uint64_t originHandle) const;
    std::optional<glm::mat4> computeAttachmentFrame(const world::Object& avatar,
                                                    const world::World& world, int point,
                                                    uint64_t originHandle) const;
    uint64_t frame_ = 0;
    size_t drawn_ = 0;
    size_t animatedObjects_ = 0; // skeletons posed for animated objects last frame
};

} // namespace viewer
