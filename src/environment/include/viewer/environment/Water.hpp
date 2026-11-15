#pragma once

// A day cycle's water settings (llsettingswater.cpp LLSettingsWater): how
// the water's waves move, how it reflects and refracts, and its fog.

#include "viewer/core/Uuid.hpp"
#include "viewer/llsd/Value.hpp"

#include <glm/glm.hpp>

namespace viewer::environment
{

struct WaterSettings
{
    // The official defaults (LLSettingsWater::defaults).
    glm::vec3 fogColor{0.0156f, 0.1490f, 0.2509f}; // roughly sRGB
    float fogDensity = 2.0f;
    float underwaterFogMod = 0.25f;
    float fresnelOffset = 0.5f;
    float fresnelScale = 0.3999f;
    float blurMultiplier = 0.04f;
    float scaleAbove = 0.0299f; // how far the waves bend what's seen through
    float scaleBelow = 0.2f;    // from above and from below
    glm::vec3 normalScale{2.0f};
    glm::vec2 wave1{1.04999f, -0.42f};
    glm::vec2 wave2{1.10999f, -1.16f};
    Uuid normalMap = *Uuid::parse("822ded49-9a6c-f61c-cb89-6df54f42cdf4"); // DEFAULT_WATER_NORMAL
    // Between two keyframes: the next one's normal map, blended in by this.
    Uuid nextNormalMap = normalMap;
    float normalBlend = 0.0f;

    // A "water" frame of a day cycle.
    static WaterSettings fromLlsd(const llsd::Value& frame);
    // The fog's density, thicker under the water by the fog mod
    // (getModifiedWaterFogDensity).
    float fogDensityFor(bool underwater) const;
};

// Between two keyframes, `t` of the way (the normal maps cross-fade).
WaterSettings blend(const WaterSettings& a, const WaterSettings& b, float t);

} // namespace viewer::environment
