#include "AvatarView.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <format>
#include <limits>
#include <map>

namespace viewer
{

namespace
{

// Bakes that mean "nothing here" (asset ids from indra_constants.cpp).
const Uuid kDefaultAvatarTexture = *Uuid::parse("c228d1cf-4b5d-4ba8-84f4-899a0796aa97");
const Uuid kInvisibleTexture = *Uuid::parse("3a367d1c-bef1-6d43-7595-e88c1e3aadb3");

glm::vec3 regionOffset(uint64_t region, uint64_t origin)
{
    return glm::vec3(
        static_cast<float>(static_cast<int64_t>(region >> 32) - static_cast<int64_t>(origin >> 32)),
        static_cast<float>(static_cast<int64_t>(region & 0xFFFFFFFFu) -
                           static_cast<int64_t>(origin & 0xFFFFFFFFu)),
        0.0f);
}

// Bake on Mesh placeholders (indra_constants.cpp) and the bakes they stand
// for: texture entry index and the name in the appearance service URL
// (llavatarappearancedefines.cpp).
struct BakeSlot
{
    Uuid placeholder;
    int slot;
    const char* name;
};
const BakeSlot kBakeSlots[] = {
    {*Uuid::parse("5a9f4a74-30f2-821c-b88d-70499d3e7183"), 8, "head"},
    {*Uuid::parse("ae2de45c-d252-50b8-5c6e-19f39ce79317"), 9, "upper"},
    {*Uuid::parse("24daea5f-0539-cfcf-047f-fbc40b2786ba"), 10, "lower"},
    {*Uuid::parse("52cc6bb6-2ee5-e632-d3ad-50197b1dcb8a"), 11, "eyes"},
    {*Uuid::parse("43529ce8-7faa-ad92-165a-bc4078371687"), 19, "skirt"},
    {*Uuid::parse("09aac1fb-6bce-0bee-7d44-caac6dbb6c63"), 20, "hair"},
    {*Uuid::parse("ff62763f-d60a-9855-890b-0c96f8f8cd98"), 40, "leftarm"},
    {*Uuid::parse("8e915e25-31d1-cc95-ae08-d58a47488251"), 41, "leftleg"},
    {*Uuid::parse("9742065b-19b5-297c-858a-29711d539043"), 42, "aux1"},
    {*Uuid::parse("03642e83-2bd1-4eb9-34b4-4c47ed586d2d"), 43, "aux2"},
    {*Uuid::parse("edd51b77-fc10-ce7a-4b3d-011dfc349e4f"), 44, "aux3"},
};

// How far a rigged mesh's joint position must be from the default to count
// (the official viewer's LLJoint::aboveJointPosThreshold).
constexpr float kJointPositionThreshold = 0.0001f;

} // namespace

const world::Object* attachedAvatar(const world::World& world, const world::Object& object)
{
    const world::Object* o = &object;
    for(int depth = 0; depth < 16 && o->parentId != 0; depth++)
    {
        o = world.object(o->region, o->parentId);
        if(!o)
        {
            return nullptr;
        }
        if(o->type == world::ObjectType::Avatar)
        {
            return o;
        }
    }
    return nullptr;
}

// Sitting on the ground (ANIM_AGENT_SIT_GROUND_CONSTRAINED).
static const Uuid kSitGround = *Uuid::parse("1a2bd58e-87ff-0df8-0b4c-53e047b0bb6e");

// The bakes whose morph masks shape the body.
static constexpr std::pair<int, const char*> kMaskBakes[] = {
    {8, "head"}, {9, "upper"}, {10, "lower"}};

SkeletonOwner skeletonOwner(const world::World& world, const world::Object& object)
{
    // Up to the link set's root: an animated object's root, or what's
    // attached to an avatar (or neither).
    const world::Object* root = &object;
    const world::Object* avatar = nullptr;
    for(int depth = 0; depth < 16 && root->parentId != 0; depth++)
    {
        const world::Object* parent = world.object(root->region, root->parentId);
        if(!parent)
        {
            return {};
        }
        if(parent->type == world::ObjectType::Avatar)
        {
            avatar = parent;
            break;
        }
        root = parent;
    }
    if(root->type == world::ObjectType::Prim && root->animatedMesh)
    {
        return {root, root, true}; // even when worn (llcontrolavatar.cpp)
    }
    return {avatar, root, false};
}

AvatarView::AvatarView(avatar::BaseAvatar base, render::Renderer& renderer,
                       const ObjectMotion& motion)
    : base_(std::move(base)), renderer_(renderer), motion_(motion)
{
    defaultBody_ = build(avatar::ParamWeights(base_));
    defaultJoints_ = avatar::poseJoints(base_, defaultBody_->shape);
    ikJoints_ = avatar::IkJoints::find(base_.skeleton);
    physicsRig_ = avatar::PhysicsRig::find(base_);
    // The head and everything below it (parents come before children).
    const int head = base_.skeleton.find("mHead");
    std::vector<bool> under(base_.skeleton.joints.size(), false);
    for(size_t i = 0; i < base_.skeleton.joints.size(); i++)
    {
        const int parent = base_.skeleton.joints[i].parent;
        under[i] =
            static_cast<int>(i) == head || (parent >= 0 && under[static_cast<size_t>(parent)]);
        if(under[i])
        {
            headJoints_.push_back(static_cast<int>(i));
        }
    }
}

std::optional<glm::vec3> AvatarView::eyePosition(const world::Object& avatar,
                                                 const world::World& world, uint64_t originHandle,
                                                 bool steady) const
{
    const auto it = avatars_.find(avatar.id);
    if(it == avatars_.end() || !it->second.hasEyes)
    {
        return std::nullopt;
    }
    const glm::vec3& eyes = steady ? it->second.steadyEyes : it->second.eyes;
    return glm::vec3(rootFrame(avatar, world, originHandle) * glm::vec4(eyes, 1.0f));
}

std::optional<glm::vec3> AvatarView::eyesFromHead(const world::Object& avatar) const
{
    const auto it = avatars_.find(avatar.id);
    if(it == avatars_.end() || !it->second.hasEyes)
    {
        return std::nullopt;
    }
    return it->second.eyesFromHead;
}

float AvatarView::eyesForward(const world::Object& avatar) const
{
    const auto it = avatars_.find(avatar.id);
    return it == avatars_.end() ? 0.0f : it->second.eyesForward;
}

std::optional<float> AvatarView::eyeHeight(const world::Object& avatar) const
{
    const auto it = avatars_.find(avatar.id);
    if(it == avatars_.end() || !it->second.hasEyes || it->second.eyeHeight <= 0.0f)
    {
        return std::nullopt;
    }
    return it->second.eyeHeight;
}

bool AvatarView::hiddenInFirstPerson(const world::Object& avatar, int point) const
{
    if(!firstPerson_ || *firstPerson_ != avatar.id)
    {
        return false;
    }
    const auto it = base_.attachmentPoints.find(point);
    return it != base_.attachmentPoints.end() && !it->second.visibleInFirstPerson;
}

void AvatarView::setIk(const Uuid& avatar, std::optional<avatar::IkGoals> goals)
{
    if(goals)
    {
        ikGoals_[avatar] = std::move(*goals);
    }
    else
    {
        ikGoals_.erase(avatar);
    }
}

std::optional<glm::vec3> AvatarView::jointPosition(const world::Object& avatar,
                                                   const world::World& world,
                                                   std::string_view joint,
                                                   uint64_t originHandle) const
{
    const int index = base_.skeleton.find(joint);
    const auto it = avatars_.find(avatar.id);
    if(index < 0 || it == avatars_.end() || static_cast<size_t>(index) >= it->second.joints.size())
    {
        return std::nullopt;
    }
    return glm::vec3(rootFrame(avatar, world, originHandle) *
                     it->second.joints[static_cast<size_t>(index)][3]);
}

std::shared_ptr<const AvatarView::Body>
AvatarView::build(const avatar::ParamWeights& weights,
                  std::span<const avatar::MorphMask> masks) const
{
    // Hands are relaxed unless an animation says otherwise.
    avatar::ParamWeights relaxed = weights;
    relaxed.set(avatar::kRelaxedHandsParam, 1.0f);
    auto body = std::make_shared<Body>(
        Body{avatar::shapeAvatar(base_, relaxed, masks), {}, std::move(relaxed)});
    for(size_t part = 0; part < base_.meshes.size(); part++)
    {
        body->meshes.push_back(partMesh(body->shape.meshes[part], part));
    }
    body->shape.meshes.clear(); // uploaded; the skeleton is what's still needed
    return body;
}

// Morphed but unskinned: the GPU places each vertex from its joints.
render::Mesh AvatarView::partMesh(const avatar::ShapedMesh& shaped, size_t part) const
{
    std::vector<render::SkinnedVertex> vertices(shaped.positions.size());
    for(size_t v = 0; v < vertices.size(); v++)
    {
        const avatar::Influence in = avatar::vertexInfluence(base_, part, v);
        render::SkinnedVertex& out = vertices[v];
        out.position = shaped.positions[v];
        out.normal = glm::normalize(shaped.normals[v]);
        out.uv = shaped.texCoords[v];
        out.joints = glm::u8vec4(in.first, in.second, 0, 0);
        out.weights = glm::vec4(1.0f - in.blend, in.blend, 0.0f, 0.0f);
    }
    return renderer_.createMesh(vertices, base_.meshes[part].indices);
}

const render::Mesh* AvatarView::expressedPart(Avatar& a, const std::shared_ptr<const Body>& body,
                                              size_t part, bool physics)
{
    // The parameters that morph this part and differ from the body's, in
    // steps (a change too small to see isn't worth a mesh).
    constexpr float kSteps = 32.0f;
    constexpr float kPhysicsSteps = 128.0f; // (it moves slowly, in plain view)
    std::map<int, float> weights;
    auto morphs = [&](int id)
    {
        const auto it = base_.params.find(id);
        return it != base_.params.end() &&
               std::ranges::any_of(it->second.morphs,
                                   [&](const auto& ref) { return ref.part == part; });
    };
    for(const auto& [id, weight] : a.animator.expression())
    {
        const int step = static_cast<int>(std::lround(weight * kSteps));
        if(morphs(id) && step != std::lround(body->weights[id] * kSteps))
        {
            weights[id] = static_cast<float>(step) / kSteps;
        }
    }
    if(physics)
    {
        for(const auto& [id, weight] : a.physics.weights())
        {
            const float rounded = std::round(weight * kPhysicsSteps) / kPhysicsSteps;
            if(morphs(id) && rounded != body->weights[id])
            {
                weights[id] = rounded;
            }
        }
    }
    if(a.animator.animated() && !a.animator.expression().contains(avatar::kRelaxedHandsParam) &&
       morphs(avatar::kRelaxedHandsParam))
    {
        weights[avatar::kRelaxedHandsParam] = 0.0f;
    }
    ExpressedPart& e = a.expressed[part];
    if(e.body != body || e.weights != weights)
    {
        e.mesh = {};
        e.body = body;
        e.weights = std::move(weights);
        if(!e.weights.empty())
        {
            avatar::ParamWeights expressed = body->weights;
            for(const auto& [id, weight] : e.weights)
            {
                expressed.set(id, weight);
            }
            std::vector<avatar::MorphMask> masks;
            for(size_t i = 0; i < a.masks.size(); i++)
            {
                if(a.masks[i])
                {
                    masks.push_back({kMaskBakes[i].first, a.masks[i]->width, a.masks[i]->height,
                                     a.masks[i]->pixels});
                }
            }
            e.mesh = partMesh(avatar::shapePart(base_, expressed, part, masks), part);
        }
    }
    return e.weights.empty() ? nullptr : &e.mesh;
}

const AvatarView::Body& AvatarView::bodyOf(const world::Object& avatar) const
{
    const auto it = avatars_.find(avatar.id);
    return it == avatars_.end() || !it->second.body ? *defaultBody_ : *it->second.body;
}

glm::mat4 AvatarView::rootFrame(const world::Object& avatar, const world::World& world,
                                uint64_t originHandle) const
{
    // Once a frame per avatar: all its attachments ask.
    const auto [it, inserted] = rootFrames_.try_emplace(avatar.id);
    if(inserted)
    {
        it->second = computeRootFrame(avatar, world, originHandle);
    }
    return it->second;
}

glm::mat4 AvatarView::computeRootFrame(const world::Object& avatar, const world::World& world,
                                       uint64_t originHandle) const
{
    const avatar::Shape& shape = bodyOf(avatar).shape;
    // Sitting, the avatar's place is relative to its seat (a prim, maybe a
    // child of a link set), as the official viewer's getRenderPosition; the
    // hover it asked for doesn't apply then, the shape's still does.
    const std::optional<glm::mat4> seat =
        avatar.parentId != 0 ? seatFrame(avatar, world, originHandle) : std::nullopt;
    const world::World::Appearance* appearance = world.appearance(avatar.id);
    const auto* playing = world.animations(avatar.id);
    const bool onGround =
        playing && std::ranges::any_of(*playing, [](const world::PlayingAnimation& p)
                                       { return p.id == kSitGround; });
    const glm::vec3 hover =
        (appearance && !seat && !onGround ? appearance->hover : glm::vec3(0.0f)) +
        glm::vec3(0.0f, 0.0f, shape.hover);
    // A rigged attachment's pelvis offset moves where the avatar is drawn
    // (llvoavatar.cpp getRenderPosition).
    const auto overrides = overrides_.find(avatar.id);
    const float pelvisOffset =
        overrides != overrides_.end() ? overrides->second.pelvisOffset : 0.0f;
    const auto known = avatars_.find(avatar.id);
    const avatar::BodySize size = known != avatars_.end() && known->second.size
                                      ? *known->second.size
                                      : avatar::BodySize{shape.pelvisToFoot, shape.bodyHeight};
    const world::Pose pose = motion_.pose(avatar);
    const glm::vec3 lowered =
        hover - glm::vec3(0.0f, 0.0f, 0.5f * size.height - size.pelvisToFoot - pelvisOffset);
    if(seat)
    {
        const glm::mat4 placed = *seat * motion_.local(avatar);
        return glm::translate(glm::mat4(1.0f), lowered) * placed;
    }
    const glm::vec3 root = regionOffset(avatar.region, originHandle) + pose.position + lowered;
    glm::quat rotation = pose.rotation;
    if(known != avatars_.end() && known->second.facing)
    {
        rotation = *known->second.facing;
    }
    return glm::translate(glm::mat4(1.0f), root) * glm::mat4_cast(rotation);
}

void AvatarView::updateFacing(Avatar& a, const world::Object& avatar, double now) const
{
    if(avatar.parentId != 0)
    {
        a.facing.reset(); // sitting: the seat turns it
        return;
    }
    // Where it faces: ours where we turned it, others as the simulator has
    // them (flattened: the body stays upright).
    const bool ours = avatar.id == self_ && selfYaw_;
    glm::vec3 forward = ours ? glm::vec3(std::cos(glm::radians(*selfYaw_)),
                                         std::sin(glm::radians(*selfYaw_)), 0.0f)
                             : motion_.pose(avatar).rotation * glm::vec3(1.0f, 0.0f, 0.0f);
    forward.z = 0.0f;
    if(glm::length(forward) < 1e-3f)
    {
        forward = a.facing ? *a.facing * glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    }
    forward = glm::normalize(forward);
    // Walking, toward where it's going: all the way from 2 m/s. (Backward
    // stays backward; sideways turns it sideways, as the official viewer.)
    constexpr float kTurnFrom = 0.5f; // m/s
    constexpr float kTurnedAt = 2.0f;
    const glm::vec3 velocity(avatar.velocity.x, avatar.velocity.y, 0.0f);
    if(const float speed = glm::length(velocity); speed > kTurnFrom)
    {
        glm::vec3 going = velocity / speed;
        if(glm::dot(going, forward) < -0.5f)
        {
            going = -going;
        }
        const glm::vec3 blended =
            glm::mix(forward, going, std::min((speed - kTurnFrom) / (kTurnedAt - kTurnFrom), 1.0f));
        if(glm::length(blended) > 1e-3f)
        {
            forward = glm::normalize(blended);
        }
    }
    const glm::quat target =
        glm::angleAxis(std::atan2(forward.y, forward.x), glm::vec3(0.0f, 0.0f, 1.0f));

    // Eased there (the official viewer's pelvis lag: 0.4 s walking, 0.15 s
    // in mouselook). New, or not turned for a while: there at once.
    const double since = now - a.facedAt;
    a.facedAt = now;
    if(!a.facing || since > 1.0 || since < 0.0)
    {
        a.facing = target;
        return;
    }
    const float lag = ours && selfQuickTurn_ ? 0.15f : 0.4f; // seconds
    const float blend = 1.0f - std::exp(-static_cast<float>(since) / lag);
    a.facing = glm::normalize(glm::slerp(*a.facing, target, blend));
}

std::optional<glm::mat4> AvatarView::seatFrame(const world::Object& avatar,
                                               const world::World& world,
                                               uint64_t originHandle) const
{
    // Up through the seat's link parents to the root prim.
    glm::mat4 frame(1.0f);
    const world::Object* o = &avatar;
    for(int depth = 0; depth < 16 && o->parentId != 0; depth++)
    {
        const world::Object* parent = world.object(o->region, o->parentId);
        if(!parent || parent->type != world::ObjectType::Prim)
        {
            return std::nullopt;
        }
        frame = motion_.local(*parent) * frame;
        o = parent;
    }
    if(o->parentId != 0)
    {
        return std::nullopt;
    }
    return glm::translate(glm::mat4(1.0f), regionOffset(o->region, originHandle)) * frame;
}

bool AvatarView::isHudPoint(int point) const
{
    const auto it = base_.attachmentPoints.find(point);
    return it != base_.attachmentPoints.end() && it->second.hud;
}

std::optional<glm::mat4> AvatarView::attachmentFrame(const world::Object& avatar,
                                                     const world::World& world, int point,
                                                     uint64_t originHandle) const
{
    // Once a frame per avatar and point: every prim attached there asks.
    const auto [it, inserted] = attachmentFrames_.try_emplace(PointKey{avatar.id, point});
    if(inserted)
    {
        it->second = computeAttachmentFrame(avatar, world, point, originHandle);
    }
    return it->second;
}

std::optional<glm::mat4> AvatarView::computeAttachmentFrame(const world::Object& avatar,
                                                            const world::World& world, int point,
                                                            uint64_t originHandle) const
{
    const auto it = base_.attachmentPoints.find(point);
    if(it == base_.attachmentPoints.end())
    {
        return std::nullopt;
    }
    const avatar::AttachmentPoint& a = it->second;
    if(a.hud)
    {
        return glm::translate(glm::mat4(1.0f), a.position * glm::vec3(1.0f, hudAspect_, 1.0f)) *
               glm::mat4_cast(a.rotation);
    }
    if(!placed(avatar, world, originHandle))
    {
        return std::nullopt;
    }
    glm::mat4 joint(1.0f);
    if(a.joint >= 0)
    {
        const auto state = avatars_.find(avatar.id);
        const std::vector<glm::mat4>& joints =
            state != avatars_.end() && !state->second.joints.empty() ? state->second.joints
                                                                     : defaultJoints_;
        // The joint's position and rotation. Its scale doesn't carry over to
        // what's attached, but scales the point's offset (as llxform.cpp does
        // for child joints).
        const glm::mat4& m = joints[static_cast<size_t>(a.joint)];
        const glm::vec3 scale(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])),
                              glm::length(glm::vec3(m[2])));
        joint = m * glm::scale(glm::mat4(1.0f), 1.0f / scale) *
                glm::translate(glm::mat4(1.0f), scale * a.position);
    }
    else
    {
        joint = glm::translate(glm::mat4(1.0f), a.position);
    }
    return rootFrame(avatar, world, originHandle) * joint * glm::mat4_cast(a.rotation);
}

