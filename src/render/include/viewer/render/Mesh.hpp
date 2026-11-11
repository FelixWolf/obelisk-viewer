#pragma once

#include <SDL3/SDL_gpu.h>

#include <glm/ext/vector_uint4_sized.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <utility>

namespace viewer::render
{

// Vertex layout for objects (Blinn-Phong and PBR materials).
struct ObjectVertex
{
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f}; // xyz tangent, w bitangent sign (MikkTSpace)
    glm::vec2 uv{0.0f};
};

// Vertex layout for skinned objects: an object vertex plus up to four joints
// (indices into the draw's joint matrices) and their weights (summing to 1).
struct SkinnedVertex
{
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
    glm::vec2 uv{0.0f};
    glm::u8vec4 joints{0};
    glm::vec4 weights{1.0f, 0.0f, 0.0f, 0.0f};
};

// Vertex layout for terrain. Texture coordinates come from the world position.
struct TerrainVertex
{
    glm::vec3 position{0.0f}; // region-local meters
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    // x: layer value 0..3 (which of the four detail layers), y: blend noise
    // 0..1 (how sharp the transition between layers is here).
    glm::vec2 blend{0.0f, 0.5f};
};

enum class VertexLayout : uint8_t
{
    Object,
    Skinned,
    Terrain,
};

class MeshPool;
struct MeshBlock;

// Indexed triangle list on the GPU (32-bit indices, relative to the mesh's
// first vertex): a range of a MeshPool block's buffers. Move-only; must not
// outlive its Renderer. Destroying it while the GPU may still draw it is
// safe (the pool reuses the range only frames later).
class Mesh
{
public:
    Mesh() = default;
    ~Mesh() { reset(); }
    Mesh(Mesh&& other) noexcept { *this = std::move(other); }
    Mesh& operator=(Mesh&& other) noexcept;
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;

    void reset();

    explicit operator bool() const { return block != nullptr && indexCount > 0; }

    SDL_GPUBuffer* vertexBuffer() const;
    SDL_GPUBuffer* indexBuffer() const;

    MeshPool* pool = nullptr;
    MeshBlock* block = nullptr;
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    VertexLayout layout = VertexLayout::Object;
};

} // namespace viewer::render
