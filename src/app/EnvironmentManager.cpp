#include "EnvironmentManager.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/llsd/Serialization.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace viewer
{

namespace
{

// The light scales the official viewer uses for its PBR pipeline
// (RenderSkySunlightScale, RenderSkyAmbientScale).
constexpr float kSunlightScale = 1.5f;
constexpr float kAmbientScale = 1.5f;

glm::vec3 srgbToLinear(glm::vec3 c)
{
    auto f = [](float v)
    {
        v = std::max(v, 0.0f);
        return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    };
    return glm::vec3(f(c.r), f(c.g), f(c.b));
}

// Midday, for when there's no environment.
environment::SkySettings defaultSky()
{
    environment::SkySettings sky;
    sky.sunRotation = glm::angleAxis(glm::radians(60.0f), glm::vec3(0.0f, -1.0f, 0.0f)) *
                      glm::angleAxis(glm::radians(-30.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    sky.moonRotation = glm::angleAxis(glm::radians(-30.0f), glm::vec3(0.0f, -1.0f, 0.0f));
    return sky;
}

} // namespace

EnvironmentManager::EnvironmentManager(net::HttpClient& http)
    : http_(http), requests_(std::make_unique<net::HttpRequestGroup>(http))
{
    status_.name = "default sky";
}

void EnvironmentManager::update(std::optional<uint64_t> region, std::string_view capability)
{
    replies_->drain(drained_);
    for(const Reply& r : drained_)
    {
        if(region_ && r.region == *region_)
        {
            handle(r);
        }
    }

    if(region == region_ && capability == capability_)
    {
        return;
    }
    region_ = region;
    capability_ = std::string(capability);
    requests_ = std::make_unique<net::HttpRequestGroup>(http_); // drops the old request
    if(!region || capability.empty())
    {
        if(!region)
        {
            cycle_.reset();
        }
        return; // keep the old region's sky until the new one's arrives
    }
    net::HttpRequest request{.url = capability_};
    request.headers.add("Accept", std::string(llsd::kXmlContentType));
    requests_->send(std::move(request),
                    [replies = replies_, handle = *region](net::HttpResponse&& response)
                    { replies->push({handle, std::move(response)}); });
}

void EnvironmentManager::handle(const Reply& reply)
{
    if(!reply.response.ok())
    {
        log::warn("environment: the region's request failed (HTTP {})", reply.response.status);
        return;
    }
    const auto parsed = llsd::parse(reply.response.bodyText());
    if(!parsed)
    {
        log::warn("environment: unreadable reply: {}", parsed.error());
        return;
    }
    auto cycle = environment::DayCycle::fromEnvironment((*parsed)["environment"]);
    if(!cycle)
    {
        log::warn("environment: {}; using the default sky", cycle.error());
        cycle_.reset();
        return;
    }
    const bool pbr = std::any_of(cycle->skies.begin(), cycle->skies.end(),
                                 [](const auto& sky) { return sky.probeAmbiance > 0.0f; });
    log::info("environment: day cycle '{}', {} skies ({}), {} s per day", cycle->name,
              cycle->skies.size(), pbr ? "made for PBR" : "legacy", cycle->dayLength);
    cycle_ = std::move(*cycle);
}

void EnvironmentManager::apply(render::Lighting& lighting, double unixSeconds, float altitude)
{
    environment::SkySettings sky;
    if(cycle_)
    {
        status_.position = fixedPosition_ ? *fixedPosition_ : cycle_->position(unixSeconds);
        status_.dayLength = cycle_->dayLength;
        status_.name = cycle_->name;
        status_.fromRegion = true;
        sky = cycle_->skyAt(status_.position, altitude);
    }
    else
    {
        status_.position = fixedPosition_ ? *fixedPosition_ : 0.5;
        status_.dayLength = 0.0;
        status_.name = "default sky";
        status_.fromRegion = false;
        sky = defaultSky();
        if(fixedPosition_)
        {
            // Round the sky once a day: rising in the east at 0.25.
            const float angle = (*fixedPosition_ - 0.25f) * glm::two_pi<float>();
            sky.sunRotation = glm::angleAxis(angle, glm::vec3(0.0f, -1.0f, 0.0f));
            sky.moonRotation =
                glm::angleAxis(angle + glm::pi<float>(), glm::vec3(0.0f, -1.0f, 0.0f));
        }
    }
    const double elapsed = lastApply_ > 0.0 ? std::clamp(unixSeconds - lastApply_, 0.0, 1.0) : 0.0;
    lastApply_ = unixSeconds;
    if(sky.cloudScrollRate == glm::vec2(0.0f))
    {
        cloudScroll_ = glm::vec2(0.0f);
    }
    else
    {
        cloudScroll_ += static_cast<float>(elapsed) * sky.cloudScrollRate / 100.0f;
        cloudScroll_ = glm::mod(cloudScroll_, glm::vec2(1.0f)); // the noise tiles
    }
    cloudTexture_ = sky.cloudTexture;

    const environment::SkyLighting light = environment::computeLighting(sky);
    status_.sunDirection = light.sunDirection;

    // Surfaces: linear light (as the official viewer's PBR pipeline: the
    // settings' sRGB-ish colors linearized and scaled; ambient as gray).
    lighting.sunDirection =
        light.lightDirection.z > 0.0f ? light.lightDirection : glm::vec3(0.0f, 0.0f, 1.0f);
    // Our shading divides the light by pi (energy-conserving BRDFs); the
    // official viewer's sunlight is used as is, so it's pi times brighter here.
    lighting.sunColor = light.lightDirection.z > 0.0f
                            ? srgbToLinear(light.lightColor) * kSunlightScale * glm::pi<float>()
                            : glm::vec3(0.0f);
    const glm::vec3 ambient = srgbToLinear(glm::pow(light.ambient, glm::vec3(0.9f)) * 0.57f);
    const float gray = glm::dot(ambient, glm::vec3(0.2126f, 0.7152f, 0.0722f)) * kAmbientScale;
    lighting.skyAmbient = glm::vec3(gray);
    lighting.groundAmbient = glm::vec3(gray * 0.6f);

    lighting.exposure = exposure_;
    // Skies made for PBR (with a probe ambiance) get the official viewer's
    // dynamic exposure (pipeline.cpp generateExposure): from 2 sqrt(gamma)
    // in dark views down to its inverse in bright ones. Older skies get
    // neither that nor tonemapping there (RenderSkyAutoAdjustLegacy off).
    lighting.exposureMin = 1.0f;
    lighting.exposureMax = 1.0f;
    lighting.tonemapMix = 0.0f;
    if(sky.probeAmbiance > 0.0f)
    {
        lighting.tonemapMix = 0.7f;
        const float scale = std::sqrt(sky.gamma) * 2.0f;
        if(scale > 1.0f)
        {
            lighting.exposureMin = 1.0f / scale;
            lighting.exposureMax = scale;
        }
    }

    render::Atmosphere& a = lighting.atmosphere;
    a.enabled = true;
    a.lightDirection = light.lightDirection;
    a.sunlight = sky.sunlightColor;
    a.ambient = sky.ambient;
    a.blueHorizon = sky.blueHorizon;
    a.blueDensity = sky.blueDensity;
    a.hazeHorizon = sky.hazeHorizon;
    a.hazeDensity = sky.hazeDensity;
    a.densityMultiplier = sky.densityMultiplier;
    a.distanceMultiplier = sky.distanceMultiplier;
    a.maxY = sky.maxY;
    a.glow = sky.glow;
    a.cloudShadow = sky.cloudShadow;
    a.sunMoonGlow = light.sunMoonGlow;
    a.sunUp = light.sunUp;
    a.sunDirection = light.sunDirection;
    a.moonDirection = light.moonDirection;
    a.sunRadius = 0.012f * sky.sunScale;
    a.moonRadius = 0.02f * sky.moonScale;
    a.moonBrightness = sky.moonBrightness;
    a.starBrightness = sky.starBrightness;
    a.cloudColor = sky.cloudColor;
    a.cloudScale = sky.cloudScale;
    // Scrolling east moves the texture the other way (llsettingsvo.cpp).
    a.cloudPosDensity1 = sky.cloudPosDensity1 + glm::vec3(-cloudScroll_.x, cloudScroll_.y, 0.0f);
    a.cloudPosDensity2 = sky.cloudPosDensity2;
    a.cloudVariance = sky.cloudVariance;
    a.domeRadius = sky.domeRadius;
    a.domeOffset = sky.domeOffset;
}

} // namespace viewer