const render::GpuTexture* AvatarView::bake(const world::World& world, const world::Object& avatar,
                                           int slot, const char* name, TextureManager& textures,
                                           float texels, bool linear) const
{
    // From AvatarAppearance if we have it, else the object's own entry.
    const world::World::Appearance* appearance = world.appearance(avatar.id);
    const world::TextureEntry& faces = appearance ? appearance->faces : avatar.faces;
    const Uuid& id = faces[static_cast<size_t>(slot)].texture;
    if(id.isNull() || id == kDefaultAvatarTexture || id == kInvisibleTexture ||
       appearanceUrl_.empty())
    {
        return nullptr;
    }
    // As the official viewer's getImageURL().
    const std::string url = std::format("{}texture/{}/{}/{}", appearanceUrl_, avatar.id.toString(),
                                        name, id.toString());
    return linear ? textures.getLinear(id, url, texels) : textures.get(id, url, texels);
}

std::shared_ptr<const image::Plane> AvatarView::bakeMask(const world::World& world,
                                                         const world::Object& avatar, int slot,
                                                         const char* name,
                                                         TextureManager& textures) const
{
    const world::World::Appearance* appearance = world.appearance(avatar.id);
    const world::TextureEntry& faces = appearance ? appearance->faces : avatar.faces;
    const Uuid& id = faces[static_cast<size_t>(slot)].texture;
    if(id.isNull() || id == kDefaultAvatarTexture || id == kInvisibleTexture ||
       appearanceUrl_.empty())
    {
        return nullptr;
    }
    const std::string url = std::format("{}texture/{}/{}/{}", appearanceUrl_, avatar.id.toString(),
                                        name, id.toString());
    return textures.mask(id, url);
}

