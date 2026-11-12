#pragma once

// Disk cache of decoded textures, stored as KTX2. JPEG 2000 is decoded at
// most once per asset; the J2K bytes themselves are never written to disk.
//
// All work runs on the JobSystem. Callers submit requests and later drain
// completed results on their own thread (normally the main thread, which
// then uploads to the GPU).
//
// Entries are stored through a DiskCache (CacheKind::Texture).

#include "viewer/core/DiskCache.hpp"
#include "viewer/core/JobSystem.hpp"
#include "viewer/core/MessageQueue.hpp"
#include "viewer/core/Uuid.hpp"
#include "viewer/image/J2kDecoder.hpp"
#include "viewer/image/TextureData.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <vector>

namespace viewer::assets
{

// Supplies J2K bytes on a cache miss. Runs on a worker thread, so it may do
// blocking I/O. Returns nullopt if the bytes are unavailable.
using J2kSource = std::function<std::optional<std::vector<uint8_t>>()>;

struct TextureRequest
{
    Uuid id;
    J2kSource source{};        // may be empty: then only the cache is consulted
    uint32_t discardLevel = 0; // J2K resolution level to decode on a miss
    uint32_t maxDimension = 0; // drop larger mips from the *result* (the cache keeps all)
    bool flipRows = false;     // the result bottom row first (image::flipRows), on the worker
};

struct TextureResult
{
    enum class Status : uint8_t
    {
        LoadedFromCache, // read the KTX2; no decode
        Decoded,         // decoded J2K and wrote a new cache entry
        NotFound,        // not cached, and no source (or the source had no data)
        Failed,          // corrupt cache entry or undecodable J2K
    };

    Uuid id;
    Status status = Status::Failed;
    image::TextureData texture{}; // empty unless LoadedFromCache or Decoded
    double milliseconds = 0;      // worker time spent on this request
};

class TextureCache
{
public:
    // `store` and `jobs` must outlive the cache.
    TextureCache(DiskCache& store, JobSystem& jobs, const image::J2kDecoder& decoder);

    // Stops accepting work and waits for this cache's queued and running jobs
    // (queued ones exit immediately). The JobSystem must outlive the cache.
    ~TextureCache();

    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;

    // Queues a request. Ignored if the same id is already in flight.
    // Returns false if it was ignored.
    bool request(TextureRequest request);

    // Moves completed results into `out` (cleared first).
    void drainResults(std::vector<TextureResult>& out) { results_.drain(out); }

    bool contains(const Uuid& id) const;
    // Where entries are stored, for showing to the user.
    std::string describe() const { return store_.describe(); }

    size_t inFlight() const;

    // Deletes every cache entry. Requests already in flight are unaffected.
    void clear();

private:
    TextureResult process(const TextureRequest& request) const;

    DiskCache& store_;
    JobSystem& jobs_;
    const image::J2kDecoder& decoder_;
    MessageQueue<TextureResult> results_;

    mutable std::mutex mutex_;
    std::condition_variable drained_;
    std::unordered_set<Uuid> inFlight_;
    bool shuttingDown_ = false;
};

} // namespace viewer::assets
