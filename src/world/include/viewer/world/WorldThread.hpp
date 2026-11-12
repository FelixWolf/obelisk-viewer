#pragma once

// The world's own thread: it owns the session's World, decodes the regions'
// messages and applies them, and publishes copies of the world for the
// render thread to read.
//
// Messages go in with post() in arrival order. snapshot() returns the latest
// published copy; whenever the render thread has taken one and the world
// has changed since, the thread publishes another. Copies share everything
// unchanged (see World), so publishing costs a pointer per object, not the
// objects. Snapshots the render thread lets go of are released here, on the
// world thread, so that cost stays off the frame too.
//
// It also keeps each region's object cache (RegionObjectCache): loaded
// when the region's handshake arrives, filled from compressed updates,
// answering the region's cache probes (ObjectUpdateCached) from disk, and
// saved when the region goes away or the thread stops. What the regions
// must be told comes back through drainRequests(): the handshake reply
// (once the cache is loaded) and the objects to send in full.
//
// Nothing else is shared. Destroying it stops the thread (messages not
// handled yet are dropped) and saves the object caches.

#include "viewer/core/DiskCache.hpp"
#include "viewer/core/MessageQueue.hpp"
#include "viewer/protocol/CircuitManager.hpp"
#include "viewer/world/ObjectCache.hpp"
#include "viewer/world/World.hpp"

#include <atomic>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace viewer::world
{

// Something a region must be told, for the session to send.
struct WorldRequest
{
    enum class Kind : uint8_t
    {
        HandshakeReply, // the region's object cache is loaded
        SendObjects,    // cache probes we have no (current) copy of
    };
    // RequestMultipleObjects' CacheMissType (llviewerregion.h).
    enum class Miss : uint8_t
    {
        Total = 0, // not in the cache
        Crc = 1,   // in the cache, but changed
    };
    struct Object
    {
        uint32_t localId = 0;
        Miss miss = Miss::Total;
    };

    Kind kind = Kind::HandshakeReply;
    uint64_t region = 0;
    bool haveCache = false; // HandshakeReply: we have objects cached for it
    std::vector<Object> objects{};
};

class WorldThread
{
public:
    // `store` (may be null: no object cache on disk) must outlive the thread.
    explicit WorldThread(DiskCache* store = nullptr);
    ~WorldThread();

    WorldThread(const WorldThread&) = delete;
    WorldThread& operator=(const WorldThread&) = delete;

    // A message from the region with this handle.
    void post(uint64_t regionHandle, protocol::CircuitEvent event);
    // The region's circuit is gone: its terrain and objects go too (in order
    // with the messages before it).
    void postRegionRemoved(uint64_t regionHandle);

    // The latest published world (never null; empty at first). Asking for it
    // is also what makes the thread publish the next one.
    std::shared_ptr<const World> snapshot();
    // Hands a snapshot back to be released on the world thread.
    void retire(std::shared_ptr<const World> snapshot);

    // Blocks until everything posted so far is applied and published (for
    // tests and tools).
    void flush();

    // What the regions must be told (cleared first).
    void drainRequests(std::vector<WorldRequest>& out) { requests_.drain(out); }

    struct CacheStats
    {
        uint64_t hits = 0;   // probes answered from the cache
        uint64_t misses = 0; // probes we had to ask for
        uint64_t stored = 0; // objects added or changed
    };
    CacheStats cacheStats() const { return {hits_.load(), misses_.load(), stored_.load()}; }

private:
    struct Input
    {
        uint64_t region = 0;
        bool removed = false;
        protocol::CircuitEvent event;
    };

    struct RegionCache
    {
        Uuid regionId{};
        RegionObjectCache objects{};
    };

    void run(std::stop_token stop);
    void handle(Input& in);
    void loadCache(uint64_t region, const protocol::CircuitEvent& event);
    void answerProbes(uint64_t region, const protocol::CircuitEvent& event);
    void cacheUpdates(uint64_t region, const WorldUpdate& update);
    void saveCache(const RegionCache& cache);

    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::condition_variable_any idle_;
    std::deque<Input> inputs_;
    bool busy_ = false;
    bool wanted_ = true; // the render thread took the last snapshot
    bool dirty_ = false; // the world changed since the last snapshot
    std::shared_ptr<const World> published_;
    std::vector<std::shared_ptr<const World>> retired_;
    World world_; // the world thread's
    DiskCache* store_ = nullptr;
    std::unordered_map<uint64_t, RegionCache> caches_; // world thread only
    MessageQueue<WorldRequest> requests_;
    std::atomic<uint64_t> hits_{0};
    std::atomic<uint64_t> misses_{0};
    std::atomic<uint64_t> stored_{0};
    std::jthread thread_; // last: started once everything else exists
};

} // namespace viewer::world
