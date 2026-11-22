#pragma once

// Sound assets by id, decoded and ready to play. Asking for one starts
// loading it: from the disk cache (the Ogg Vorbis as downloaded), else from
// the region's ViewerAsset capability (<cap>/?sound_id=<uuid>, as the
// official viewer). Reading and decoding run on worker threads. Clips not
// asked for in a while are let go (the disk keeps them).

#include "viewer/audio/SoundClip.hpp"
#include "viewer/core/DiskCache.hpp"
#include "viewer/core/JobSystem.hpp"
#include "viewer/core/MessageQueue.hpp"
#include "viewer/core/Uuid.hpp"
#include "viewer/net/HttpClient.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace viewer
{

class SoundAssets
{
public:
    SoundAssets(net::HttpClient& http, JobSystem& jobs, DiskCache& store);
    // Cancels downloads and waits for jobs.
    ~SoundAssets();
    SoundAssets(const SoundAssets&) = delete;
    SoundAssets& operator=(const SoundAssets&) = delete;

    // Empty while logged out (nothing is downloaded then).
    void setViewerAssetUrl(std::string url);

    // The clip once it's decoded; nullptr until then (or if it failed).
    std::shared_ptr<const audio::SoundClip> get(const Uuid& id);
    // It failed for good (missing, or not a sound).
    bool failed(const Uuid& id) const;

    // Handles finished reads, downloads and decodes; lets go of old clips.
    void update();

    struct Stats
    {
        size_t ready = 0;
        size_t loading = 0;
        size_t failed = 0;
        size_t bytes = 0; // decoded, kept
    };
    Stats stats() const;
    // Logged out: nothing kept.
    void clear();

private:
    using Clock = std::chrono::steady_clock;
    enum class State : uint8_t
    {
        Loading, // disk cache, then decoding
        Queued,
        Downloading,
        Ready,
        Failed,
    };
    struct Entry
    {
        State state = State::Loading;
        std::shared_ptr<const audio::SoundClip> clip;
        Clock::time_point used{};
    };
    struct Result
    {
        Uuid id;
        bool onDisk = false; // false: not cached, download it
        std::shared_ptr<const audio::SoundClip> clip;
    };
    struct Download
    {
        Uuid id;
        net::HttpResponse response;
    };

    void decode(const Uuid& id, std::vector<uint8_t> bytes, bool save);
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
    std::shared_ptr<MessageQueue<Result>> results_ = std::make_shared<MessageQueue<Result>>();
    MessageQueue<Download> downloads_;
    std::vector<Result> drainedResults_;
    std::vector<Download> drainedDownloads_;
    Clock::time_point lastSweep_{};
    std::unique_ptr<net::HttpRequestGroup> requests_;
};

} // namespace viewer
