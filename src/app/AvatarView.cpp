#include "AvatarView.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <format>

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

AvatarView::AvatarView(avatar::BaseAvatar base, render::Renderer& renderer)
    : base_(std::move(base)), renderer_(renderer)
{
    defaultBody_ = build(avatar::ParamWeights(base_));
    defaultJoints_ = avatar::poseJoints(base_, defaultBody_->shape);
}

std::shared_ptr<const AvatarView::Body> AvatarView::build(const avatar::ParamWeights& weights) const
{
    auto body = std::make_shared<Body>();
    body->shape = avatar::shapeAvatar(base_, weights);
    for(size_t part = 0; part < base_.meshes.size(); part++)
    {
        // Morphed but unskinned: the GPU places each vertex from its joints.
        const avatar::ShapedMesh& shaped = body->shape.meshes[part];
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
        body->meshes.push_back(renderer_.createMesh(vertices, base_.meshes[part].indices));
    }
    body->shape.meshes.clear(); // uploaded; the skeleton is what's still needed
    return body;
}

const AvatarView::Body& AvatarView::bodyOf(const world::Object& avatar) const
{
    const auto it = avatars_.find(avatar.id);
    return it == avatars_.end() || !it->second.body ? *defaultBody_ : *it->second.body;
}

glm::mat4 AvatarView::rootFrame(const world::Object& avatar, const world::World& world,
                                uint64_t originHandle) const
{
    const avatar::Shape& shape = bodyOf(avatar).shape;
    const world::World::Appearance* appearance = world.appearance(avatar.id);
    const glm::vec3 hover =
        (appearance ? appearance->hover : glm::vec3(0.0f)) + glm::vec3(0.0f, 0.0f, shape.hover);
    const glm::vec3 root = regionOffset(avatar.region, originHandle) + avatar.position + hover -
                           glm::vec3(0.0f, 0.0f, 0.5f * shape.bodyHeight - shape.pelvisToFoot);
    return glm::translate(glm::mat4(1.0f), root) * glm::mat4_cast(avatar.rotation);
}

std::optional<glm::mat4> AvatarView::attachmentFrame(const world::Object& avatar,
                                                     const world::World& world, int point,
                                                     uint64_t originHandle) const
{
    const auto it = base_.attachmentPoints.find(point);
    if(it == base_.attachmentPoints.end())
    {
        return std::nullopt;
    }
    const avatar::AttachmentPoint& a = it->second;
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
                                           int slot, const char* name,
                                           TextureManager& textures) const
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
    return textures.get(id, std::format("{}texture/{}/{}/{}", appearanceUrl_, avatar.id.toString(),
                                        name, id.toString()));
}

std::optional<const render::GpuTexture*> AvatarView::bakeFor(const world::World& world,
                                                             const world::Object& avatar,
                                                             const Uuid& texture,
                                                             TextureManager& textures) const
{
    for(const BakeSlot& b : kBakeSlots)
    {
        if(b.placeholder == texture)
        {
            return bake(world, avatar, b.slot, b.name, textures);
        }
    }
    return std::nullopt;
}

const std::vector<glm::mat4>& AvatarView::posedJoints(const world::Object& avatar) const
{
    const auto it = avatars_.find(avatar.id);
    return it != avatars_.end() && !it->second.joints.empty() ? it->second.joints : defaultJoints_;
}

void AvatarView::collectOverrides(const world::World& world, MeshManager& meshes)
{
    overrides_.clear();
    const int pelvis = base_.skeleton.find("mPelvis");
    for(const auto& [key, object] : world.objects())
    {
        if(object.type != world::ObjectType::Prim ||
           (object.sculptType & world::SculptType::Mask) != world::SculptType::Mesh ||
           !meshes.isRigged(object.sculptTexture).value_or(false))
        {
            continue;
        }
        const world::Object* avatar = attachedAvatar(world, object);
        if(!avatar)
        {
            continue;
        }
        const auto skin = meshes.skin(object.sculptTexture);
        if(!skin || skin->alternateBind.empty())
        {
            continue;
        }
        Overrides& overrides = overrides_[avatar->id];
        for(size_t j = 0; j < skin->jointNames.size(); j++)
        {
            const int joint = base_.skeleton.find(skin->jointNames[j]);
            // The pelvis stays at the root (its offset is a hover change in
            // the official viewer; not done yet).
            if(joint < 0 || joint == pelvis)
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
            const auto it = overrides.find(joint);
            if(it == overrides.end() || object.sculptTexture < it->second.mesh)
            {
                overrides[joint] = {object.sculptTexture, position, skin->lockScaleIfJointPosition};
            }
        }
    }
}

