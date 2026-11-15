#pragma once

// A prim's particle system (llParticleSystem), as objects carry it: the
// PSBlock of a full update, or the legacy 86 bytes of a compressed one
// (llpartdata.cpp LLPartSysData). The source's settings (how it emits:
// pattern, burst rate, count, speed, angles, its life) and its particles'
// (their life, color, scale and glow from start to end, how they blend).

#include "viewer/core/Uuid.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <span>

namespace viewer::world
{

struct ParticleSystem
{
    // Source flags (LLPartSysData).
    static constexpr uint32_t kObjectRelative = 0x01; // velocity and acceleration turn with it
    static constexpr uint32_t kNewAngles = 0x02;      // the 'correct' angle parameters
    // Patterns.
    static constexpr uint8_t kDrop = 0x01;
    static constexpr uint8_t kExplode = 0x02;
    static constexpr uint8_t kAngle = 0x04;
    static constexpr uint8_t kAngleCone = 0x08;
    static constexpr uint8_t kAngleConeEmpty = 0x10;
    // Particle flags (LLPartData).
    static constexpr uint32_t kInterpolateColor = 0x01;
    static constexpr uint32_t kInterpolateScale = 0x02;
    static constexpr uint32_t kBounce = 0x04;
    static constexpr uint32_t kWind = 0x08;
    static constexpr uint32_t kFollowSource = 0x10;
    static constexpr uint32_t kFollowVelocity = 0x20;
    static constexpr uint32_t kTarget = 0x40;
    static constexpr uint32_t kTargetLinear = 0x80;
    static constexpr uint32_t kEmissive = 0x100;
    static constexpr uint32_t kBeam = 0x200;
    static constexpr uint32_t kRibbon = 0x400;
    static constexpr uint32_t kHasGlow = 0x10000;
    static constexpr uint32_t kHasBlend = 0x20000;
    // Blend factors (LLPartData::LL_PART_BF_*, the same order as
    // LLRender::eBlendFactor).
    enum Blend : uint8_t
    {
        One = 0,
        Zero = 1,
        DestColor = 2,
        SourceColor = 3,
        OneMinusDestColor = 4,
        OneMinusSourceColor = 5,
        SourceAlpha = 7,
        OneMinusSourceAlpha = 9,
    };

    // The source.
    uint32_t crc = 0;
    uint32_t flags = 0;
    uint8_t pattern = kDrop;
    float maxAge = 0.0f;   // seconds the source lives (0: for ever)
    float startAge = 0.0f; // how old it already is
    float innerAngle = 0.0f;
    float outerAngle = 0.0f;
    float burstRate = 0.1f; // seconds between bursts
    float burstRadius = 0.0f;
    float burstSpeedMin = 1.0f;
    float burstSpeedMax = 1.0f;
    uint8_t burstCount = 1;
    glm::vec3 angularVelocity{0.0f}; // of the emission direction
    glm::vec3 acceleration{0.0f};    // the particles'
    Uuid texture{};
    Uuid target{};
    // Its particles.
    uint32_t particleFlags = 0;
    float particleMaxAge = 10.0f;
    glm::vec4 startColor{1.0f};
    glm::vec4 endColor{1.0f};
    glm::vec2 startScale{1.0f};
    glm::vec2 endScale{1.0f};
    float startGlow = 0.0f;
    float endGlow = 0.0f;
    uint8_t blendSource = SourceAlpha;
    uint8_t blendDest = OneMinusSourceAlpha;

    bool operator==(const ParticleSystem&) const = default;
};

// A PSBlock (68 bytes of source and 18 of particle as the legacy format;
// newer ones size-prefixed, with optional glow and blend). Nothing if there
// is no system (empty, or a checksum of 0) or it's a version this doesn't
// know.
std::optional<ParticleSystem> decodeParticleSystem(std::span<const uint8_t> data);

} // namespace viewer::world
