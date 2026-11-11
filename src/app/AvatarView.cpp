#include "AvatarView.hpp"

#include "FaceUpload.hpp"

#include <glm/gtc/matrix_transform.hpp>

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
}

AvatarView::Body AvatarView::build(const avatar::ParamWeights& weights) const
{
    Body body;
    body.shape = avatar::shapeAvatar(base_, weights);
    std::vector<volume::VolumeFace> faces;
    for(size_t i = 0; i < base_.meshes.size(); i++)
    {
        avatar::ShapedMesh& shaped = body.shape.meshes[i];
        volume::VolumeFace face;
        face.positions = std::move(shaped.positions);
        face.normals = std::move(shaped.normals);
        face.texCoords = std::move(shaped.texCoords);
        face.indices = base_.meshes[i].indices;
        faces.push_back(std::move(face));
    }
    body.shape.meshes.clear(); // uploaded; only the skeleton is kept
    body.meshes = uploadFaces(renderer_, faces);
    return body;
}

const AvatarView::Body& AvatarView::bodyOf(const world::Object& avatar) const
{
    const auto it = bodies_.find(avatar.id);
    return it == bodies_.end() ? defaultBody_ : it->second;
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
    const avatar::Shape& shape = bodyOf(avatar).shape;
    glm::mat4 joint(1.0f);
    if(a.joint >= 0)
    {
        // The joint's position and rotation. Its scale doesn't carry over to
        // what's attached, but scales the point's offset (as llxform.cpp does
        // for child joints).
        const glm::mat4& m = shape.joints[static_cast<size_t>(a.joint)];
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

void AvatarView::addTo(render::Scene& scene, const world::World& world, uint64_t originHandle,
                       TextureManager& textures, float drawDistance)
{
    frame_++;
    drawn_ = 0;
    for(const auto& [key, object] : world.objects())
    {
        if(object.type != world::ObjectType::Avatar)
        {
            continue;
        }

        // Reshape when the appearance changed.
        const world::World::Appearance* appearance = world.appearance(object.id);
        if(appearance && !appearance->visualParams.empty())
        {
            auto it = bodies_.find(object.id);
            if(it == bodies_.end() || it->second.appearanceSerial != appearance->serial)
            {
                Body body = build(avatar::ParamWeights(base_, appearance->visualParams));
                body.appearanceSerial = appearance->serial;
                it = bodies_.insert_or_assign(object.id, std::move(body)).first;
            }
            it->second.seen = frame_;
        }

        const glm::mat4 root = rootFrame(object, world, originHandle);
        if(glm::length(glm::vec3(root[3]) - scene.camera.position) > drawDistance)
        {
            continue;
        }
        // Bakes from AvatarAppearance if we have it, else the object's own.
        const world::TextureEntry& faces = appearance ? appearance->faces : object.faces;
        const Body& body = bodyOf(object);

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
            scene.objects.push_back(render::ObjectDraw{&body.meshes[i], root, m});
        }
    }

    // Forget avatars that are gone.
    std::erase_if(bodies_, [&](const auto& entry) { return entry.second.seen != frame_; });
}

} // namespace viewer
