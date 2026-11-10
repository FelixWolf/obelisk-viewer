#pragma once

#include "viewer/render/GpuBuffer.hpp"

#include <glm/glm.hpp>

#include <cstdint>

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

// Vertex layout for terrain. Texture coordinates come from the world position.
struct TerrainVertex
{
    glm::vec3 position{0.0f}; // region-local meters
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    float composition = 0.0f; // 0..3: which of the four detail layers
};

enum class VertexLayout : uint8_t
{
    Object,
    Terrain,
};

// Indexed triangle list on the GPU (32-bit indices).
struct Mesh
{
    GpuBuffer vertices;
    GpuBuffer indices;
    uint32_t indexCount = 0;
    VertexLayout layout = VertexLayout::Object;

    explicit operator bool() const { return vertices && indices && indexCount > 0; }
};

} // namespace viewer::render
