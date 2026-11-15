#pragma once

// Second Life sky settings ("windlight"/EEP sky frames) and the lighting
// they imply. Values and defaults follow llsettingssky.cpp; the lighting
// model (sunlight dimmed by the atmosphere, haze from blue and haze
// densities) follows LLSettingsSky::calculateLightSettings and the
// official viewer's atmospherics shaders. Colors are in the settings' own
// (roughly sRGB) units, as the official viewer keeps them.
//
// Directions are in region coordinates: X east, Y north, Z up.

#include "viewer/core/Uuid.hpp"
#include "viewer/llsd/Value.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace viewer::environment
{

struct SkySettings
{
    // Atmosphere (from "legacy_haze" when present, as the official viewer).
    glm::vec3 ambient{0.25f};
    glm::vec3 blueDensity{0.2447f, 0.4487f, 0.7599f};
    glm::vec3 blueHorizon{0.4954f, 0.4954f, 0.6399f};
    float hazeDensity = 0.7f;
    float hazeHorizon = 0.19f;
    float densityMultiplier = 0.0001f;
    float distanceMultiplier = 0.8f;
    float maxY = 1605.0f;
    glm::vec3 glow{5.0f, 0.001f, -0.4799f};
    float gamma = 1.0f;
    // Reflection probe ambiance: 0 for skies made before PBR (no
    // "reflection_probe_ambiance", the official default), which the official
    // viewer shows without dynamic exposure or tonemapping.
    float probeAmbiance = 0.0f;

    // Sun and moon: rotations of +X (east) to where they are.
    glm::quat sunRotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::quat moonRotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 sunlightColor{0.7342f, 0.7815f, 0.8999f};
    float sunScale = 1.0f;
    float moonScale = 1.0f;
    float moonBrightness = 0.5f;
    float starBrightness = 250.0f;
    Uuid sunTexture{}; // none by default (llsettingssky.cpp GetDefaultSunTextureId)
    Uuid moonTexture = *Uuid::parse("d07f6eed-b96a-47cd-b51d-400ad4a1c428"); // DEFAULT_MOON_ID

    // Clouds.
    glm::vec3 cloudColor{0.4099f};
    glm::vec3 cloudPosDensity1{1.0f, 0.526f, 1.0f};
    glm::vec3 cloudPosDensity2{1.0f, 0.526f, 1.0f};
    float cloudScale = 0.4199f;
    glm::vec2 cloudScrollRate{0.2f, 0.01f};
    float cloudShadow = 0.2699f;
    float cloudVariance = 0.0f;
    // The default cloud noise (llsettingssky.cpp GetDefaultCloudNoiseTextureId).
    Uuid cloudTexture = *Uuid::parse("1dc1368f-e8fe-f02d-a08d-9d9f11c1af6b");
    // The sky dome the clouds are drawn on: its radius, and how far below
    // the eye its center is (as a fraction of the radius).
    float domeRadius = 15000.0f;
    float domeOffset = 0.96f;

    // Reads a sky frame; anything missing keeps its default.
    static SkySettings fromLlsd(const llsd::Value& sky);
};

// `t` of the way from a to b: values blend linearly, rotations along the
// shorter arc, textures switch half way.
SkySettings blend(const SkySettings& a, const SkySettings& b, float t);

// What a sky means for lighting, worked out once per frame.
struct SkyLighting
{
    glm::vec3 sunDirection{0.0f, 0.0f, 1.0f};  // towards the sun
    glm::vec3 moonDirection{0.0f, 0.0f, 1.0f}; // towards the moon
    bool sunUp = true;
    bool moonUp = false;
    // Towards the light that counts: the sun if up, else the moon if up,
    // else straight down (nothing lit from above).
    glm::vec3 lightDirection{0.0f, 0.0f, 1.0f};
    // The light reaching the ground, dimmed by the air it crossed (as the
    // official atmospherics shader lights surfaces); the moon's is scaled
    // by its brightness.
    glm::vec3 lightColor{1.0f};
    // Ambient light, raised by cloud cover.
    glm::vec3 ambient{0.25f};
    // Glow around the sun: full while it's up, faint for the moon.
    float sunMoonGlow = 1.0f;
};

SkyLighting computeLighting(const SkySettings& sky);

} // namespace viewer::environment
