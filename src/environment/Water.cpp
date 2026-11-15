#include "viewer/environment/Water.hpp"

#include <algorithm>
#include <cmath>

namespace viewer::environment
{

namespace
{

void read(const llsd::Value& frame, std::string_view key, float& out)
{
    if(frame.has(key))
    {
        out = static_cast<float>(frame[key].asReal());
    }
}

template <int N>
void read(const llsd::Value& frame, std::string_view key, glm::vec<N, float>& out)
{
    const llsd::Value& v = frame[key];
    if(v.isArray() && v.size() >= N)
    {
        for(int i = 0; i < N; i++)
        {
            out[i] = static_cast<float>(v[static_cast<size_t>(i)].asReal());
        }
    }
}

} // namespace

WaterSettings WaterSettings::fromLlsd(const llsd::Value& frame)
{
    WaterSettings w;
    read(frame, "water_fog_color", w.fogColor);
    read(frame, "water_fog_density", w.fogDensity);
    read(frame, "underwater_fog_mod", w.underwaterFogMod);
    read(frame, "fresnel_offset", w.fresnelOffset);
    read(frame, "fresnel_scale", w.fresnelScale);
    read(frame, "blur_multiplier", w.blurMultiplier);
    read(frame, "scale_above", w.scaleAbove);
    read(frame, "scale_below", w.scaleBelow);
    read(frame, "normal_scale", w.normalScale);
    read(frame, "wave1_direction", w.wave1);
    read(frame, "wave2_direction", w.wave2);
    if(frame.has("normal_map") && !frame["normal_map"].asUuid().isNull())
    {
        w.normalMap = frame["normal_map"].asUuid();
    }
    w.nextNormalMap = w.normalMap;
    return w;
}

float WaterSettings::fogDensityFor(bool underwater) const
{
    float density = fogDensity;
    if(underwater && underwaterFogMod > 0.0f)
    {
        const float mod = std::clamp(underwaterFogMod, 0.0f, 10.0f);
        // A negative density to a fractional power isn't a number: 1
        // instead (as the official viewer, BUG-233797).
        if(density < 0.0f && mod != std::round(mod))
        {
            density = 1.0f;
        }
        density = std::pow(density, mod);
    }
    return density;
}

WaterSettings blend(const WaterSettings& a, const WaterSettings& b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    auto mix = [t](const auto& x, const auto& y) { return x + (y - x) * t; };
    WaterSettings w = a;
    w.fogColor = mix(a.fogColor, b.fogColor);
    w.fogDensity = mix(a.fogDensity, b.fogDensity);
    w.underwaterFogMod = mix(a.underwaterFogMod, b.underwaterFogMod);
    w.fresnelOffset = mix(a.fresnelOffset, b.fresnelOffset);
    w.fresnelScale = mix(a.fresnelScale, b.fresnelScale);
    w.blurMultiplier = mix(a.blurMultiplier, b.blurMultiplier);
    w.scaleAbove = mix(a.scaleAbove, b.scaleAbove);
    w.scaleBelow = mix(a.scaleBelow, b.scaleBelow);
    w.normalScale = mix(a.normalScale, b.normalScale);
    w.wave1 = mix(a.wave1, b.wave1);
    w.wave2 = mix(a.wave2, b.wave2);
    w.normalMap = a.normalMap;
    w.nextNormalMap = b.normalMap;
    w.normalBlend = a.normalMap == b.normalMap ? 0.0f : t;
    return w;
}

} // namespace viewer::environment
