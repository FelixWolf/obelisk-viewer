#pragma once

// The sky and light: the agent's region's environment (a day cycle from
// its ExtEnvironment capability), or its parcel's if the parcel has its
// own (asked for with ?parcelid=, again when the agent changes parcels or
// the parcel's environment version changes, as llenvironment.cpp), followed
// through the day, turned into the scene's lighting and atmosphere. A
// parcel's day uses the region's sky altitudes. Without one (logged out,
// or the region gives none we can read) it uses a default midday sky.

#include "viewer/core/MessageQueue.hpp"
#include "viewer/environment/DayCycle.hpp"
#include "viewer/net/HttpClient.hpp"
#include "viewer/render/Scene.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace viewer
{

class EnvironmentManager
{
public:
    explicit EnvironmentManager(net::HttpClient& http);

    // The region the agent is in and its ExtEnvironment capability (empty
    // if none), and the parcel it's on (its local id and environment
    // version), if known; fetches their environments when they change.
    // Call every frame.
    struct ParcelKey
    {
        int localId = 0;
        int environmentVersion = -1;
        bool operator==(const ParcelKey&) const = default;
    };
    void update(std::optional<uint64_t> region, std::string_view capability,
                std::optional<ParcelKey> parcel = std::nullopt);

    // The scene's lighting for `unixSeconds`, the agent at `altitude` (which
    // sky track: the official viewer goes by the agent's, not the camera's).
    void apply(render::Lighting& lighting, double unixSeconds, float altitude);

    // For testing and screenshots: a fixed time of day (0..1), or none to
    // follow the clock.
    void setDayPosition(std::optional<float> position) { fixedPosition_ = position; }
    std::optional<float> dayPosition() const { return fixedPosition_; }
    // For comparisons: a sky settings asset used instead of the region's
    // and parcel's skies (as the official viewer's personal lighting), its
    // sun where the sky puts it; fetched from the ViewerAsset capability
    // (?settings_id=) once it's known. The water stays the region's.
    void setSkyAsset(std::optional<Uuid> asset) { skyAsset_ = asset; }
    void setViewerAssetUrl(std::string url) { viewerAssetUrl_ = std::move(url); }
    // Exposure for the tonemap (the official viewer's RenderExposure, 1),
    // before the dynamic exposure.
    void setExposure(float exposure) { exposure_ = exposure; }
    float exposure() const { return exposure_; }

    struct Status
    {
        std::string name;       // the day cycle's, or what's in use instead
        double position = 0.0;  // 0..1 through the day, last apply()
        double dayLength = 0.0; // seconds
        glm::vec3 sunDirection{0.0f};
        bool fromRegion = false; // false: the default sky
        bool fromParcel = false; // the parcel's own
    };
    const Status& status() const { return status_; }
    // The cloud noise texture the current sky uses (after apply()).
    const Uuid& cloudTexture() const { return cloudTexture_; }
    // The sun's and moon's textures (null: none).
    const Uuid& sunTexture() const { return sunTexture_; }
    const Uuid& moonTexture() const { return moonTexture_; }
    // The water at the time of the last apply().
    const environment::WaterSettings& water() const { return water_; }

private:
    struct Reply
    {
        uint64_t region = 0;
        std::optional<int> parcel; // a parcel's (else the region's)
        net::HttpResponse response;
    };
    void request(std::optional<int> parcel);
    void handle(const Reply& reply);

    net::HttpClient& http_;
    std::unique_ptr<net::HttpRequestGroup> requests_;
    std::shared_ptr<MessageQueue<Reply>> replies_ = std::make_shared<MessageQueue<Reply>>();
    std::vector<Reply> drained_;
    std::optional<uint64_t> region_;
    std::string capability_;
    std::optional<environment::DayCycle> cycle_;       // the region's
    std::optional<ParcelKey> parcel_;                  // asked for
    std::optional<environment::DayCycle> parcelCycle_; // the parcel's own
    std::optional<float> fixedPosition_;
    std::optional<Uuid> skyAsset_;
    std::string viewerAssetUrl_;
    bool skyRequested_ = false;
    std::optional<environment::SkySettings> fixedSky_;
    std::unique_ptr<net::HttpRequestGroup> skyRequests_;
    std::shared_ptr<MessageQueue<net::HttpResponse>> skyReplies_ =
        std::make_shared<MessageQueue<net::HttpResponse>>();
    std::vector<net::HttpResponse> skyDrained_;
    float exposure_ = 1.0f;
    // Clouds drift by the sky's scroll rate; accumulated here so a change of
    // sky doesn't make them jump (as llenvironment.cpp updateCloudScroll).
    glm::vec2 cloudScroll_{0.0f};
    double lastApply_ = 0.0;
    Uuid cloudTexture_{};
    Uuid sunTexture_{};
    Uuid moonTexture_{};
    environment::WaterSettings water_;
    Status status_;
};

} // namespace viewer
