#pragma once

// Animation assets by id, parsed and bound to the avatar skeleton. Asking
// for one starts loading it: from the disk cache (the asset as downloaded),
// else from the region's ViewerAsset capability
// (<cap>/?animatn_id=<uuid>, as the official viewer). Reading, parsing and
// writing run on worker threads.

#include "viewer/avatar/Animation.hpp"
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

class AnimationManager
{
public:
    AnimationManager(net::HttpClient& http, JobSystem& jobs, avatar::Skeleton skeleton,
                     DiskCache& store);
    // Cancels downloads and waits for jobs.
    ~AnimationManager();

    AnimationManager(const AnimationManager&) = delete;
    AnimationManager& operator=(const AnimationManager&) = delete;

    // Empty while logged out (nothing is downloaded then).
    void setViewerAssetUrl(std::string url);

    // The animation once it has loaded; nullptr until then (or if it failed).
    std::shared_ptr<const avatar::Animation> get(const Uuid& id);

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
        std::shared_ptr<const avatar::Animation> animation;
    };
    // A finished read or parse.
    struct Result
    {
        Uuid id;
        bool onDisk = false; // false: not cached, download it
        std::shared_ptr<const avatar::Animation> animation;
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
    const avatar::Skeleton skeleton_;
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