std::optional<const render::GpuTexture*>
AvatarView::bakeFor(const world::World& world, const world::Object& avatar, const Uuid& texture,
                    TextureManager& textures, float texels, bool linear) const
{
    for(const BakeSlot& b : kBakeSlots)
    {
        if(b.placeholder == texture)
        {
            return bake(world, avatar, b.slot, b.name, textures, texels, linear);
        }
    }
    return std::nullopt;
}

std::optional<Uuid> AvatarView::bakeId(const world::World& world, const world::Object& avatar,
                                       const Uuid& texture) const
{
    for(const BakeSlot& b : kBakeSlots)
    {
        if(b.placeholder == texture)
        {
            const world::World::Appearance* appearance = world.appearance(avatar.id);
            const world::TextureEntry& faces = appearance ? appearance->faces : avatar.faces;
            return faces[static_cast<size_t>(b.slot)].texture;
        }
    }
    return std::nullopt;
}

const std::vector<glm::mat4>& AvatarView::posedJoints(const world::Object& avatar) const
{
    const auto it = avatars_.find(avatar.id);
    return it != avatars_.end() && !it->second.joints.empty() ? it->second.joints : defaultJoints_;
}

const std::vector<glm::mat4>* AvatarView::shadowJoints(const world::Object& avatar) const
{
    const auto it = avatars_.find(avatar.id);
    return it != avatars_.end() && !it->second.shadowJoints.empty() ? &it->second.shadowJoints
                                                                    : nullptr;
}

