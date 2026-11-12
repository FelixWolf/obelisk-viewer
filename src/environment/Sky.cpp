#include "viewer/environment/Sky.hpp"

#include <algorithm>
#include <cmath>

namespace viewer::environment
{

namespace
{

const llsd::Value* lookup(const llsd::Value& sky, std::string_view key)
{
    // Legacy haze values take precedence (llsettingssky.cpp getColor/getFloat).
    const llsd::Value& legacy = sky["legacy_haze"];
    if(legacy.isMap() && legacy.has(key))
    {
        return &legacy[key];
    }
    return sky.has(key) ? &sky[key] : nullptr;
}

void read(const llsd::Value& sky, std::string_view key, float& out)
{
    if(const llsd::Value* v = lookup(sky, key))
    {
        out = static_cast<float>(v->asReal());
    }
}

void read(const llsd::Value& sky, std::string_view key, glm::vec3& out)
{
    if(const llsd::Value* v = lookup(sky, key); v && v->isArray() && v->size() >= 3)
    {
        out = glm::vec3(static_cast<float>((*v)[0].asReal()), static_cast<float>((*v)[1].asReal()),
                        static_cast<float>((*v)[2].asReal()));
    }
}

void read(const llsd::Value& sky, std::string_view key, glm::vec2& out)
{
    if(const llsd::Value* v = lookup(sky, key); v && v->isArray() && v->size() >= 2)
    {
        out = glm::vec2(static_cast<float>((*v)[0].asReal()), static_cast<float>((*v)[1].asReal()));
    }
}

void read(const llsd::Value& sky, std::string_view key, glm::quat& out)
{
    // Stored x, y, z, w.
    if(const llsd::Value* v = lookup(sky, key); v && v->isArray() && v->size() >= 4)
    {
        const glm::quat q(
            static_cast<float>((*v)[3].asReal()), static_cast<float>((*v)[0].asReal()),
            static_cast<float>((*v)[1].asReal()), static_cast<float>((*v)[2].asReal()));
        if(glm::length(q) > 1e-4f)
        {
            out = glm::normalize(q);
        }
    }
}

void read(const llsd::Value& sky, std::string_view key, Uuid& out)
{
    if(const llsd::Value* v = lookup(sky, key))
    {
        out = v->asUuid();
    }
}

glm::vec3 exp3(const glm::vec3& v)
{
    return glm::vec3(std::exp(v.x), std::exp(v.y), std::exp(v.z));
}

} // namespace

SkySettings SkySettings::fromLlsd(const llsd::Value& sky)
{
    SkySettings s;
    read(sky, "ambient", s.ambient);
    read(sky, "blue_density", s.blueDensity);
    read(sky, "blue_horizon", s.blueHorizon);
    read(sky, "haze_density", s.hazeDensity);
    read(sky, "haze_horizon", s.hazeHorizon);
    read(sky, "density_multiplier", s.densityMultiplier);
    read(sky, "distance_multiplier", s.distanceMultiplier);
    read(sky, "max_y", s.maxY);
    read(sky, "glow", s.glow);
    read(sky, "gamma", s.gamma);
    read(sky, "sun_rotation", s.sunRotation);
    read(sky, "moon_rotation", s.moonRotation);
    read(sky, "sunlight_color", s.sunlightColor);
    read(sky, "sun_scale", s.sunScale);
    read(sky, "moon_scale", s.moonScale);
    read(sky, "moon_brightness", s.moonBrightness);
    read(sky, "star_brightness", s.starBrightness);
    read(sky, "sun_id", s.sunTexture);
    read(sky, "moon_id", s.moonTexture);
    read(sky, "cloud_color", s.cloudColor);
    read(sky, "cloud_pos_density1", s.cloudPosDensity1);
    read(sky, "cloud_pos_density2", s.cloudPosDensity2);
    read(sky, "cloud_scale", s.cloudScale);
    read(sky, "cloud_scroll_rate", s.cloudScrollRate);
    read(sky, "cloud_shadow", s.cloudShadow);
    read(sky, "cloud_variance", s.cloudVariance);
    read(sky, "cloud_id", s.cloudTexture);
    read(sky, "dome_radius", s.domeRadius);
    read(sky, "dome_offset", s.domeOffset);
    return s;
}

SkySettings blend(const SkySettings& a, const SkySettings& b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    auto mix = [t](const auto& x, const auto& y) { return x + (y - x) * t; };
    SkySettings s = t < 0.5f ? a : b; // textures and anything not blended
    s.ambient = mix(a.ambient, b.ambient);
    s.blueDensity = mix(a.blueDensity, b.blueDensity);
    s.blueHorizon = mix(a.blueHorizon, b.blueHorizon);
    s.hazeDensity = mix(a.hazeDensity, b.hazeDensity);
    s.hazeHorizon = mix(a.hazeHorizon, b.hazeHorizon);
    s.densityMultiplier = mix(a.densityMultiplier, b.densityMultiplier);
    s.distanceMultiplier = mix(a.distanceMultiplier, b.distanceMultiplier);
    s.maxY = mix(a.maxY, b.maxY);
    s.glow = mix(a.glow, b.glow);
    s.gamma = mix(a.gamma, b.gamma);
    s.sunRotation = glm::slerp(a.sunRotation, b.sunRotation, t);
    s.moonRotation = glm::slerp(a.moonRotation, b.moonRotation, t);
    s.sunlightColor = mix(a.sunlightColor, b.sunlightColor);
    s.sunScale = mix(a.sunScale, b.sunScale);
    s.moonScale = mix(a.moonScale, b.moonScale);
    s.moonBrightness = mix(a.moonBrightness, b.moonBrightness);
    s.starBrightness = mix(a.starBrightness, b.starBrightness);
    s.cloudColor = mix(a.cloudColor, b.cloudColor);
    s.cloudPosDensity1 = mix(a.cloudPosDensity1, b.cloudPosDensity1);
    s.cloudPosDensity2 = mix(a.cloudPosDensity2, b.cloudPosDensity2);
    s.cloudScale = mix(a.cloudScale, b.cloudScale);
    s.cloudScrollRate = mix(a.cloudScrollRate, b.cloudScrollRate);
    s.cloudShadow = mix(a.cloudShadow, b.cloudShadow);
    s.cloudVariance = mix(a.cloudVariance, b.cloudVariance);
    s.domeRadius = mix(a.domeRadius, b.domeRadius);
    s.domeOffset = mix(a.domeOffset, b.domeOffset);
    return s;
}

SkyLighting computeLighting(const SkySettings& sky)
{
    SkyLighting l;
    l.sunDirection = glm::normalize(sky.sunRotation * glm::vec3(1.0f, 0.0f, 0.0f));
    l.moonDirection = glm::normalize(sky.moonRotation * glm::vec3(1.0f, 0.0f, 0.0f));
    l.sunUp = l.sunDirection.z >= 0.0f;
    l.moonUp = l.moonDirection.z >= 0.0f;
    l.lightDirection = l.sunUp    ? l.sunDirection
                       : l.moonUp ? l.moonDirection
                                  : glm::vec3(0.0f, 0.0f, -1.0f);

    // Light crossing the atmosphere: the lower the light, the longer its
    // path (about 1 / sin(elevation)) and the more it's dimmed and reddened.
    const glm::vec3 attenuation =
        (sky.blueDensity + glm::vec3(sky.hazeDensity * 0.25f)) * sky.densityMultiplier * sky.maxY;
    const float pathLength = 1.0f / std::max(std::abs(l.lightDirection.z), 1e-3f);
    const glm::vec3 dimmed = sky.sunlightColor * exp3(-attenuation * pathLength);
    if(l.sunUp)
    {
        l.lightColor = dimmed;
    }
    else if(l.moonUp)
    {
        l.lightColor = dimmed * sky.moonBrightness;
    }
    else
    {
        l.lightColor = glm::vec3(0.0f);
    }

    l.ambient = sky.ambient +
                glm::max(glm::vec3(0.0f), glm::vec3(1.0f) - sky.ambient) * sky.cloudShadow * 0.5f;
    l.sunMoonGlow = l.sunUp ? 1.0f : l.moonUp ? sky.moonBrightness * 0.25f : 0.0f;
    return l;
}

} // namespace viewer::environment
