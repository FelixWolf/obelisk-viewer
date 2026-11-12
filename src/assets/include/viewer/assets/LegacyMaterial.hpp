#pragma once

// Legacy (pre-PBR) materials, as the official viewer's LLMaterial: normal
// and specular maps (each with its own repeats, offset and rotation),
// specular color and glossiness, environment intensity, and how the
// diffuse texture's alpha is used. A texture entry face names one by id
// (16 bytes); regions hand them out through the RenderMaterials
// capability (llmaterialmgr.cpp): POST {"Zipped": zlib(binary LLSD [id
// as a 16-byte binary, ...])}, answered {"Zipped": zlib(binary LLSD [{ID,
// Material: {NormMap, ...}}, ...])}.

#include "viewer/assets/Material.hpp"
#include "viewer/core/Uuid.hpp"
#include "viewer/llsd/Value.hpp"

#include <glm/vec4.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace viewer::assets
{

// What the diffuse texture's alpha means (llmaterial.h).
enum class DiffuseAlphaMode : uint8_t
{
    None = 0,     // opaque: alpha ignored
    Blend = 1,    // see-through
    Mask = 2,     // cut out below the cutoff
    Emissive = 3, // opaque; alpha is how much shows unlit (glows)
    Default = 4,  // as the texture's own alpha suggests
};

struct LegacyMaterial
{
    Uuid normalMap{};
    TextureTransform normalTransform{}; // offset, scale (repeats), rotation in radians
    Uuid specularMap{};
    TextureTransform specularTransform{};
    glm::vec4 specularColor{1.0f}; // sRGB, 0..1
    float glossiness = 0.2f;       // specular exponent / 255
    float environment = 0.0f;      // environment intensity / 255
    DiffuseAlphaMode alphaMode = DiffuseAlphaMode::Blend;
    float alphaCutoff = 0.0f; // mask cutoff / 255
};

// One material's LLSD (LLMaterial::fromLLSD; fixed point: x 10000).
LegacyMaterial legacyMaterialFromLlsd(const llsd::Value& data);

// The POST body asking for these materials.
llsd::Value legacyMaterialRequest(std::span<const Uuid> ids);

// The materials in a RenderMaterials answer.
std::expected<std::vector<std::pair<Uuid, LegacyMaterial>>, std::string>
parseLegacyMaterials(const llsd::Value& response);

// zlib-compresses `data` (as zip_llsd).
std::vector<uint8_t> deflateZlib(std::span<const uint8_t> data);

} // namespace viewer::assets