const AvatarView::SkinBinding&
AvatarView::binding(const std::shared_ptr<const assets::MeshSkin>& skin) const
{
    auto [it, inserted] = bindings_.try_emplace(skin.get());
    if(inserted)
    {
        it->second.first = skin;
        SkinBinding& b = it->second.second;
        b.joints.reserve(skin->jointNames.size());
        b.bind.reserve(skin->jointNames.size());
        for(size_t j = 0; j < skin->jointNames.size(); j++)
        {
            b.joints.push_back(base_.skeleton.find(skin->jointNames[j]));
            b.bind.push_back(skin->inverseBind[j] * skin->bindShape);
        }
    }
    return it->second.second;
}

void AvatarView::clear()
{
    avatars_.clear();
    ikGoals_.clear();
    overrides_.clear();
    bakesInUse_.clear();
    animatedParts_.clear();
    worn_.clear();
    loading_.clear();
    collectedAt_.reset();
}

float AvatarView::extent(const world::Object& avatar) const
{
    const auto it = avatars_.find(avatar.id);
    // Until posed: about a default avatar's.
    return it != avatars_.end() && it->second.extent > 0.0f ? it->second.extent : 2.0f;
}

void AvatarView::collectAttachments(const world::World& world, MeshManager& meshes)
{
    // Busy regions change some object nearly every frame: rescanning them
    // all then is wasted. At once when meshes arrive, otherwise at most a
    // few times a second.
    const std::pair revisions(world.objectContentsRevision(), meshes.revision());
    const auto now = std::chrono::steady_clock::now();
    if(collectedAt_ == revisions || (collectedAt_ && collectedAt_->second == revisions.second &&
                                     now - collectedTime_ < kCollectInterval))
    {
        return;
    }
    collectedAt_ = revisions;
    collectedTime_ = now;
    collection_++;
    overrides_.clear();
    bakesInUse_.clear();
    animatedParts_.clear();
    worn_.clear();
    for(const auto& [key, object] : world.objects())
    {
        if(object.type != world::ObjectType::Prim)
        {
            continue;
        }

        // An animated object's other prims.
        if(object.parentId != 0)
        {
            const world::Object* parent = world.object(object.region, object.parentId);
            if(parent && parent->type == world::ObjectType::Prim && parent->animatedMesh)
            {
                animatedParts_[parent->id].push_back(object.id);
            }
        }

        // Bake on Mesh: any face of an attachment showing a bake.
        if(const world::Object* avatar =
               object.parentId != 0 ? attachedAvatar(world, object) : nullptr)
        {
            if(!loaded(avatar->id))
            {
                Worn& worn = worn_[avatar->id];
                worn.attachments += object.parentId == avatar->localId;
                if((object.sculptType & world::SculptType::Mask) == world::SculptType::Mesh)
                {
                    worn.meshes.push_back(object.sculptTexture);
                }
            }
            uint64_t bakes = 0;
            for(const world::TextureFace& face : object.faces)
            {
                for(const BakeSlot& b : kBakeSlots)
                {
                    if(face.texture == b.placeholder)
                    {
                        bakes |= uint64_t(1) << b.slot;
                    }
                }
            }
            if(bakes != 0)
            {
                bakesInUse_[avatar->id] |= bakes;
            }
        }

        if((object.sculptType & world::SculptType::Mask) != world::SculptType::Mesh ||
           !meshes.isRigged(object.sculptTexture).value_or(false))
        {
            continue;
        }
        // Joint positions go to the skeleton the mesh follows.
        const world::Object* owner = skeletonOwner(world, object).owner;
        const auto skin = owner ? meshes.skin(object.sculptTexture) : nullptr;
        if(!skin || skin->alternateBind.empty())
        {
            continue;
        }
        Overrides& overrides = overrides_[owner->id];
        const Uuid& mesh = object.sculptTexture;
        if(skin->pelvisOffset != 0.0f &&
           (overrides.pelvisOffsetMesh.isNull() || mesh > overrides.pelvisOffsetMesh))
        {
            overrides.pelvisOffsetMesh = mesh;
            overrides.pelvisOffset = skin->pelvisOffset;
        }
        const SkinBinding& resolved = binding(skin);
        for(size_t j = 0; j < skin->jointNames.size(); j++)
        {
            const int joint = resolved.joints[j];
            if(joint < 0)
            {
                continue;
            }
            const glm::vec3 position(skin->alternateBind[j][3]);
            const glm::vec3 defaultPosition =
                base_.skeleton.joints[static_cast<size_t>(joint)].position;
            if(glm::length(position - defaultPosition) <= kJointPositionThreshold)
            {
                continue;
            }
            // Compared with the skeleton file's position, also for the
            // pelvis: it sits at the root unless a mesh moves it, and then
            // it's where the mesh says (llavatarappearance.cpp, lljoint.cpp).
            const auto it = overrides.joints.find(joint);
            if(it == overrides.joints.end() || mesh > it->second.mesh)
            {
                overrides.joints[joint] = {mesh, position, skin->lockScaleIfJointPosition};
            }
        }
    }
}

