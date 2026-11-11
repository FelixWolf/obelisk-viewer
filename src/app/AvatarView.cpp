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
    : base_(std::move(base)), restPose_(base_.skeleton.restPose())
{
    std::vector<volume::VolumeFace> faces;
    for(const avatar::BodyMesh& mesh : base_.meshes)
    {
        volume::VolumeFace face;
        face.positions = mesh.positions;
        face.normals = mesh.normals;
        face.texCoords = mesh.texCoords;
        face.indices = mesh.indices;
        faces.push_back(std::move(face));
    }
    meshes_ = uploadFaces(renderer, faces);
}

glm::mat4 AvatarView::rootFrame(const world::Object& avatar, uint64_t originHandle) const
{
    const glm::vec3 root = regionOffset(avatar.region, originHandle) + avatar.position -
                           glm::vec3(0.0f, 0.0f, 0.5f * base_.bodyHeight - base_.pelvisToFoot);
    return glm::translate(glm::mat4(1.0f), root) * glm::mat4_cast(avatar.rotation);
}

std::optional<glm::mat4> AvatarView::attachmentFrame(const world::Object& avatar, int point,
                                                     uint64_t originHandle) const
{
    const auto it = base_.attachmentPoints.find(point);
    if(it == base_.attachmentPoints.end())
    {
        return std::nullopt;
    }
    const avatar::AttachmentPoint& a = it->second;
    const glm::mat4 joint = a.joint < 0 ? glm::mat4(1.0f) : restPose_[static_cast<size_t>(a.joint)];
    return rootFrame(avatar, originHandle) * joint * glm::translate(glm::mat4(1.0f), a.position) *
           glm::mat4_cast(a.rotation);
}

void AvatarView::addTo(render::Scene& scene, const world::World& world, uint64_t originHandle,
                       TextureManager& textures, float drawDistance)
{
    drawn_ = 0;
    for(const auto& [key, object] : world.objects())
    {
        if(object.type != world::ObjectType::Avatar)
        {
            continue;
        }
        const glm::mat4 root = rootFrame(object, originHandle);
        if(glm::length(glm::vec3(root[3]) - scene.camera.position) > drawDistance)
        {
            continue;
        }
        // Bakes from AvatarAppearance if we have it, else the object's own.
        const world::TextureEntry* appearance = world.appearance(object.id);
        const world::TextureEntry& faces = appearance ? *appearance : object.faces;

        drawn_++;
        const glm::mat4 bodySpace = root * glm::translate(glm::mat4(1.0f), -base_.pelvisBind);
        for(size_t i = 0; i < std::size(avatar::kBodyParts); i++)
        {
            const avatar::BodyPart& part = avatar::kBodyParts[i];
            const Uuid& bake = faces[static_cast<size_t>(part.bake)].texture;
            if(bake.isNull() || bake == kDefaultAvatarTexture || bake == kInvisibleTexture ||
               !meshes_[i])
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

            glm::mat4 transform = bodySpace;
            if(part.joint)
            {
                const int joint = base_.skeleton.find(part.joint);
                if(joint < 0)
                {
                    continue;
                }
                transform = root * restPose_[static_cast<size_t>(joint)];
            }
            scene.objects.push_back(render::ObjectDraw{&meshes_[i], transform, m});
        }
    }
}

} // namespace viewer
