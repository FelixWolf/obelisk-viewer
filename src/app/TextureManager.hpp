#pragma once

// Textures by asset id, ready for drawing. Asking for one that isn't resident
// starts loading it: from the KTX2 disk cache if it's there, otherwise
// downloaded from the region's ViewerAsset capability, decoded once on a
// worker thread and cached, then uploaded to the GPU (a bounded amount per
// frame). Until then get() returns nullptr and the caller draws a stand-in.
//
// Each texture is kept at the size its uses need: callers say how many
// texels across they'd show (texelsFor), and the texture is loaded at the
// largest of those this frame (a power of two, at least kMinSize), shrinking
// only once a smaller size has done for a while. The disk cache keeps every
// mip level, so a new size is a cheap read of the levels wanted. GPU memory
// stays within a budget: textures not drawn lately go first (least recently
// used); if what's in view still doesn't fit, every texture is loaded at
// half the size, and so on (as the official viewer's discard bias).
//
// Images are stored bottom-up, like the official viewer, so Second Life
// texture coordinates work unchanged.
//
// The cache and GPU textures outlive sessions; downloads need a session's
// ViewerAsset URL, set with setViewerAssetUrl() (empty while logged out,
// which also cancels downloads in flight).

#include "viewer/assets/TextureCache.hpp"
#include "viewer/core/JobSystem.hpp"
#include "viewer/core/MessageQueue.hpp"
#include "viewer/core/Uuid.hpp"
#include "viewer/net/HttpClient.hpp"
#include "viewer/render/Renderer.hpp"