// Starts and stops animations to match what the simulator says is playing.
void AvatarView::animate(Avatar& a, const world::World& world, const Uuid& id,
                         AnimationManager& animations, double now)
{
    const auto* playing = world.animations(id);
    if(playing)
    {
        for(const world::World::PlayingAnimation& p : *playing)
        {
            const auto it = a.started.find(p.id);
            if(it != a.started.end() && it->second == p.sequence)
            {
                continue;
            }
            auto animation = animations.get(p.id);
            if(!animation)
            {
                continue; // still loading: tried again next frame
            }
            if(it != a.started.end())
            {
                a.animator.stop(p.id, now); // a new sequence number restarts it
            }
            a.animator.start(p.id, std::move(animation), now);
            a.started[p.id] = p.sequence;
        }
    }
    std::erase_if(a.started,
                  [&](const auto& entry)
                  {
                      const bool still =
                          playing && std::ranges::any_of(*playing, [&](const auto& p)
                                                         { return p.id == entry.first; });
                      if(!still)
                      {
                          a.animator.stop(entry.first, now);
                      }
                      return !still;
                  });
}

void AvatarView::addTo(render::Scene& scene, const world::World& world, uint64_t originHandle,
                       TextureManager& textures, AnimationManager& animations, MeshManager& meshes,
                       double now, float drawDistance)
{
    frame_++;
    drawn_ = 0;
    collectOverrides(world, meshes);
    for(const auto& [key, object] : world.objects())
    {
        if(object.type != world::ObjectType::Avatar)
        {
            continue;
        }
        Avatar& a = avatars_[object.id];
        a.seen = frame_;

        // Reshape when the appearance changed.
        const world::World::Appearance* appearance = world.appearance(object.id);
        if(appearance && !appearance->visualParams.empty())
        {
            if(!a.body || a.appearanceSerial != appearance->serial)
            {
                a.body = build(avatar::ParamWeights(base_, appearance->visualParams));
                a.appearanceSerial = appearance->serial;
            }
        }
        const Body& body = a.body ? *a.body : *defaultBody_;

        // Pose: every avatar's, since attachments follow it even when the
        // body is out of sight.
        animate(a, world, object.id, animations, now);
        a.animator.evaluate(now, base_.skeleton.joints.size(), a.pose);
        // Rigged attachments' joint positions replace the shape's, but not
        // animated ones.
        if(const auto o = overrides_.find(object.id); o != overrides_.end())
        {
            for(const auto& [joint, override] : o->second)
            {
                avatar::JointPose& p = a.pose[static_cast<size_t>(joint)];
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
        a.joints = avatar::poseJoints(base_, body.shape, a.pose);

        const glm::mat4 root = rootFrame(object, world, originHandle);
        if(glm::length(glm::vec3(root[3]) - scene.camera.position) > drawDistance)
        {
            continue;
        }
        drawn_++;
        for(size_t i = 0; i < std::size(avatar::kBodyParts); i++)
        {
            const avatar::BodyPart& part = avatar::kBodyParts[i];
            if(!body.meshes[i])
            {
                continue;
            }
            render::BlinnPhongMaterial m;
            m.diffuse = bake(world, object, part.bake, part.bakeName, textures);
            if(!m.diffuse)
            {
                continue; // none, or not drawn until it's in (avoids grey ghosts)
            }
            m.alphaCutoff = 0.5f; // alpha layers cut holes in bakes
            m.specularTint = glm::vec3(0.03f);
            m.maxExponent = 16.0f;

            avatar::skinPalette(base_, i, a.joints, a.palettes[i]);
            render::ObjectDraw draw{&body.meshes[i], root, m};
            draw.joints = a.palettes[i];
            scene.objects.push_back(draw);
        }
    }

    // Forget avatars that are gone.
    std::erase_if(avatars_, [&](const auto& entry) { return entry.second.seen != frame_; });
}

} // namespace viewer
