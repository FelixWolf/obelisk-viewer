#pragma once

// Material descriptions for the four shader families. Textures are borrowed
// (the caller keeps them alive until the frame is submitted); nullptr means
// the neutral default (white, flat normal, black emission...).
//
// Color textures (diffuse, albedo, emissive, specular) should be uploaded as
// sRGB; data textures (normal, ORM) as UNORM.

#include "viewer/render/GpuTexture.hpp"

#include <glm/glm.hpp>

#include <array>

namespace viewer::render
{

// Default (legacy) objects.
struct BlinnPhongMaterial
{
    const GpuTexture* diffuse = nullptr;  // RGB color, A opacity
    bool diffuseLoading = false;          // no diffuse yet, but one is coming: gray till then
    const GpuTexture* normal = nullptr;   // RGB normal, A glossiness
    const GpuTexture* specular = nullptr; // RGB specular color, A environment intensity
    glm::vec4 diffuseTint{1.0f};
    glm::vec3 specularTint{1.0f};
    float maxExponent = 128.0f; // specular exponent where the normal map's A is 1 (times A²)
    float alphaCutoff = -1.0f;  // < 0: no alpha test
    bool fullbright = false;    // unlit: the texture's color as is
    bool blend = false;         // alpha-blended (drawn after opaque, back to front)
    bool emissiveMask = false;  // the diffuse alpha is how much shows unlit (opaque)
    bool opaque = false;        // the diffuse alpha is ignored
    // The normal and specular maps' own texture transforms (rows, as
    // ObjectDraw::uvRow0/1, applied to the mesh's coordinates); without
    // them they follow the diffuse map's.
    bool mapTransforms = false;
    std::array<glm::vec3, 2> normalUv{glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)};
    std::array<glm::vec3, 2> specularUv{glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)};
};

// PBR objects (glTF metallic-roughness).
struct PbrMaterial
{
    const GpuTexture* albedo = nullptr;   // RGB base color, A opacity
    bool albedoLoading = false;           // no albedo yet, but one is coming: gray till then
    const GpuTexture* orm = nullptr;      // R occlusion, G roughness, B metallic
    const GpuTexture* emissive = nullptr; // RGB emission; none: the tint alone (as glTF)
    const GpuTexture* normal = nullptr;   // MikkTSpace normal map
    glm::vec4 baseColorFactor{1.0f};
    glm::vec3 emissiveTint{0.0f};
    float emissiveStrength = 1.0f;
    float roughness = 1.0f; // multiplies the map's G
    float metallic = 1.0f;  // multiplies the map's B
    float occlusionStrength = 1.0f;
    float alphaCutoff = -1.0f;
    bool blend = false;       // alpha-blended (drawn after opaque, back to front)
    bool doubleSided = false; // both sides drawn (else back faces are culled)
};

// Classic terrain: one 4-layer array of diffuse textures.
struct TerrainBlinnPhongMaterial
{
    const GpuTexture* diffuseLayers = nullptr;   // 2D array, 4 layers
    float textureRepeatsPerMeter = 1.0f / 12.0f; // official viewer default: 12 m
};

// PBR terrain: four PBR materials, each map as a 4-layer array. Sampled
// triplanar, so steep slopes don't stretch the textures.
struct TerrainPbrMaterial
{
    const GpuTexture* albedoLayers = nullptr;
    const GpuTexture* ormLayers = nullptr;
    const GpuTexture* emissiveLayers = nullptr;
    const GpuTexture* normalLayers = nullptr;
    struct Layer
    {
        glm::vec4 baseColorFactor{1.0f};
        glm::vec3 emissiveTint{1.0f};
        float emissiveStrength = 1.0f;
        float roughness = 1.0f;
        float metallic = 1.0f;
        float occlusionStrength = 1.0f;
    };
    std::array<Layer, 4> layers{};
    float textureRepeatsPerMeter = 1.0f / 8.0f; // official viewer default: 8 m
};

} // namespace viewer::render
