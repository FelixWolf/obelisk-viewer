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
// Nothing else is shared. Destroying it stops the thread (messages not
// handled yet are dropped).

#include "viewer/protocol/CircuitManager.hpp"
#include "viewer/world/World.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace viewer::world
{

class WorldThread
{
public:
    WorldThread();
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

private:
    struct Input
    {
        uint64_t region = 0;
        bool removed = false;
        protocol::CircuitEvent event;
    };

    void run(std::stop_token stop);

    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::condition_variable_any idle_;
    std::deque<Input> inputs_;
    bool busy_ = false;
    bool wanted_ = true; // the render thread took the last snapshot
    bool dirty_ = false; // the world changed since the last snapshot
    std::shared_ptr<const World> published_;
    std::vector<std::shared_ptr<const World>> retired_;
    World world_;         // the world thread's
    std::jthread thread_; // last: started once everything else exists
};

} // namespace viewer::world
