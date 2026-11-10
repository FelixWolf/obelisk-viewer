#include "ObjectView.hpp"

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

// Tangents along +u (Lengyel's method), for normal maps.
std::vector<glm::vec4> tangents(const volume::VolumeFace& face)
{
    std::vector<glm::vec3> tan(face.positions.size(), glm::vec3(0.0f));
    std::vector<glm::vec3> bitan(face.positions.size(), glm::vec3(0.0f));
    for(size_t i = 0; i + 2 < face.indices.size(); i += 3)
    {
        const uint32_t i0 = face.indices[i];
        const uint32_t i1 = face.indices[i + 1];
        const uint32_t i2 = face.indices[i + 2];
        const glm::vec3 e1 = face.positions[i1] - face.positions[i0];
        const glm::vec3 e2 = face.positions[i2] - face.positions[i0];
        const glm::vec2 d1 = face.texCoords[i1] - face.texCoords[i0];
        const glm::vec2 d2 = face.texCoords[i2] - face.texCoords[i0];
        const float det = d1.x * d2.y - d2.x * d1.y;
        if(std::abs(det) < 1e-12f)
        {
            continue;
        }
        const float r = 1.0f / det;
        const glm::vec3 sdir = (e1 * d2.y - e2 * d1.y) * r;
        const glm::vec3 tdir = (e2 * d1.x - e1 * d2.x) * r;
        for(const uint32_t v : {i0, i1, i2})
        {
            tan[v] += sdir;
            bitan[v] += tdir;
        }
    }
    std::vector<glm::vec4> out(face.positions.size());
    for(size_t v = 0; v < out.size(); v++)
    {
        const glm::vec3& n = face.normals[v];
        glm::vec3 t = tan[v] - n * glm::dot(n, tan[v]);
        if(glm::dot(t, t) < 1e-12f)
        {
            out[v] = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
            continue;
        }
        t = glm::normalize(t);
        const float handedness = glm::dot(glm::cross(n, t), bitan[v]) < 0.0f ? -1.0f : 1.0f;
        out[v] = glm::vec4(t, handedness);
    }
    return out;
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
        geo.faces.clear();
        for(const volume::VolumeFace& face : g.faces)
        {
            if(face.indices.empty())
            {
                geo.faces.emplace_back();
                continue;
            }
            const auto tan = tangents(face);
            std::vector<render::ObjectVertex> vertices(face.positions.size());
            for(size_t v = 0; v < vertices.size(); v++)
            {
                vertices[v] = {face.positions[v], face.normals[v], tan[v], face.texCoords[v]};
            }
            geo.faces.push_back(renderer_.createMesh(
                std::span<const render::ObjectVertex>(vertices), face.indices));
        }
        geo.ready = true;
    }
    drained_.clear();
}

void ObjectView::addTo(render::Scene& scene, const world::World& world, uint64_t originHandle,
                       TextureManager& textures, float drawDistance)
{
    stats_.drawn = 0;
    stats_.faces = 0;
    stats_.skipped = 0;
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
        if(sculpt != world::SculptType::None)
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

        const Key k{object.volume, detailFor(radius, distance)};
        Geometry& geo = geometry(k);
        if(!geo.ready)
        {
            continue;
        }

        const glm::mat4 transform = glm::translate(glm::mat4(1.0f), position) *
                                    glm::mat4_cast(rotation) *
                                    glm::scale(glm::mat4(1.0f), object.scale);
        stats_.drawn++;
        for(size_t f = 0; f < geo.faces.size() && f < world::kMaxFaces; f++)
        {
            const render::Mesh& mesh = geo.faces[f];
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
