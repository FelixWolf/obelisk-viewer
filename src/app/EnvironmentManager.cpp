#include "EnvironmentManager.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/llsd/Serialization.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace viewer
{

namespace
{

// The light scales the official viewer uses for its PBR pipeline: its
// settings.xml's RenderSkySunlightScale (RenderHDRSkySunlightScale with HDR)
// and RenderSkyAmbientScale, 1 (the 1.5 in llsettingsvo.cpp is only the
// fallback for a missing setting).
constexpr float kSunlightScale = 1.0f;
constexpr float kAmbientScale = 1.0f;
constexpr float kClassicSunlight = 1.35f;

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

void EnvironmentManager::update(std::optional<uint64_t> region, std::string_view capability,
                                std::optional<ParcelKey> parcel)
{
    // The sky asset asked for, once.
    if(skyAsset_ && !skyRequested_ && !viewerAssetUrl_.empty())
    {
        skyRequested_ = true;
        skyRequests_ = std::make_unique<net::HttpRequestGroup>(http_);
        net::HttpRequest request{.url = viewerAssetUrl_ + "/?settings_id=" + skyAsset_->toString()};
        skyRequests_->send(std::move(request), [replies = skyReplies_](net::HttpResponse&& response)
                           { replies->push(std::move(response)); });
    }
    skyReplies_->drain(skyDrained_);
    for(const net::HttpResponse& r : skyDrained_)
    {
        const auto parsed = r.ok() ? llsd::parse(r.bodyText())
                                   : std::expected<llsd::Value, std::string>(
                                         std::unexpected(std::format("HTTP {}", r.status)));
        if(!parsed || (*parsed)["type"].asString() != "sky")
        {
            log::warn("environment: the sky asset {} couldn't be had ({})", skyAsset_->toString(),
                      parsed ? "not a sky" : parsed.error());
            continue;
        }
        fixedSky_ = environment::SkySettings::fromLlsd(*parsed);
        const environment::SkySettings& k = *fixedSky_;
        log::info("environment: using the sky '{}' ({}): ambient ({:.3f} {:.3f} {:.3f}), "
                  "sunlight ({:.3f} {:.3f} {:.3f}), blue density ({:.3f} {:.3f} {:.3f}), "
                  "blue horizon ({:.3f} {:.3f} {:.3f}), haze {:.3f}/{:.3f}, gamma {:.2f}",
                  (*parsed)["name"].asString(), k.classic ? "legacy" : "made for PBR",
                  k.ambient.r, k.ambient.g, k.ambient.b, k.sunlightColor.r, k.sunlightColor.g,
                  k.sunlightColor.b, k.blueDensity.r, k.blueDensity.g, k.blueDensity.b,
                  k.blueHorizon.r, k.blueHorizon.g, k.blueHorizon.b, k.hazeDensity,
                  k.hazeHorizon, k.gamma);
    }

    replies_->drain(drained_);
    for(const Reply& r : drained_)
    {
        if(region_ && r.region == *region_ &&
           (!r.parcel || (parcel_ && parcel_->localId == *r.parcel)))
        {
            handle(r);
        }
    }

    if(region != region_ || capability != capability_)
    {
        region_ = region;
        capability_ = std::string(capability);
        parcel_.reset();
        parcelCycle_.reset();
        requests_ = std::make_unique<net::HttpRequestGroup>(http_); // drops the old requests
        if(!region || capability.empty())
        {
            if(!region)
            {
                cycle_.reset();
            }
            return; // keep the old region's sky until the new one's arrives
        }
        request(std::nullopt);
    }
    // The parcel's, when the agent changes parcels or its environment does.
    if(region_ && !capability_.empty() && parcel && parcel != parcel_)
    {
        if(!parcel_ || parcel_->localId != parcel->localId)
        {
            parcelCycle_.reset(); // another parcel: the region's until it says
        }
        parcel_ = parcel;
        request(parcel->localId);
    }
}

void EnvironmentManager::request(std::optional<int> parcel)
{
    std::string url = capability_;
    if(parcel)
    {
        url += "?parcelid=" + std::to_string(*parcel);
    }
    net::HttpRequest request{.url = std::move(url)};
    request.headers.add("Accept", std::string(llsd::kXmlContentType));
    requests_->send(std::move(request),
                    [replies = replies_, handle = *region_, parcel](net::HttpResponse&& response)
                    { replies->push({handle, parcel, std::move(response)}); });
}

void EnvironmentManager::handle(const Reply& reply)
{
    const char* what = reply.parcel ? "the parcel's" : "the region's";
    if(!reply.response.ok())
    {
        log::warn("environment: {} request failed (HTTP {})", what, reply.response.status);
        return;
    }
    const auto parsed = llsd::parse(reply.response.bodyText());
    if(!parsed)
    {
        log::warn("environment: unreadable reply: {}", parsed.error());
        return;
    }
    const llsd::Value& environment = (*parsed)["environment"];
    if(reply.parcel)
    {
        // The parcel's own, if it has one (parcel_id -1, or no day: it uses
        // the region's).
        auto cycle =
            environment["parcel_id"].asInteger() == -1
                ? std::expected<environment::DayCycle, std::string>(std::unexpected("the region's"))
                : environment::DayCycle::fromEnvironment(environment);
        if(!cycle)
        {
            if(parcelCycle_)
            {
                log::info("environment: the parcel uses the region's");
            }
            parcelCycle_.reset();
            return;
        }
        log::info("environment: the parcel's own day cycle '{}', {} s per day, offset {} s",
                  cycle->name, cycle->dayLength, cycle->dayOffset);
        parcelCycle_ = std::move(*cycle);
        return;
    }
    auto cycle = environment::DayCycle::fromEnvironment(environment);
    if(!cycle)
    {
        log::warn("environment: {}; using the default sky", cycle.error());
        cycle_.reset();
        return;
    }
    const bool pbr = std::any_of(cycle->skies.begin(), cycle->skies.end(),
                                 [](const auto& sky) { return !sky.classic; });
    log::info("environment: day cycle '{}', {} skies ({}), {} s per day, offset {} s", cycle->name,
              cycle->skies.size(), pbr ? "made for PBR" : "legacy", cycle->dayLength,
              cycle->dayOffset);
    cycle_ = std::move(*cycle);
}

void EnvironmentManager::apply(render::Lighting& lighting, double unixSeconds, float altitude)
{
    environment::SkySettings sky;
    // The parcel's own day (with the region's sky altitudes), else the
    // region's.
    if(parcelCycle_ && cycle_)
    {
        parcelCycle_->trackAltitudes = cycle_->trackAltitudes;
    }
    const environment::DayCycle* cycle = parcelCycle_ ? &*parcelCycle_
                                         : cycle_     ? &*cycle_
                                                      : nullptr;
    status_.fromParcel = parcelCycle_.has_value();
    if(cycle)
    {
        status_.position = fixedPosition_ ? *fixedPosition_ : cycle->position(unixSeconds);
        status_.dayLength = cycle->dayLength;
        status_.name = cycle->name;
        status_.fromRegion = true;
        sky = cycle->skyAt(status_.position, altitude);
        water_ = cycle->waterAt(status_.position);
        if(fixedSky_)
        {
            sky = *fixedSky_;
            status_.name = "sky asset";
        }
    }
    else
    {
        status_.position = fixedPosition_ ? *fixedPosition_ : 0.5;
        status_.dayLength = 0.0;
        status_.name = "default sky";
        status_.fromRegion = false;
        sky = defaultSky();
        water_ = environment::WaterSettings{};
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
    sunTexture_ = sky.sunTexture;
    moonTexture_ = sky.moonTexture;

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
    // One gray all round, as the official viewer's (no sky and ground
    // colors); the shaders dim it facing towards or away from the light
    // (ambientLighting).
    lighting.skyAmbient = glm::vec3(gray);
    lighting.groundAmbient = glm::vec3(gray);
    // Skies made before PBR: the official viewer's "classic mode"
    // (RenderSkyAutoAdjustLegacy off, its default). The sun, 1.35 times
    // brighter, and the ambient in its color stay sRGB (softenLightF.glsl,
    // atmosphericsFuncs.glsl calcAtmosphericVarsLinear).
    lighting.classic = sky.classic;
    if(sky.classic)
    {
        lighting.sunColor = light.lightDirection.z > 0.0f
                                ? light.lightColor * kSunlightScale * kClassicSunlight
                                : glm::vec3(0.0f);
        const glm::vec3 amblit = glm::pow(light.ambient, glm::vec3(0.9f)) * 0.57f * kAmbientScale;
        lighting.skyAmbient = amblit;
        lighting.groundAmbient = amblit;
    }

    lighting.exposure = exposure_;
    // Skies made for PBR (with a probe ambiance) get the official viewer's
    // dynamic exposure (pipeline.cpp generateExposure): from 2 sqrt(gamma)
    // in dark views down to its inverse in bright ones. Older skies get
    // neither that nor tonemapping there (RenderSkyAutoAdjustLegacy off).
    lighting.exposureMin = 1.0f;
    lighting.exposureMax = 1.0f;
    lighting.tonemapMix = 0.0f;
    lighting.probeAmbiance = sky.probeAmbiance;
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
    a.sunScale = sky.sunScale;
    a.moonScale = sky.moonScale;
    a.moonBrightness = sky.moonBrightness;
    // llsettingsvo.cpp applyToShader: sqrt(gamma) x 2 with a probe ambiance.
    a.skyHdrScale = sky.probeAmbiance > 0.0f ? std::sqrt(sky.gamma) * 2.0f : 1.0f;
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