#include <glm/glm.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace viewer
{

// How many texels across a texture needs where it's shown `repeats` times
// over something `texels` across (texelsFor): fewer the more it repeats,
// more where it's zoomed in (fewer than one repeat, as a flipbook frame
// is), going by the more zoomed in way; from 1/8 to 8 repeats (as
// LLFace::getTextureVirtualSize divides by the texture's area, clamped).
float repeatedTexels(float texels, glm::vec2 repeats);

class TextureManager
{
public:
    // Texel counts at or above this load the whole image.
    static constexpr float kFullSize = std::numeric_limits<float>::infinity();
    static constexpr uint32_t kMinSize = 32;
    static constexpr uint32_t kMaxSize = 2048;

    TextureManager(assets::TextureCache& cache, render::Renderer& renderer, net::HttpClient& http,
                   JobSystem& jobs);
    // Waits for GPU uploads running on workers.
    ~TextureManager();

    TextureManager(const TextureManager&) = delete;
    TextureManager& operator=(const TextureManager&) = delete;

    void setViewerAssetUrl(std::string url);
    // GPU memory for textures, in bytes.
    void setBudget(uint64_t bytes) { budget_ = bytes; }
    // The view: its height in pixels and vertical field of view (texelsFor).
    void setView(uint32_t heightPixels, float verticalFov);

    // How many texels across a texture needs on something `size` meters
    // across, `distance` meters away (once over it: repeatedTexels).
    float texelsFor(float size, float distance) const;

    // A texture a use asked for (record): kept to ask for it again without
    // looking it up (touch). Entries are never removed, so it stays valid
    // for the manager's life.
    struct TextureUse
    {
        void* entry = nullptr;
        float texels = 0.0f;
        bool linear = false;
        bool resident = false; // it was ready when asked for
    };
    // While recording, every get() and getLinear() is noted in `uses`
    // (null: stop).
    void record(std::vector<TextureUse>* uses) { recording_ = uses; }
    // Asks for a recorded texture again, at `texels`.
    void touch(const TextureUse& use, float texels);

    // The GPU texture if it's ready; otherwise starts loading it (once) and
    // returns nullptr. Color textures are sampled as sRGB. `texels`: how
    // many across this use needs (texelsFor).
    const render::GpuTexture* get(const Uuid& id, float texels = kFullSize);
    // The same, downloading from `url` instead of the ViewerAsset capability
    // (avatar bakes come from the appearance service).
    const render::GpuTexture* get(const Uuid& id, const std::string& url, float texels = kFullSize);
    // The texture sampled as plain data, not sRGB color: normal maps,
    // metallic-roughness maps. A copy of its own, made only when asked for.
    const render::GpuTexture* getLinear(const Uuid& id, float texels = kFullSize);
    const render::GpuTexture* getLinear(const Uuid& id, const std::string& url,
                                        float texels = kFullSize);

    // The decoded image, whole, for textures that need CPU-side processing
    // (terrain layers go into an array). Keeps a copy once asked for;
    // nullptr until it's loaded.
    const image::TextureData* data(const Uuid& id);
    // An avatar bake's morph mask (its fifth component), for the morphs
    // that follow clothing; nullptr until it's known, or if it has none.
    // Changes (another pointer) when a better one arrives.
    std::shared_ptr<const image::Plane> mask(const Uuid& id);
    std::shared_ptr<const image::Plane> mask(const Uuid& id, const std::string& url);

    // How the texture uses alpha, once it's resident.
    std::optional<image::AlphaMode> alphaMode(const Uuid& id) const;

    // Whether a texture asked for is ready to draw, or can't be had.
    bool settled(const Uuid& id) const;

    // Processes downloads and decodes, picks sizes, keeps to the budget, and
    // uploads up to a per-frame amount. Call once a frame, before the
    // frame's get()s.
    void update();

    struct Stats
    {
        size_t resident = 0;    // with a GPU copy
        size_t loading = 0;     // cache reads, decodes and uploads
        size_t downloading = 0; // HTTP requests in flight
        size_t queued = 0;      // waiting for a download slot
        size_t failed = 0;
        size_t evicted = 0; // dropped to keep to the budget, so far
        uint64_t bytesDownloaded = 0;
        uint64_t gpuBytes = 0; // uploaded textures (color and linear copies), mips included
        uint64_t cpuBytes = 0; // decoded data waiting for upload, and kept copies
        uint64_t budget = 0;
        int bias = 0; // sizes are halved this many times
    };
    Stats stats() const;

private:
    enum class State : uint8_t
    {
        Lookup,        // not known to be in the disk cache yet
        WaitingForUrl, // not cached; no ViewerAsset URL yet
        Queued,        // waiting for a download slot
        Downloading,
        Decoding, // bytes handed to the cache to decode and store
        Cached,   // in the disk cache: read at whatever size is needed
        Missing,  // the grid doesn't have it (404, or 503: see AssetResponse.hpp)
        Failed,
    };
    struct Entry
    {
        State state = State::Lookup;
        bool reading = false;  // a disk cache read in flight
        uint32_t readSize = 0; // what it asked for (0: whole)
        int uploading = 0;     // copies handed to upload jobs, not back yet
        render::GpuTexture color;
        render::GpuTexture linear;
        image::AlphaMode alphaMode = image::AlphaMode::Opaque;
        bool wantColor = false;
        bool wantLinear = false;
        bool keepCpuCopy = false;
        // The morph mask: wanted, read (whether or not there's one), and
        // downloaded again once for one decoded before masks were kept.
        bool wantMask = false;
        bool maskKnown = false;
        bool maskRefetched = false;
        std::shared_ptr<const image::Plane> mask;
        image::TextureData pending;       // decoded, waiting for an upload job
        image::TextureData pendingLinear; // the same, for the linear copy
        std::unique_ptr<image::TextureData> cpuCopy;
        uint32_t fullSize = 0; // the whole image's larger side, once known
        // Use: the most texels asked for this frame, and the size kept
        // (the largest lately; it shrinks after a while).
        float wanted = 0.0f;
        float kept = 0.0f;
        std::chrono::steady_clock::time_point keptSince{};
        uint64_t usedFrame = 0;
        uint64_t listedFrame = 0; // when last put in used_
        Uuid id{};
        std::string url; // explicit download URL, if any
        int attempts = 0;
        std::chrono::steady_clock::time_point retryAt{};
    };

    struct Download
    {
        Uuid id;
        net::HttpResponse response;
    };
    struct Uploaded
    {
        std::vector<Uuid> ids;
        std::vector<render::GpuTexture> textures;
        std::vector<image::AlphaMode> alphaModes;
        uint64_t bytes = 0;
        bool linear = false; // a batch of linear copies
    };

    Entry& entry(const Uuid& id);
    void use(Entry& e, float texels);
    // The entries used this frame, for chooseSizes (rather than all of them).
    std::vector<Entry*> used_;
    std::vector<TextureUse>* recording_ = nullptr;
    void startDownloads();
    void handleDownload(Download& download);
    void handleCacheResult(assets::TextureResult& result);
    // Sizes each texture should have now, and the reads that get them.
    void chooseSizes();
    // Evicts until within the budget; adjusts the bias.
    void keepToBudget();
    void startReads();
    // Hands queued textures to upload jobs (a few batches at a time).
    void startUploads();
    // The size (larger side) a texture should be loaded at; 0: whole.
    uint32_t targetSize(const Entry& e) const;
    static uint32_t largerSide(const render::GpuTexture& t);
    bool busy(const Entry& e) const;

    assets::TextureCache& cache_;
    render::Renderer& renderer_;
    net::HttpClient& http_;
    JobSystem& jobs_;
    std::string viewerAssetUrl_;

    std::unordered_map<Uuid, Entry> entries_;
    std::deque<Uuid> downloadQueue_;
    std::deque<std::pair<Uuid, uint32_t>> readQueue_; // (id, size; 0 whole)
    std::deque<Uuid> uploadQueue_;
    std::deque<Uuid> linearUploadQueue_;
    size_t downloading_ = 0;
    size_t reading_ = 0; // disk cache reads in flight (not decodes)
    uint64_t bytesDownloaded_ = 0;

    uint64_t frame_ = 1;
    float pixelsPerRadian_ = 1000.0f;
    uint64_t budget_ = 2048ull << 20;
    uint64_t gpuBytes_ = 0;     // as of the last update
    uint64_t pendingBytes_ = 0; // decoded, not yet uploaded (also in upload jobs)
    int bias_ = 0;
    std::chrono::steady_clock::time_point biasChangedAt_{};
    size_t evicted_ = 0;

    size_t uploadJobs_ = 0; // batches handed to workers, not yet back
    std::atomic<int> jobsRunning_{0};
    MessageQueue<Uploaded> uploaded_;
    std::vector<Uploaded> drainedUploads_;

    std::vector<assets::TextureResult> cacheResults_;
    std::vector<Download> downloads_;
    MessageQueue<Download> completedDownloads_;
    // Last: destroyed first, so no download callback outlives the queue.
    std::unique_ptr<net::HttpRequestGroup> requests_;
};

} // namespace viewer
