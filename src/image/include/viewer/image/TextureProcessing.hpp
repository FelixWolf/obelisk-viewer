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

// Decode + RGBA expansion + alpha classification + mips.
std::optional<TextureData> textureFromJ2k(const J2kDecoder& decoder, std::span<const uint8_t> j2k,
                                          uint32_t discardLevel = 0);

} // namespace viewer::image