void AvatarView::applyOverrides(const Uuid& id, std::vector<avatar::JointPose>& pose) const
{
    // Rigged mesh's joint positions replace the shape's, but not animated
    // ones.
    const auto o = overrides_.find(id);
    if(o == overrides_.end())
    {
        return;
    }
    for(const auto& [joint, override] : o->second.joints)
    {
        avatar::JointPose& p = pose[static_cast<size_t>(joint)];
        if(!p.position)
        {
            p.position = override.position;
        }
        if(override.lockScale)
        {
            p.scale = base_.skeleton.joints[static_cast<size_t>(joint)].scale;
        }
    }
}

void AvatarView::updateSize(Avatar& a, const std::shared_ptr<const Body>& body, const Uuid& id,
                            double now)
{
    // Animations moved joints by now, if those that changed do.
    constexpr double kSettleSeconds = 1.0;
    bool changed = !a.size || a.sizedBody != body || std::exchange(a.resize, false);
    if(a.sizedCollection != collection_)
    {
        a.sizedCollection = collection_;
        const auto o = overrides_.find(id);
        Overrides overrides = o != overrides_.end() ? o->second : Overrides{};
        if(overrides != a.sizedOverrides)
        {
            a.sizedOverrides = std::move(overrides);
            changed = true;
        }
    }
    const bool settled = a.resizeAt && now >= *a.resizeAt;
    if(!changed && !settled)
    {
        return;
    }
    a.resizeAt = changed ? std::optional(now + kSettleSeconds) : std::nullopt;
    a.sizedBody = body;
    std::vector<avatar::JointPose> pose(a.animator.held().begin(), a.animator.held().end());
    pose.resize(base_.skeleton.joints.size());
    applyOverrides(id, pose);
    a.size = avatar::bodySize(base_, body->shape, pose);
}

