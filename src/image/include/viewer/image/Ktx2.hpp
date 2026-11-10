#pragma once

// Minimal KTX2 (https://registry.khronos.org/KTX/specs/2.0/ktxspec.v2.html)
// reader/writer for the texture cache. Supports exactly what the cache stores:
// 2D, one layer, one face, VK_FORMAT_R8G8B8A8_UNORM, full mip chain,
// optionally zstd-supercompressed per level.
//
// Viewer-specific metadata goes in key/value entries:
//   viewer.alphaMode     "opaque" | "mask" | "blend"
//   viewer.discardLevel  decimal J2K discard level the pixels came from

#include "viewer/image/TextureData.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace viewer::image
{

struct Ktx2WriteOptions
{
    // 0 = store uncompressed; 1..22 = zstd level (higher: smaller, slower).
    int zstdLevel = 3;
};

std::vector<uint8_t> writeKtx2(const TextureData& texture, const Ktx2WriteOptions& options = {});

struct Ktx2ReadOptions
{
    // Skip mip levels whose width or height exceeds this (0 = keep all).
    // Skipped levels are never decompressed. The smallest level is always kept.
    uint32_t maxDimension = 0;
};

// Returns nullopt for malformed files or formats this reader doesn't handle.
std::optional<TextureData> readKtx2(std::span<const uint8_t> file,
                                    const Ktx2ReadOptions& options = {});

} // namespace viewer::image
