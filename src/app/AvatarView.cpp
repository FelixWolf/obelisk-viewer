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

} // namespace

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
                       TextureManager& textures, AnimationManager& animations, double now,
                       float drawDistance)
{
    frame_++;
    drawn_ = 0;
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
        a.joints = avatar::poseJoints(base_, body.shape, a.pose);

        const glm::mat4 root = rootFrame(object, world, originHandle);
        if(glm::length(glm::vec3(root[3]) - scene.camera.position) > drawDistance)
        {
            continue;
        }
        // Bakes from AvatarAppearance if we have it, else the object's own.
        const world::TextureEntry& faces = appearance ? appearance->faces : object.faces;

        drawn_++;
        for(size_t i = 0; i < std::size(avatar::kBodyParts); i++)
        {
            const avatar::BodyPart& part = avatar::kBodyParts[i];
            const Uuid& bake = faces[static_cast<size_t>(part.bake)].texture;
            if(bake.isNull() || bake == kDefaultAvatarTexture || bake == kInvisibleTexture ||
               !body.meshes[i])
            {
                continue;
            }
            render::BlinnPhongMaterial m;
            if(!appearanceUrl_.empty())
            {
                // As the official viewer's getImageURL().
                m.diffuse = textures.get(bake, std::format("{}texture/{}/{}/{}", appearanceUrl_,
                                                           object.id.toString(), part.bakeName,
                                                           bake.toString()));
            }
            if(!m.diffuse)
            {
                continue; // not drawn until its bake is in (avoids grey ghosts)
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