void AvatarView::pose(Avatar& a, const avatar::Shape& shape, const world::World& world,
                      const Uuid& id, AnimationManager& animations, double now,
                      const glm::mat4& root, uint64_t originHandle, const world::Object* standing)
{
    animate(a, world, id, animations, now);
    const auto complete = [&](std::vector<avatar::JointPose>& pose) { applyOverrides(id, pose); };
    avatar::ConstraintContext context{base_, shape, complete, root, originHandle, {}};
    if(standing)
    {
        // The land, or the plane the simulator says the avatar stands on
        // where that's higher (LLWorld::resolveStepHeightGlobal, from a
        // meter up to a meter down).
        context.ground =
            [&](const glm::vec3& at) -> std::optional<avatar::ConstraintContext::Ground>
        {
            const glm::vec3 offset = regionOffset(standing->region, originHandle);
            const glm::vec3 top = at - offset + glm::vec3(0.0f, 0.0f, 1.0f);
            std::optional<avatar::ConstraintContext::Ground> found;
            if(const world::RegionTerrain* land = world.terrain(standing->region))
            {
                const float height = land->heightAt(top.x, top.y);
                const glm::vec3 slope(height - land->heightAt(top.x + 1.0f, top.y),
                                      height - land->heightAt(top.x, top.y + 1.0f), 1.0f);
                found = {height + offset.z, glm::normalize(slope)};
            }
            const glm::vec3 normal(standing->footPlane);
            if(normal != glm::vec3(0.0f))
            {
                // 5 cm for the simulator's error.
                const float down =
                    std::clamp(glm::dot(top, normal) - standing->footPlane.w + 0.05f, 0.0f, 2.0f);
                const float height = top.z - down + offset.z;
                if(!found || height > found->height)
                {
                    found = {height, normal};
                }
            }
            return found;
        };
    }
    a.animator.evaluate(now, base_.skeleton.joints.size(), a.pose, &context);
    complete(a.pose);
    // Viewer-made motion (ours only), from scene to avatar space.
    if(const auto ik = ikGoals_.find(id); ik != ikGoals_.end())
    {
        const glm::mat4 toAvatar = glm::inverse(root);
        const glm::quat toAvatarRotation = glm::quat_cast(glm::mat3(toAvatar));
        avatar::IkGoals goals = ik->second;
        auto local = [&](glm::vec3& p) { p = glm::vec3(toAvatar * glm::vec4(p, 1.0f)); };
        auto effector = [&](std::optional<avatar::IkGoals::Effector>& e)
        {
            if(e)
            {
                local(e->position);
                if(e->rotation)
                {
                    e->rotation = glm::normalize(toAvatarRotation * *e->rotation);
                }
            }
        };
        if(!goals.avatarSpace)
        {
            if(goals.lookAt)
            {
                local(*goals.lookAt);
            }
            effector(goals.head);
            effector(goals.leftHand);
            effector(goals.rightHand);
            for(avatar::IkGoals::Chain& c : goals.chains)
            {
                std::optional<avatar::IkGoals::Effector> e = c.goal;
                effector(e);
                c.goal = *e;
            }
        }
        if(a.pose.size() != base_.skeleton.joints.size())
        {
            a.pose.resize(base_.skeleton.joints.size());
        }
        avatar::applyIk(base_, shape, ikJoints_, goals, a.pose, a.animator.rotationClaims());
    }
    // First person: the head shrunk to nothing (the eyes noted first).
    if(firstPerson_ && *firstPerson_ == id)
    {
        const std::vector<avatar::JointFrame> frames = avatar::poseFrames(base_, shape, a.pose);
        if(ikJoints_.eyeLeft >= 0 && ikJoints_.eyeRight >= 0)
        {
            auto between = [&](const std::vector<avatar::JointFrame>& f)
            {
                return (f[static_cast<size_t>(ikJoints_.eyeLeft)].position +
                        f[static_cast<size_t>(ikJoints_.eyeRight)].position) *
                       0.5f;
            };
            a.eyes = between(frames);
            // Steady: the unanimated eyes' offset from the pelvis, on the
            // animated pelvis (sitting or crouching still lowers them).
            a.steadyEyes = a.eyes;
            if(ikJoints_.pelvis >= 0)
            {
                const auto pelvis = static_cast<size_t>(ikJoints_.pelvis);
                // Unanimated, but with rigged mesh's joint positions (a
                // smaller or larger mesh avatar's eyes are where its mesh
                // puts them).
                std::vector<avatar::JointPose> restPose(base_.skeleton.joints.size());
                if(const auto o = overrides_.find(id); o != overrides_.end())
                {
                    for(const auto& [joint, override] : o->second.joints)
                    {
                        avatar::JointPose& p = restPose[static_cast<size_t>(joint)];
                        p.position = override.position;
                        if(override.lockScale)
                        {
                            p.scale = base_.skeleton.joints[static_cast<size_t>(joint)].scale;
                        }
                    }
                }
                const std::vector<avatar::JointFrame> rest =
                    avatar::poseFrames(base_, shape, restPose);
                // Straight above the pelvis: turning the body mustn't move
                // the view.
                const glm::vec3 offset = between(rest) - rest[pelvis].position;
                a.steadyEyes = frames[pelvis].position + glm::vec3(0.0f, 0.0f, offset.z);
                a.eyesForward = offset.x;
                // Above the ground: the pelvis is its height off the feet
                // (and a rigged mesh's pelvis offset) above it.
                const auto o = overrides_.find(id);
                a.eyeHeight = offset.z + avatar::bodySize(base_, shape, restPose).pelvisToFoot +
                              (o != overrides_.end() ? o->second.pelvisOffset : 0.0f);
                if(ikJoints_.head >= 0)
                {
                    a.eyesFromHead =
                        between(rest) - rest[static_cast<size_t>(ikJoints_.head)].position;
                }
            }
            a.hasEyes = true;
        }
        if(a.pose.size() != base_.skeleton.joints.size())
        {
            a.pose.resize(base_.skeleton.joints.size());
        }
        // The shadow keeps the head: it's only out of our own view.
        a.shadowJoints = avatar::poseJoints(base_, shape, a.pose);
        for(const int j : headJoints_)
        {
            a.pose[static_cast<size_t>(j)].scale = glm::vec3(0.0f);
        }
    }
    else
    {
        a.shadowJoints.clear();
    }
    a.joints = avatar::poseJoints(base_, shape, a.pose);
    glm::vec3 low(a.joints.empty() ? 0.0f : std::numeric_limits<float>::max());
    glm::vec3 high(a.joints.empty() ? 0.0f : std::numeric_limits<float>::lowest());
    for(const glm::mat4& joint : a.joints)
    {
        low = glm::min(low, glm::vec3(joint[3]));
        high = glm::max(high, glm::vec3(joint[3]));
    }
    a.extent = glm::length(high - low);
}

void AvatarView::simulate(Avatar& a, const Body& body, const glm::mat4& root, uint64_t originHandle,
                          double now)
{
    if(!physicsOn_)
    {
        a.physics.reset();
        return;
    }
    const glm::dvec3 origin(static_cast<double>(originHandle >> 32),
                            static_cast<double>(originHandle & 0xFFFFFFFFu), 0.0);
    a.physics.update(physicsRig_, base_, body.weights, a.joints, root, origin, now);
    a.physics.moveVolumes(base_, a.joints);
    if(!a.shadowJoints.empty())
    {
        a.physics.moveVolumes(base_, a.shadowJoints);
    }
}

std::string AvatarView::physicsText(const world::World& world, const world::Object& avatar,
                                    MeshManager& meshes) const
{
    const Body& body = bodyOf(avatar);
    const auto it = avatars_.find(avatar.id);
    const world::World::Appearance* appearance = world.appearance(avatar.id);
    std::string text = std::format(
        "physics: {}, {} body, {} of {} parameters sent, outfit version {}\n",
        physicsOn_ ? "on" : "off", body.weights.sex() == avatar::Sex::Male ? "male" : "female",
        appearance ? appearance->visualParams.size() : size_t{0}, base_.transmitted.size(),
        appearance ? appearance->cofVersion : -1);
    for(const auto& [id, p] : base_.params)
    {
        if(p.transmitted() && p.name.contains("_Physics_"))
        {
            text += std::format("  {} {:.3f}\n", p.name, body.weights[id]);
        }
    }
    if(it != avatars_.end())
    {
        for(const auto& [id, weight] : it->second.physics.weights())
        {
            text += std::format("  now: {} {:.3f}\n", base_.params.at(id).name, weight);
        }
    }
    for(const auto& [key, object] : world.objects())
    {
        const world::Object* wearer =
            object.parentId != 0 ? attachedAvatar(world, object) : nullptr;
        if(!wearer || wearer->id != avatar.id ||
           (object.sculptType & world::SculptType::Mask) != world::SculptType::Mesh)
        {
            continue;
        }
        const auto skin = meshes.skin(object.sculptTexture);
        if(!skin)
        {
            continue;
        }
        text += std::format("  mesh {} (local {}): volumes", object.sculptTexture.toString(),
                            object.localId);
        for(const std::string& name : skin->jointNames)
        {
            const int joint = base_.skeleton.find(name);
            if(joint < 0)
            {
                text += " unknown:" + name;
            }
            else if(base_.skeleton.joints[static_cast<size_t>(joint)].volume)
            {
                text += " " + name;
            }
        }
        text += "\n";
    }
    return text;
}

