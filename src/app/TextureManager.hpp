#pragma once

// Textures by asset id, ready for drawing. Asking for one that isn't resident
// starts loading it: from the KTX2 disk cache if it's there, otherwise
// downloaded from the region's ViewerAsset capability, decoded once on a
// worker thread and cached, then uploaded to the GPU (a bounded amount per
// frame). Until then get() returns nullptr and the caller draws a stand-in.
//
// Images are stored bottom-up, like the official viewer, so Second Life
// texture coordinates work unchanged.
//
// The cache and GPU textures outlive sessions; downloads need a session's
// ViewerAsset URL, set with setViewerAssetUrl() (empty while logged out,
// which also cancels downloads in flight).

#include "viewer/assets/TextureCache.hpp"
#include "viewer/core/Uuid.hpp"
#include "viewer/net/HttpClient.hpp"
#include "viewer/render/Renderer.hpp"

#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace viewer
{

class TextureManager
{
public:
    TextureManager(assets::TextureCache& cache, render::Renderer& renderer, net::HttpClient& http);

    void setViewerAssetUrl(std::string url);

    // The GPU texture if it's ready; otherwise starts loading it (once) and
    // returns nullptr. Color textures are sampled as sRGB.
    const render::GpuTexture* get(const Uuid& id);

    // The decoded image, for textures that need CPU-side processing (terrain
    // layers go into an array). Keeps a copy once asked for; nullptr until
    // it's loaded.
    const image::TextureData* data(const Uuid& id);

    // Processes downloads and decodes, and uploads up to a per-frame budget.
    void update();

    struct Stats
    {
        size_t resident = 0;
        size_t loading = 0;     // cache lookups and decodes
        size_t downloading = 0; // HTTP requests in flight
        size_t queued = 0;      // waiting for a download slot
        size_t failed = 0;
        uint64_t bytesDownloaded = 0;
    };
    Stats stats() const;

private:
    enum class State : uint8_t
    {
        CacheLookup,   // asked the disk cache
        WaitingForUrl, // not cached; no ViewerAsset URL yet
        Queued,        // waiting for a download slot
        Downloading,
        Decoding,  // bytes handed to the cache to decode and store
        Uploading, // decoded; waiting for the GPU upload budget
        Resident,
        Missing, // the grid doesn't have it (404)
        Failed,
    };

    struct Entry
    {
        State state = State::CacheLookup;
        render::GpuTexture texture;
        image::TextureData pending; // decoded, waiting for upload
        std::unique_ptr<image::TextureData> cpuCopy;
        bool keepCpuCopy = false;
        int attempts = 0;
        std::chrono::steady_clock::time_point retryAt{};
    };

    struct Download
    {
        Uuid id;
        net::HttpResponse response;
    };

    Entry& entry(const Uuid& id);
    void startDownloads();
    void handleDownload(Download& download);
    void handleCacheResult(assets::TextureResult& result);

    assets::TextureCache& cache_;
    render::Renderer& renderer_;
    net::HttpClient& http_;
    std::string viewerAssetUrl_;

    std::unordered_map<Uuid, Entry> entries_;
    std::deque<Uuid> downloadQueue_;
    std::deque<Uuid> uploadQueue_;
    size_t downloading_ = 0;
    uint64_t bytesDownloaded_ = 0;

    std::vector<assets::TextureResult> cacheResults_;
    std::vector<Download> downloads_;
    MessageQueue<Download> completedDownloads_;
    // Last: destroyed first, so no download callback outlives the queue.
    std::unique_ptr<net::HttpRequestGroup> requests_;
};

} // namespace viewer
