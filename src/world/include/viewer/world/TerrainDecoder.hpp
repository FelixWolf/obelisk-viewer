#pragma once

// Decodes LayerData messages: terrain heights sent as 16x16 patches, each a
// quantized, zig-zag ordered 2D DCT, bit-packed. Same scheme as the official
// viewer (llpatch_code / llsurfacepatch) and libopenmetaverse.

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace viewer::world
{

inline constexpr uint32_t kPatchSize = 16;

// LayerData.LayerID.Type
enum class LayerType : uint8_t
{
    Land = 'L',
    LandExtended = 'M', // variable-size regions: 32-bit patch ids
    Water = 'W',
    WaterExtended = 'X',
    Wind = '7',
    WindExtended = '9',
    Cloud = '8',
    CloudExtended = ':',
};

struct TerrainPatch
{
    uint32_t x = 0;                                       // patch column (16 m each)
    uint32_t y = 0;                                       // patch row
    std::array<float, kPatchSize * kPatchSize> heights{}; // row-major, meters
};

// Decodes the LayerData.Data bytes of a land layer.
std::expected<std::vector<TerrainPatch>, std::string>
decodeLandLayer(uint8_t layerType, std::span<const uint8_t> data);

// A region's wind (llwind.cpp LLWind): east and north velocity on a 16 by
// 16 grid over the region, from its south-west corner, row by row.
struct WindField
{
    std::array<float, kPatchSize * kPatchSize> x{};
    std::array<float, kPatchSize * kPatchSize> y{};
};

// Decodes a wind layer: two patches, the x velocities then the y.
std::expected<WindField, std::string> decodeWindLayer(uint8_t layerType,
                                                      std::span<const uint8_t> data);

// The wind at a point in a region `width` meters across (meters from its
// south-west corner; z ignored): between the grid's points, times the
// official viewer's WIND_SCALE_HACK of 2 (LLWind::getVelocity). Horizontal.
glm::vec3 windVelocity(const WindField& wind, glm::vec2 position, float width);

} // namespace viewer::world
