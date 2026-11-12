#pragma once

// PBR materials (glTF 2.0 metallic-roughness), as Second Life uses them
// (llgltfmaterial.cpp): a material asset per face, plus per-face overrides
// the simulator streams separately (GenericStreamingMessage 0x4175).
//
// Texture slots are the official viewer's: base color, normal,
// metallic-roughness, emissive; each with a KHR_texture_transform.

#include "viewer/core/Uuid.hpp"
#include "viewer/llsd/Value.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>

namespace viewer::assets
{

enum class MaterialAlpha : uint8_t
{
    Opaque = 0, // as llgltfmaterial.h's ALPHA_MODE_*
    Blend = 1,
    Mask = 2,
};

enum MaterialTexture : uint8_t
{
    kBaseColorTexture = 0,
    kNormalTexture = 1,
    kMetallicRoughnessTexture = 2,
    kEmissiveTexture = 3,
    kMaterialTextureCount = 4,
};

struct TextureTransform
{
    glm::vec2 offset{0.0f};
    glm::vec2 scale{1.0f};
    float rotation = 0.0f; // radians

    bool operator==(const TextureTransform&) const = default;
};

struct Material
{
    std::array<Uuid, kMaterialTextureCount> textures{};
    std::array<TextureTransform, kMaterialTextureCount> transforms{};
    glm::vec4 baseColor{1.0f};
    glm::vec3 emissive{0.0f};
    float metallic = 1.0f; // glTF defaults
    float roughness = 1.0f;
    MaterialAlpha alphaMode = MaterialAlpha::Opaque;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;

    bool operator==(const Material&) const = default;
};

// A face's changes to its material: only what's set replaces the
// material's value.
struct MaterialOverride
{
    std::array<std::optional<Uuid>, kMaterialTextureCount> textures{};
    std::array<std::optional<glm::vec2>, kMaterialTextureCount> offsets{};
    std::array<std::optional<glm::vec2>, kMaterialTextureCount> scales{};
    std::array<std::optional<float>, kMaterialTextureCount> rotations{};
    std::optional<glm::vec4> baseColor;
    std::optional<glm::vec3> emissive;
    std::optional<float> metallic;
    std::optional<float> roughness;
    std::optional<MaterialAlpha> alphaMode;
    std::optional<float> alphaCutoff;
    std::optional<bool> doubleSided;

    // The override LLSD (applyOverrideLLSD's keys: tex, bc, ec, mf, rf, am,
    // ac, ds, ti).
    static MaterialOverride fromLlsd(const llsd::Value& data);
};

// A texture id meaning "no texture" in overrides (an unset id means "keep
// the material's").
inline const Uuid kOverrideNullTexture = *Uuid::parse("ffffffff-ffff-ffff-ffff-ffffffffffff");

Material applyOverride(Material material, const MaterialOverride& change);

// A material asset as downloaded: LLSD {version 1.0/1.1, type "GLTF 2.0",
// data: glTF JSON}; the first material in it. Textures are image URIs
// holding asset ids.
std::expected<Material, std::string> parseMaterialAsset(std::span<const uint8_t> bytes);

// The glTF JSON alone.
std::expected<Material, std::string> parseGltfMaterial(std::string_view json);

} // namespace viewer::assets
