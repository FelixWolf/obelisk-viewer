#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace viewer
{

// 128-bit identifier as used by Second Life for assets, agents, objects, etc.
// Stored in the canonical (network/string) byte order.
struct Uuid
{
    std::array<uint8_t, 16> bytes{};

    // Parses "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (case-insensitive).
    static std::optional<Uuid> parse(std::string_view text);

    // Lowercase, hyphenated, 36 characters.
    std::string toString() const;

    bool isNull() const;

    auto operator<=>(const Uuid&) const = default;
};

} // namespace viewer

template <>
struct std::hash<viewer::Uuid>
{
    size_t operator()(const viewer::Uuid& id) const noexcept
    {
        // UUIDs are already uniformly distributed; fold the two halves.
        uint64_t a = 0;
        uint64_t b = 0;
        std::memcpy(&a, id.bytes.data(), 8);
        std::memcpy(&b, id.bytes.data() + 8, 8);
        return static_cast<size_t>(a ^ (b * 0x9E3779B97F4A7C15ull));
    }
};
