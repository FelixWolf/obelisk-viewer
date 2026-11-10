#pragma once

#include "viewer/image/TextureData.hpp"

#include <filesystem>

namespace viewer::image
{

// Writes RGBA8 pixels as a PNG (uncompressed deflate blocks: simple, large;
// meant for screenshots and debugging). Returns false on I/O failure.
bool writePng(const std::filesystem::path& path, const MipLevel& rgba);

} // namespace viewer::image