// Starts and stops animations to match what the simulator says is playing.
void AvatarView::animate(Avatar& a, const world::World& world, const Uuid& id,
                         AnimationManager& animations, double now)
{
    // By id, as the official viewer's map: the order those told of together
    // start in, with one sequence number each. An animated object also plays
    // what its other prims' scripts started, the larger sequence number
    // winning (LLControlAvatar::updateAnimations).
    std::map<Uuid, int32_t> signaled;
    if(const auto* playing = world.animations(id))
    {
        for(const world::PlayingAnimation& p : *playing)
        {
            signaled[p.id] = p.sequence;
        }
    }
    if(const auto parts = animatedParts_.find(id); parts != animatedParts_.end())
    {
        for(const Uuid& part : parts->second)
        {
            std::map<Uuid, int32_t> theirs;
            if(const auto* playing = world.animations(part))
            {
                for(const world::PlayingAnimation& p : *playing)
                {
                    theirs[p.id] = p.sequence;
                }
            }
            for(const auto& [animation, sequence] : theirs)
            {
                const auto [it, inserted] = signaled.try_emplace(animation, sequence);
                it->second = std::max(it->second, sequence);
            }
        }
    }
    for(const auto& [animationId, sequence] : signaled)
    {
        const auto it = a.started.find(animationId);
        if(it != a.started.end() && it->second == sequence)
        {
            continue;
        }
        auto animation = avatar::emoteAnimation(animationId);
        if(!animation)
        {
            animation = animations.get(animationId);
        }
        if(!animation)
        {
            continue; // still loading: tried again next frame
        }
        a.animator.start(animationId, std::move(animation), now);
        a.started[animationId] = sequence;
        a.resize = true;
    }
    std::erase_if(a.started,
                  [&](const auto& entry)
                  {
                      const bool still = signaled.contains(entry.first);
                      if(!still)
                      {
                          a.animator.stop(entry.first, now);
                          a.resize = true;
                      }
                      return !still;
                  });
}

std::vector<Uuid> AvatarView::resetSkeleton(const Uuid& avatar, bool animations, bool ours)
{
    const auto it = avatars_.find(avatar);
    if(it == avatars_.end())
    {
        return {};
    }
    Avatar& a = it->second;
    a.animator.resetHeld();
    a.resize = true;
    collectedAt_.reset(); // rigged mesh's joint positions again
    std::vector<Uuid> stopped;
    if(animations && ours)
    {
        // LLAgent::stopCurrentAnimations: sitting on the ground, do not
        // disturb.
        static const std::array<Uuid, 2> kKeep = {
            kSitGround, *Uuid::parse("efcf670c-2d18-8128-973a-034ebc806b67")};
        for(const auto& [id, sequence] : a.started)
        {
            if(!std::ranges::contains(kKeep, id))
            {
                stopped.push_back(id);
            }
        }
        a.animator.stopAll(kKeep);
    }
    else if(animations)
    {
        a.animator.flush();
    }
    return stopped;
}

std::vector<Uuid> AvatarView::takeEndedAnimations(const Uuid& avatar)
{
    const auto it = avatars_.find(avatar);
    return it != avatars_.end() ? it->second.animator.takeEnded() : std::vector<Uuid>{};
}

void AvatarView::updateLoading(const world::World& world, const world::Object& avatar,
                               TextureManager& textures, MeshManager& meshes, double now)
{
    Loading& l = loading_[avatar.id];
    l.seen = now;
    if(l.state.loaded())
    {
        return;
    }
    const world::World::Appearance* appearance = world.appearance(avatar.id);
    AvatarLoading::Inputs in;
    in.missingParts = hasMissingParts(appearance);
    if(appearance)
    {
        // Every bake it has, asked for even out of sight (the least will
        // do: those in sight are asked for at their size once drawn).
        for(const BakeSlot& b : kBakeSlots)
        {
            const Uuid& id = appearance->faces[static_cast<size_t>(b.slot)].texture;
            if(!bakeDefined(id) || id == kInvisibleTexture)
            {
                continue;
            }
            in.bakes = in.bakes * 31 + std::hash<Uuid>()(id);
            bake(world, avatar, b.slot, b.name, textures, 0.0f);
            in.bakesPending |= !textures.settled(id);
        }
        if(avatar.id != self_)
        {
            in.expected = appearance->attachments;
        }
    }
    if(const auto worn = worn_.find(avatar.id); worn != worn_.end())
    {
        in.attachments = worn->second.attachments;
        for(const Uuid& mesh : worn->second.meshes)
        {
            in.meshesPending |= !meshes.settled(mesh); // (asks for each)
        }
    }
    l.state.update(in, now);
}

