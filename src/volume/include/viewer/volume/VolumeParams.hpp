#pragma once

// Prim shape parameters: a 2D profile (the cross-section) swept along a path.
// Values and their network quantization follow the official viewer
// (llvolume.h, llvolumemessage.cpp).

#include <glm/glm.hpp>

#include <compare>
#include <cstdint>

namespace viewer::volume
{

// Profile curve: low nibble the outline, high nibble the hollow's shape.
namespace ProfileCurve
{
inline constexpr uint8_t Mask = 0x0F;
inline constexpr uint8_t Circle = 0x00;
inline constexpr uint8_t Square = 0x01;
inline constexpr uint8_t IsoTriangle = 0x02;
inline constexpr uint8_t EqualTriangle = 0x03;
inline constexpr uint8_t RightTriangle = 0x04;
inline constexpr uint8_t HalfCircle = 0x05;
} // namespace ProfileCurve

namespace HoleShape
{
inline constexpr uint8_t Mask = 0xF0;
inline constexpr uint8_t Same = 0x00;
inline constexpr uint8_t Circle = 0x10;
inline constexpr uint8_t Square = 0x20;
inline constexpr uint8_t Triangle = 0x30;
} // namespace HoleShape

namespace PathCurve
{
inline constexpr uint8_t Line = 0x10;
inline constexpr uint8_t Circle = 0x20;
inline constexpr uint8_t Circle2 = 0x30;
inline constexpr uint8_t Test = 0x40;
inline constexpr uint8_t Flexible = 0x80;
} // namespace PathCurve

struct ProfileParams
{
    uint8_t curve = ProfileCurve::Square;
    float begin = 0.0f;
    float end = 1.0f;
    float hollow = 0.0f;

    auto operator<=>(const ProfileParams&) const = default;
};

struct PathParams
{
    uint8_t curve = PathCurve::Line;
    float begin = 0.0f;
    float end = 1.0f;
    glm::vec2 scale{1.0f};
    glm::vec2 shear{0.0f};
    float twistBegin = 0.0f;
    float twist = 0.0f; // twist at the end
    float radiusOffset = 0.0f;
    glm::vec2 taper{0.0f};
    float revolutions = 1.0f;
    float skew = 0.0f;

    auto operator<=>(const PathParams& other) const
    {
        auto key = [](const PathParams& p)
        {
            return std::tuple(p.curve, p.begin, p.end, p.scale.x, p.scale.y, p.shear.x, p.shear.y,
                              p.twistBegin, p.twist, p.radiusOffset, p.taper.x, p.taper.y,
                              p.revolutions, p.skew);
        };
        return key(*this) <=> key(other);
    }
    bool operator==(const PathParams& other) const { return (*this <=> other) == 0; }

    // Path scale as taper at the start and end of the path (line paths).
    glm::vec2 beginScale() const;
    glm::vec2 endScale() const;
};

struct VolumeParams
{
    ProfileParams profile;
    PathParams path;

    auto operator<=>(const VolumeParams&) const = default;
};

// The quantized values as sent in ObjectUpdate and compressed updates.
struct PackedVolumeParams
{
    uint8_t pathCurve = 0;
    uint16_t pathBegin = 0;
    uint16_t pathEnd = 0;
    uint8_t pathScaleX = 0;
    uint8_t pathScaleY = 0;
    int8_t pathShearX = 0;
    int8_t pathShearY = 0;
    int8_t pathTwist = 0;
    int8_t pathTwistBegin = 0;
    int8_t pathRadiusOffset = 0;
    int8_t pathTaperX = 0;
    int8_t pathTaperY = 0;
    uint8_t pathRevolutions = 0;
    int8_t pathSkew = 0;
    uint8_t profileCurve = 0;
    uint16_t profileBegin = 0;
    uint16_t profileEnd = 0;
    uint16_t profileHollow = 0;
};

VolumeParams unpackVolumeParams(const PackedVolumeParams& packed);

} // namespace viewer::volume
