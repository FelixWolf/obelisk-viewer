#pragma once

// MD5, for protocol compatibility only (the SL login password hash and id0).
// It is not a security primitive; don't use it as one.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace viewer
{

using Md5Digest = std::array<uint8_t, 16>;

Md5Digest md5(std::span<const uint8_t> data);
Md5Digest md5(std::string_view text);

// Lowercase hex, two characters per byte.
std::string toHex(std::span<const uint8_t> bytes);

inline std::string md5Hex(std::string_view text)
{
    return toHex(md5(text));
}

} // namespace viewer
