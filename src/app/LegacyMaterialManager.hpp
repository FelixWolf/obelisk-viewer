#pragma once

// Legacy materials (assets::LegacyMaterial) by id, from the region's
// RenderMaterials capability. Asking for one queues it; update() sends
// what's queued, up to 50 to a request, one request a second (as the
// official viewer's LLMaterialMgr and its region throttle; the capability
// answers 503 when asked faster: those go back in the queue). Ids an answer
// leaves out are missing (asked once).

#include "viewer/assets/LegacyMaterial.hpp"
#include "viewer/core/MessageQueue.hpp"
#include "viewer/core/Uuid.hpp"
#include "viewer/net/HttpClient.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace viewer
{

class LegacyMaterialManager
{
public:
    explicit LegacyMaterialManager(net::HttpClient& http);

    LegacyMaterialManager(const LegacyMaterialManager&) = delete;
    LegacyMaterialManager& operator=(const LegacyMaterialManager&) = delete;

    // Empty while logged out (nothing is asked for then).
    void setCapabilityUrl(std::string url);

    // The material once it has arrived; nullptr until then (or if missing).
    std::shared_ptr<const assets::LegacyMaterial> get(const Uuid& id);

    // Developer: where a material is ("ready", "requested", ...; "unknown":
    // never asked for).
    std::string_view stateName(const Uuid& id) const;

    // Handles answers and sends what's queued.
    void update();
    // Bumped whenever a material arrives (or turns out missing).
    uint64_t revision() const { return revision_; }

    struct Stats
    {
        size_t ready = 0;
        size_t loading = 0;
        size_t missing = 0;
    };
    Stats stats() const;

private:
    enum class State : uint8_t
    {
        Queued,
        Requested,
        Ready,
        Missing,
    };
    struct Entry
    {
        State state = State::Queued;
        uint8_t attempts = 0;
        std::shared_ptr<const assets::LegacyMaterial> material;
    };
    struct Answer
    {
        std::vector<Uuid> asked;
        net::HttpResponse response;
    };

    net::HttpClient& http_;
    std::string url_;
    std::unordered_map<Uuid, Entry> entries_;
    uint64_t revision_ = 0;
    std::vector<Uuid> queue_;
    size_t requests_ = 0;
    std::chrono::steady_clock::time_point nextRequest_{};

    MessageQueue<Answer> answers_;
    std::vector<Answer> drained_;
    // Last: destroyed first, so no callback outlives the queue.
    std::unique_ptr<net::HttpRequestGroup> group_;
};

} // namespace viewer
