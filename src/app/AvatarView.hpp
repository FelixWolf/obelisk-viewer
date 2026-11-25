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
#include <map>
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
    // Unanimated, how high its eyes are above its feet (standing); known
    // in first person only, as the eyes are.
    std::optional<float> eyeHeight(const world::Object& avatar) const;
    // A joint's position as posed (scene coordinates); nothing for an
    // avatar not posed yet or an unknown joint.
    std::optional<glm::vec3> jointPosition(const world::Object& avatar, const world::World& world,
                                           std::string_view joint, uint64_t originHandle) const;

    // Where baked textures come from (the login reply's
    // agent_appearance_service); empty while logged out.
    void setAppearanceServiceUrl(std::string url) { appearanceUrl_ = std::move(url); }

    // Whether an avatar can be placed: not sitting on something that isn't
    // known yet (its seat, or a link parent of it).
    bool placed(const world::Object& avatar, const world::World& world, uint64_t originHandle) const
    {
        return avatar.parentId == 0 || seatFrame(avatar, world, originHandle).has_value();
    }
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
    // First person: the joints as posed, the head not shrunk, for the
    // avatar's shadow (which keeps its head); nullptr for an avatar not in
    // first person.
    const std::vector<glm::mat4>* shadowJoints(const world::Object& avatar) const;

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
        std::vector<render::Mesh> meshes; // one per kBodyParts entry
        avatar::ParamWeights weights;
    };
    // A body part morphed by a hand pose or facial expression.
    struct ExpressedPart
    {
        std::shared_ptr<const Body> body; // made from
        std::map<int, int> weights;       // the parameters, in steps
        render::Mesh mesh;                // none: the body's own
    };
    struct Avatar
    {
        std::shared_ptr<const Body> body;
        uint64_t appearanceSerial = 0;
        // The head, upper and lower bakes' morph masks the body was built
        // with (rebuilt as they arrive).
        std::array<std::shared_ptr<const image::Plane>, 3> masks;
        avatar::Animator animator;
        std::unordered_map<Uuid, int32_t> started; // animation -> sequence
        std::vector<avatar::JointPose> pose;
        std::array<ExpressedPart, std::size(avatar::kBodyParts)> expressed;
        std::vector<glm::mat4> joints; // posed this frame, relative to the root
        std::array<std::vector<glm::mat4>, std::size(avatar::kBodyParts)> palettes;
        // First person: as above, the head not shrunk (for the shadow).
        std::vector<glm::mat4> shadowJoints;
        std::array<std::vector<glm::mat4>, std::size(avatar::kBodyParts)> shadowPalettes;
        float extent = 0.0f;
        glm::vec3 eyes{0.0f};         // between the eyes, avatar space (before first person)
        glm::vec3 steadyEyes{0.0f};   // as eyePosition's steady ones, avatar space
        glm::vec3 eyesFromHead{0.0f}; // unanimated: between the eyes, from mHead
        float eyesForward = 0.0f;     // unanimated: how far the eyes are ahead of the pelvis
        float eyeHeight = 0.0f;       // unanimated: how high the eyes are above the feet
        bool hasEyes = false;
        uint64_t seen = 0; // frame last updated
        world::ObjectKey key;
        // Footsteps: whether each foot was on the ground last frame.
        bool leftDown = false;
        bool rightDown = false;
    };

public:
    // A foot coming down while walking, running or landing
    // (LLVOAvatar::updateFootstepSounds): where (scene coordinates), and on
    // what: an object's physical material, or nothing for land.
    struct Footstep
    {
        Uuid avatar;
        glm::vec3 position{0.0f};
        std::optional<uint8_t> material;
    };
    // What's under a point, looking from `top` down `depth` meters (scene
    // coordinates): the surface's height, and its material (nothing: land);
    // nothing if neither is within reach.
    struct Ground
    {
        float height = 0.0f;
        std::optional<uint8_t> material;
    };
    using GroundBelow = std::function<std::optional<Ground>(const glm::vec3& top, float depth)>;
    // An avatar's animations that came to their end this frame (after
    // addTo): for ours, the simulator is told. Told once.
    std::vector<Uuid> takeEndedAnimations(const Uuid& avatar);
    // The footsteps of avatars drawn this frame (after addTo).
    std::vector<Footstep> footsteps(const world::World& world, uint64_t originHandle,
                                    const GroundBelow& ground);

private:
    // One of the avatar's bakes (texture entry index `slot`), nullptr while
    // it loads or if it has none.
    const render::GpuTexture* bake(const world::World& world, const world::Object& avatar, int slot,
                                   const char* name, TextureManager& textures, float texels,
                                   bool linear = false) const;
    // A bake's morph mask (head, upper or lower), nullptr until it's here or
    // if it has none.
    std::shared_ptr<const image::Plane> bakeMask(const world::World& world,
                                                 const world::Object& avatar, int slot,
                                                 const char* name, TextureManager& textures) const;
    std::shared_ptr<const Body> build(const avatar::ParamWeights& weights,
                                      std::span<const avatar::MorphMask> masks = {}) const;
    // A body part as a hand pose or expression has it, made again when
    // that changes; nullptr: as the body's.
    const render::Mesh* expressedPart(Avatar& a, const std::shared_ptr<const Body>& body,
                                      size_t part);
    render::Mesh partMesh(const avatar::ShapedMesh& shaped, size_t part) const;
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
              AnimationManager& animations, double now, const glm::mat4& root,
              uint64_t originHandle, const world::Object* standing);

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
    std::unordered_map<Uuid, std::vector<Uuid>>
        animatedParts_;                             // animated objects' other prims, by root
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
    // The frame of the prim an avatar sits on, in the scene; nothing if the
    // seat (or one of its link parents) isn't known yet.
    std::optional<glm::mat4> seatFrame(const world::Object& avatar, const world::World& world,
                                       uint64_t originHandle) const;
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
