#pragma once

// CPU-side texture preparation: everything between "J2K bytes" and
// "TextureData ready to cache". All functions are pure and thread-safe.

#include "viewer/image/Image.hpp"
#include "viewer/image/J2kDecoder.hpp"
#include "viewer/image/TextureData.hpp"

#include <optional>
#include <span>

namespace viewer::image
{

// Expands a 1-4 component image to RGBA8.
// 1 = luminance, 2 = luminance + alpha, 3 = RGB (alpha = 255), 4 = RGBA.
MipLevel toRgba8(const Image& image);

AlphaMode classifyAlpha(const MipLevel& rgba);

// Builds the full mip chain down to 1x1 with a 2x2 box filter.
// Note: filters the stored bytes directly (no sRGB linearization).
std::vector<MipLevel> buildMipChain(MipLevel base);

// Resamples RGBA8 to the given size (bilinear; averages when shrinking by
// more than 2x). Bytes are filtered as stored (no sRGB linearization).
MipLevel resizeRgba8(const MipLevel& source, uint32_t width, uint32_t height);

// Reverses the row order of every mip level. Second Life texture
// coordinates assume rows stored bottom-up (v = 0 is the image's bottom row,
// as the official viewer stores them); decoded images are top-down.
void flipRows(TextureData& texture);

// A J2K codestream's size and how many times its resolution halves
// (decomposition levels), from its main header (SIZ, COD). Raw codestreams
// or JP2 files; nothing if there's no readable header.
struct J2kInfo
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t levels = 0; // discard levels 0..levels are decodable
};
std::optional<J2kInfo> readJ2kInfo(std::span<const uint8_t> j2k);

// The coarsest discard level whose larger side is still at least `wanted`
// (0: full resolution).
uint32_t discardLevelFor(const J2kInfo& info, uint32_t wanted);

// Decode + RGBA expansion + alpha classification + mips.
std::optional<TextureData> textureFromJ2k(const J2kDecoder& decoder, std::span<const uint8_t> j2k,
                                          uint32_t discardLevel = 0);

} // namespace viewer::image
