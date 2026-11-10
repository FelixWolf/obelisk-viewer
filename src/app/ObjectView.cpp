#include "ObjectView.hpp"

#include "FaceUpload.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <thread>

namespace viewer
{

namespace
{

// Faces whose texture is the official viewer's fully transparent one aren't
// drawn (asset id from indra_constants.cpp).
const Uuid kTransparentTexture = *Uuid::parse("8dcd4a48-2d37-4909-9f78-f7a9eb4ef903");

// Level of detail from how large the prim appears, as the official viewer's
// LLVolumeLODGroup::getDetailFromTan with its default thresholds and a LOD
// factor of 1.25.
int detailFor(float radius, float distance)
{
    constexpr float kThresholds[] = {0.03f, 0.06f, 0.24f, 3.0f};
    constexpr float kLodFactor = 1.25f;
    const float tanAngle = radius / std::max(distance, 0.01f) * kLodFactor;
    for(int i = 0; i < 4; i++)
    {
        if(tanAngle <= kThresholds[i])
        {
            return i;
        }
    }
    return 3;
}

// Texture entry transform as the official viewer's llface.cpp xform(): about
// the face's center, rotate, then scale, then offset.
void uvTransform(const world::TextureFace& face, glm::vec3& row0, glm::vec3& row1)
{
    const float c = std::cos(face.rotation);
    const float s = std::sin(face.rotation);
    const float a = c * face.repeats.x;
    const float b = s * face.repeats.x;
    const float d = -s * face.repeats.y;
    const float e = c * face.repeats.y;
    row0 = glm::vec3(a, b, -0.5f * a - 0.5f * b + face.offset.x + 0.5f);
    row1 = glm::vec3(d, e, -0.5f * d - 0.5f * e + face.offset.y + 0.5f);
}

} // namespace

ObjectView::ObjectView(JobSystem& jobs, render::Renderer& renderer)
    : jobs_(jobs), renderer_(renderer)
{
}

ObjectView::~ObjectView()
{
    // Jobs push into generated_; wait until none is running.
    while(running_.load() > 0)
    {
        std::this_thread::yield();
    }
}

void ObjectView::clear()
{
    cache_.clear();
}

ObjectView::Geometry& ObjectView::geometry(const Key& key)
{
    auto [it, inserted] = cache_.try_emplace(key);
    if(inserted)
    {
        running_++;
        jobs_.submit(
            [this, key]
            {
                generated_.push(
                    {key, volume::generateVolume(
                              key.params, volume::kDetailScales[static_cast<size_t>(key.detail)])});
                running_--;
            });
    }
    return it->second;
}

void ObjectView::update()
{
    generated_.drain(drained_);
    for(Generated& g : drained_)
    {
        Geometry& geo = cache_[g.key];
        geo.faces = uploadFaces(renderer_, g.faces);
        geo.ready = true;
    }
    drained_.clear();
}

void ObjectView::addTo(render::Scene& scene, const world::World& world, uint64_t originHandle,
                       TextureManager& textures, MeshManager& meshes, float drawDistance)
{
    stats_.drawn = 0;
    stats_.faces = 0;
    stats_.skipped = 0;
    stats_.meshObjects = 0;
    const glm::vec3 camera = scene.camera.position;
    const glm::vec3 forward = glm::normalize(scene.camera.target - scene.camera.position);
    const auto originX = static_cast<int64_t>(originHandle >> 32);
    const auto originY = static_cast<int64_t>(originHandle & 0xFFFFFFFFu);

    for(const auto& [key, object] : world.objects())
    {
        if(object.type != world::ObjectType::Prim || !object.hasVolume)
        {
            continue;
        }
        const uint8_t sculpt = object.sculptType & world::SculptType::Mask;
        const bool isMesh = sculpt == world::SculptType::Mesh;
        if(sculpt != world::SculptType::None && !isMesh)
        {
            stats_.skipped++;
            continue;
        }

        // Linked children are relative to their root.
        glm::vec3 position = object.position;
        glm::quat rotation = object.rotation;
        if(object.parentId != 0)
        {
            const world::Object* parent = world.object(object.region, object.parentId);
            if(!parent || parent->type != world::ObjectType::Prim)
            {
                stats_.skipped++; // attachments (parent is an avatar) or parent unknown yet
                continue;
            }
            position = parent->position + parent->rotation * object.position;
            rotation = parent->rotation * object.rotation;
        }
        position += glm::vec3(
            static_cast<float>(static_cast<int64_t>(object.region >> 32) - originX),
            static_cast<float>(static_cast<int64_t>(object.region & 0xFFFFFFFFu) - originY), 0.0f);

        // Distance and a rough "behind the camera" cull.
        const float radius = glm::length(object.scale) * 0.5f;
        const glm::vec3 toObject = position - camera;
        const float distance = glm::length(toObject);
        if(distance - radius > drawDistance || glm::dot(toObject, forward) < -radius)
        {
            continue;
        }

        const int detail = detailFor(radius, distance);
        const std::vector<render::Mesh>* faces = nullptr;
        if(isMesh)
        {
            faces = meshes.get(object.sculptTexture, detail);
            stats_.meshObjects += faces != nullptr;
        }
        else
        {
            Geometry& geo = geometry(Key{object.volume, detail});
            faces = geo.ready ? &geo.faces : nullptr;
        }
        if(!faces)
        {
            continue;
        }

        const glm::mat4 transform = glm::translate(glm::mat4(1.0f), position) *
                                    glm::mat4_cast(rotation) *
                                    glm::scale(glm::mat4(1.0f), object.scale);
        stats_.drawn++;
        for(size_t f = 0; f < faces->size() && f < world::kMaxFaces; f++)
        {
            const render::Mesh& mesh = (*faces)[f];
            const world::TextureFace& face = object.faces[f];
            if(!mesh || face.color.a <= 0.0f || face.texture == kTransparentTexture)
            {
                continue;
            }
            render::BlinnPhongMaterial m;
            m.diffuse = textures.get(face.texture);
            m.diffuseTint = face.color;
            m.fullbright = face.fullbright();
            // Legacy faces are matte unless shiny; keep a faint highlight.
            m.specularTint = glm::vec3(0.04f + 0.3f * static_cast<float>(face.shiny()));
            m.maxExponent = 32.0f + 96.0f * static_cast<float>(face.shiny());
            const auto alpha = textures.alphaMode(face.texture);
            if(alpha == image::AlphaMode::Mask)
            {
                m.alphaCutoff = 0.5f;
            }
            m.blend = face.color.a < 1.0f || alpha == image::AlphaMode::Blend;

            render::ObjectDraw draw{&mesh, transform, m};
            uvTransform(face, draw.uvRow0, draw.uvRow1);
            scene.objects.push_back(draw);
            stats_.faces++;
        }
    }

    stats_.meshes = cache_.size();
    stats_.pending = static_cast<size_t>(std::max(running_.load(), 0));
}

} // namespace viewer