void AvatarView::addTo(render::Scene& scene, const world::World& world, uint64_t originHandle,
                       TextureManager& textures, AnimationManager& animations, MeshManager& meshes,
                       double now, float drawDistance)
{
    frame_++;
    drawn_ = 0;
    animatedObjects_ = 0;
    rootFrames_.clear(); // poses change below
    attachmentFrames_.clear();
    collectAttachments(world, meshes);
    for(const world::ObjectKey& key : world.avatarKeys())
    {
        const world::Object* found = world.object(key.region, key.localId);
        if(!found)
        {
            continue;
        }
        const world::Object& object = *found;
        updateLoading(world, object, textures, meshes, now);
        if(!placed(object, world, originHandle))
        {
            continue; // sitting on something not here yet
        }
        Avatar& a = avatars_[object.id];
        a.seen = frame_;
        a.key = key;
        updateFacing(a, object, now);

        // Reshape when the appearance changed.
        const world::World::Appearance* appearance = world.appearance(object.id);
        if(appearance && !appearance->visualParams.empty())
        {
            // The morphs that follow clothing go by the bakes' masks
            // (LLVOAvatar::onBakedTextureMasksLoaded): rebuilt as they come.
            std::array<std::shared_ptr<const image::Plane>, 3> masks;
            for(size_t i = 0; i < masks.size(); i++)
            {
                masks[i] =
                    bakeMask(world, object, kMaskBakes[i].first, kMaskBakes[i].second, textures);
            }
            if(!a.body || a.appearanceSerial != appearance->serial || masks != a.masks)
            {
                std::vector<avatar::MorphMask> inputs;
                for(size_t i = 0; i < masks.size(); i++)
                {
                    if(masks[i])
                    {
                        inputs.push_back({kMaskBakes[i].first, masks[i]->width, masks[i]->height,
                                          masks[i]->pixels});
                    }
                }
                a.body = build(avatar::ParamWeights(base_, appearance->visualParams), inputs);
                a.appearanceSerial = appearance->serial;
                a.masks = std::move(masks);
            }
        }
        const std::shared_ptr<const Body>& bodyShared = a.body ? a.body : defaultBody_;
        const Body& body = *bodyShared;
        updateSize(a, bodyShared, object.id, now);

        // Seen (or its shadow, or near enough)? Where it stands doesn't
        // depend on its pose.
        const glm::mat4 root = rootFrame(object, world, originHandle);
        const render::Visibility seen =
            scene.visibility(glm::vec3(root[3]), std::max(extent(object), 1.5f));

        // Pose: every avatar's (attachments follow it), those out of sight
        // only every so many frames, as the official viewer slows unseen
        // avatars' updates (their animation's time still runs).
        const bool unseen = seen == render::Visibility::Hidden;
        if(!unseen || a.joints.empty() ||
           (frame_ + std::hash<Uuid>()(object.id)) % kUnseenPosePeriod == 0)
        {
            pose(a, body.shape, world, object.id, animations, now, root, originHandle, &object);
            simulate(a, body, root, originHandle, now);
        }
        if(glm::length(glm::vec3(root[3]) - scene.camera.position) > drawDistance ||
           seen == render::Visibility::Hidden || !loaded(object.id))
        {
            continue;
        }
        drawn_++;
        // A bake unwraps its part front and back: about twice the avatar's
        // height across (the least for shadows only).
        const float texels =
            seen == render::Visibility::ShadowOnly
                ? 0.0f
                : textures.texelsFor(2.0f * std::max(extent(object), 1.5f),
                                     glm::length(glm::vec3(root[3]) - scene.camera.position));
        const auto inUse = bakesInUse_.find(object.id);
        const uint64_t hidden = inUse != bakesInUse_.end() ? inUse->second : 0;
        for(size_t i = 0; i < std::size(avatar::kBodyParts); i++)
        {
            const avatar::BodyPart& part = avatar::kBodyParts[i];
            if(!body.meshes[i] || (hidden >> part.bake & 1) != 0)
            {
                continue; // none, or an attachment shows its bake instead
            }
            render::BlinnPhongMaterial m;
            m.diffuse = bake(world, object, part.bake, part.bakeName, textures, texels);
            if(!m.diffuse)
            {
                continue; // none, or not drawn until it's in (avoids grey ghosts)
            }
            m.alphaCutoff = 0.5f;             // alpha layers cut holes in bakes
            m.specularTint = glm::vec3(0.0f); // no highlight, as the official avatar shader

            avatar::skinPalette(base_, i, a.joints, a.palettes[i]);
            const render::Mesh* expressed =
                expressedPart(a, bodyShared, i, seen == render::Visibility::Visible);
            render::ObjectDraw draw{expressed ? expressed : &body.meshes[i], root, m};
            draw.joints = a.palettes[i];
            draw.shadowOnly = seen == render::Visibility::ShadowOnly;
            if(!a.shadowJoints.empty())
            {
                // First person: the head shrunk in view, whole in the shadow.
                draw.castsShadow = false;
                scene.objects.push_back(draw);
                avatar::skinPalette(base_, i, a.shadowJoints, a.shadowPalettes[i]);
                draw.joints = a.shadowPalettes[i];
                draw.shadowOnly = true;
                draw.castsShadow = true;
            }
            scene.objects.push_back(draw);
        }
    }

    // Animated objects: a skeleton of the default shape with no body, posed
    // by the object's animations (llcontrolavatar.cpp). Their rigged mesh
    // is skinned to it by ObjectView.
    for(const world::ObjectKey& key : world.animatedKeys())
    {
        const world::Object* found = world.object(key.region, key.localId);
        if(!found || skeletonOwner(world, *found).owner != found)
        {
            continue;
        }
        const world::Object& object = *found;
        Avatar& a = avatars_[object.id];
        a.seen = frame_;
        pose(a, defaultBody_->shape, world, object.id, animations, now, glm::mat4(1.0f),
             originHandle, nullptr);
        animatedObjects_++;
    }

    // Forget avatars and animated objects that are gone.
    std::erase_if(avatars_, [&](const auto& entry) { return entry.second.seen != frame_; });
    std::erase_if(loading_, [&](const auto& entry)
                  { return entry.first != self_ && now - entry.second.seen > kForgetLoaded; });
    // Frames asked for while posing were of the old poses.
    rootFrames_.clear();
    attachmentFrames_.clear();
}

std::vector<AvatarView::Footstep>
AvatarView::footsteps(const world::World& world, uint64_t originHandle, const GroundBelow& ground)
{
    // Walking, running or landing (the server's ids; others' motions map
    // to these too).
    static const Uuid kSteps[] = {*Uuid::parse("6ed24bd8-91aa-4b12-ccc7-c97c857ab4e0"),
                                  *Uuid::parse("05ddbff8-aaa9-92a1-2b74-8fe77a29b445"),
                                  *Uuid::parse("7a17b059-12b2-41b1-570a-186368b6aa6f")};
    std::vector<Footstep> steps;
    for(auto& [id, a] : avatars_)
    {
        if(a.seen != frame_)
        {
            continue;
        }
        const auto* playing = world.animations(id);
        const bool stepping =
            playing && std::ranges::any_of(*playing, [](const world::PlayingAnimation& p)
                                           { return std::ranges::contains(kSteps, p.id); });
        if(!stepping)
        {
            continue;
        }
        const world::Object* object = world.object(a.key.region, a.key.localId);
        if(!object || object->id != id)
        {
            continue;
        }
        // A foot is down within 5 cm of what's under it (looking half a
        // meter up and down).
        auto foot = [&](const char* joint, bool& wasDown)
        {
            const auto at = jointPosition(*object, world, joint, originHandle);
            if(!at)
            {
                return;
            }
            const auto under = ground(*at + glm::vec3(0.0f, 0.0f, 0.5f), 1.0f);
            const float elevation = under ? std::max(-0.2f, at->z - under->height) : 1.0f;
            const bool down = elevation <= 0.05f;
            if(down && !wasDown)
            {
                steps.push_back({id, *at, under->material});
            }
            wasDown = down;
        };
        foot("mFootLeft", a.leftDown);
        foot("mFootRight", a.rightDown);
    }
    return steps;
}

} // namespace viewer
