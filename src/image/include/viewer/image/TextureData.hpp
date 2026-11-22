#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace viewer::image
{

// How a texture's alpha channel needs to be rendered. Computed once when the
// texture is decoded and stored in the cache, so nothing re-scans pixels.
enum class AlphaMode : uint8_t
{
    Opaque,      // no alpha channel, or every alpha value is 255
    Mask,        // nearly all alpha near 0 or 255 (alpha test will do: no sorting)
    Blend,       // partial transparency (needs blending)
    Transparent, // every alpha value is 0: nothing shows
};

std::string_view alphaModeName(AlphaMode mode);

// One mip level of RGBA8 pixels, rows tightly packed, top row first.
struct MipLevel
{
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels; // width * height * 4

    size_t byteSize() const { return pixels.size(); }
};

// One 8-bit channel, rows tightly packed.
struct Plane
{
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels; // width * height

    bool empty() const { return pixels.empty(); }
};

// A texture ready to cache or upload: RGBA8, full mip chain, largest level
// first. Bytes are not tagged sRGB or linear; the renderer picks the GPU
// format at upload time based on how the texture is used.
struct TextureData
{
    std::vector<MipLevel> levels;
    AlphaMode alphaMode = AlphaMode::Opaque;

    // Resolution level the J2K source was decoded at (0 = full resolution).
    // A cache entry with discardLevel > 0 can later be replaced by a
    // higher-resolution decode.
    uint32_t discardLevel = 0;
    // The source image's size at full resolution (these levels times
    // 2^discardLevel, as J2K resolution levels halve it). 0: unknown.
    uint32_t fullWidth = 0;
    uint32_t fullHeight = 0;
    // An avatar bake's morph mask (its J2K's fifth component) at the first
    // level's size, rows in the levels' order; empty for other textures.
    // Not in the KTX2: the cache keeps it beside it.
    Plane aux;

    uint32_t width() const { return levels.empty() ? 0 : levels.front().width; }
    uint32_t height() const { return levels.empty() ? 0 : levels.front().height; }
};

} // namespace viewer::image
