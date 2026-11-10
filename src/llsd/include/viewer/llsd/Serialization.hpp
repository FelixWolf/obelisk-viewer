#pragma once

// Reading and writing LLSD: XML and binary. Notation is detected (so callers
// get a clear error) but not parsed yet.

#include "viewer/llsd/Value.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace viewer::llsd
{

// Scoped enumerators can't clash with the Date/Binary/Array types, but
// GCC's -Wshadow reports them anyway.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#endif
enum class Format : uint8_t
{
    Xml,
    Notation, // "<?llsd/notation?>" header
    Binary,   // "<?llsd/binary?>" header
};
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

// MIME type used in Content-Type headers.
inline constexpr std::string_view kXmlContentType = "application/llsd+xml";

struct XmlWriteOptions
{
    // Write default values (false, 0, "", null UUID...) as empty elements,
    // e.g. <integer />. Smaller, and every LLSD parser accepts it.
    bool compact = true;
    // Indent nested elements (for logs and files meant for people).
    bool pretty = false;
};

std::string toXml(const Value& value, const XmlWriteOptions& options = {});

std::expected<Value, std::string> fromXml(std::string_view xml);

// Binary LLSD (as llsdserialize.cpp's LLSDBinaryParser/Formatter): one-byte
// type tags, big-endian integers, reals and lengths. `data` starts at the
// first value (no "<?llsd/binary?>" header). If `consumed` is given, it gets
// the number of bytes the value took.
std::expected<Value, std::string> fromBinary(std::span<const uint8_t> data,
                                             size_t* consumed = nullptr);
std::vector<uint8_t> toBinary(const Value& value);

// Detects the serialization from the first bytes of the document.
std::optional<Format> detectFormat(std::string_view data);

// Detects the format and parses it.
std::expected<Value, std::string> parse(std::string_view data);

inline std::string_view asText(std::span<const uint8_t> bytes)
{
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

} // namespace viewer::llsd
