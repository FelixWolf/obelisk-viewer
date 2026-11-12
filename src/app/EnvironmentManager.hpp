#pragma once

// The sky and light: the agent's region's environment (a day cycle from
// its ExtEnvironment capability), followed through the day, turned into
// the scene's lighting and atmosphere. Without one (logged out, or the
// region gives none we can read) it uses a default midday sky.
//
// Parcel environments aren't fetched yet: the region's applies everywhere.

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
    // if none); fetches its environment when either changes. Call every
    // frame.
    void update(std::optional<uint64_t> region, std::string_view capability);

    // The scene's lighting for `unixSeconds` and a camera at `altitude`.
    void apply(render::Lighting& lighting, double unixSeconds, float altitude);

    // For testing and screenshots: a fixed time of day (0..1), or none to
    // follow the clock.
    void setDayPosition(std::optional<float> position) { fixedPosition_ = position; }
    std::optional<float> dayPosition() const { return fixedPosition_; }
    // Exposure for the tonemap. Fixed for now: the official viewer adjusts
    // it to the scene's brightness.
    void setExposure(float exposure) { exposure_ = exposure; }
    float exposure() const { return exposure_; }

    struct Status
    {
        std::string name;       // the day cycle's, or what's in use instead
        double position = 0.0;  // 0..1 through the day, last apply()
        double dayLength = 0.0; // seconds
        glm::vec3 sunDirection{0.0f};
        bool fromRegion = false; // false: the default sky
    };
    const Status& status() const { return status_; }

private:
    struct Reply
    {
        uint64_t region = 0;
        net::HttpResponse response;
    };
    void handle(const Reply& reply);

    net::HttpClient& http_;
    std::unique_ptr<net::HttpRequestGroup> requests_;
    std::shared_ptr<MessageQueue<Reply>> replies_ = std::make_shared<MessageQueue<Reply>>();
    std::vector<Reply> drained_;
    std::optional<uint64_t> region_;
    std::string capability_;
    std::optional<environment::DayCycle> cycle_;
    std::optional<float> fixedPosition_;
    float exposure_ = 0.6f;
    Status status_;
};

} // namespace viewer
