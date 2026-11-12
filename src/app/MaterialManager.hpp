#pragma once

// PBR material assets by id (assets::Material). Asking for one starts
// loading it: from the disk cache (the asset as downloaded), else from the
// region's ViewerAsset capability (<cap>/?material_id=<uuid>, as the
// official viewer). Reading, parsing and writing run on worker threads.

#include "viewer/assets/Material.hpp"
#include "viewer/core/DiskCache.hpp"
#include "viewer/core/JobSystem.hpp"
#include "viewer/core/MessageQueue.hpp"
#include "viewer/core/Uuid.hpp"
#include "viewer/net/HttpClient.hpp"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace viewer
{

class MaterialManager
{
public:
    MaterialManager(net::HttpClient& http, JobSystem& jobs, DiskCache& store);
    // Cancels downloads and waits for jobs.
    ~MaterialManager();

    MaterialManager(const MaterialManager&) = delete;
    MaterialManager& operator=(const MaterialManager&) = delete;

    // Empty while logged out (nothing is downloaded then).
    void setViewerAssetUrl(std::string url);

    // The material once it has loaded; nullptr until then (or if it failed).
    std::shared_ptr<const assets::Material> get(const Uuid& id);

    // Handles finished reads, downloads and parses.
    void update();

    struct Stats
    {
        size_t ready = 0;
        size_t loading = 0;
        size_t failed = 0;
    };
    Stats stats() const;

private:
    enum class State : uint8_t
    {
        Loading, // disk cache, then parsing
        Queued,
        Downloading,
        Ready,
        Failed,
    };
    struct Entry
    {
        State state = State::Loading;
        std::shared_ptr<const assets::Material> material;
    };
    // A finished read or parse.
    struct Result
    {
        Uuid id;
        bool onDisk = false; // false: not cached, download it
        std::shared_ptr<const assets::Material> material;
        std::string error;
    };
    struct Download
    {
        Uuid id;
        net::HttpResponse response;
    };

    void parse(const Uuid& id, std::vector<uint8_t> bytes, bool save);
    void startDownloads();
    void submit(std::function<void()> job);

    net::HttpClient& http_;
    JobSystem& jobs_;
    DiskCache& store_;
    std::string viewerAssetUrl_;
    std::unordered_map<Uuid, Entry> entries_;
    std::deque<Uuid> queue_;
    size_t downloading_ = 0;
    std::atomic<int> jobsRunning_{0};

    MessageQueue<Result> results_;
    MessageQueue<Download> downloads_;
    std::vector<Result> drainedResults_;
    std::vector<Download> drainedDownloads_;
    // Last: destroyed first, so no download callback outlives the queues.
    std::unique_ptr<net::HttpRequestGroup> requests_;
};

} // namespace viewer
