#pragma once

// Decodes LayerData messages: terrain heights sent as 16x16 patches, each a
// quantized, zig-zag ordered 2D DCT, bit-packed. Same scheme as the official
// viewer (llpatch_code / llsurfacepatch) and libopenmetaverse.

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

// Decodes the LayerData.Data bytes of a land layer. Other layer types (wind,
// cloud, water) decode to an error for now.
std::expected<std::vector<TerrainPatch>, std::string>
decodeLandLayer(uint8_t layerType, std::span<const uint8_t> data);

} // namespace viewer::world
