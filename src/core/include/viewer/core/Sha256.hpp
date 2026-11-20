#pragma once

// SHA-256 (FIPS 180-4), to check downloads against the hash they were
// announced with (viewerconfig's home scene).

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace viewer
{

using Sha256Digest = std::array<uint8_t, 32>;

Sha256Digest sha256(std::span<const uint8_t> data);
Sha256Digest sha256(std::string_view text);

// Lowercase hex.
std::string sha256Hex(std::span<const uint8_t> data);

} // namespace viewer
