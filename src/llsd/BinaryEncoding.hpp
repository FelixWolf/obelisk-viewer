#pragma once

// Text encodings for LLSD binary values (internal to the llsd module).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace viewer::llsd::detail
{

std::string encodeBase64(const std::vector<uint8_t>& data);

// Whitespace is ignored. Returns nullopt on invalid characters or length.
std::optional<std::vector<uint8_t>> decodeBase64(std::string_view text);
std::optional<std::vector<uint8_t>> decodeBase16(std::string_view text);
// RFC 1924 alphabet (same as Python's base64.b85decode).
std::optional<std::vector<uint8_t>> decodeBase85(std::string_view text);

} // namespace viewer::llsd::detail
