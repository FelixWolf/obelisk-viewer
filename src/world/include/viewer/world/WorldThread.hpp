#pragma once

// Decodes the regions' messages for the world on a thread of its own, so the
// render thread only applies the results (World::apply), which is cheap.
//
// Messages go in with post() in arrival order and come out of drain() as
// WorldUpdates in the same order. Nothing is shared but the two queues.
// Destroying it stops the thread (messages not decoded yet are dropped).

#include "viewer/protocol/CircuitManager.hpp"
#include "viewer/world/World.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
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

    // Moves every decoded update into `out` (cleared first), oldest first.
    void drain(std::vector<WorldUpdate>& out);

    // Blocks until everything posted so far has been decoded (for tests and
    // tools).
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
    std::vector<WorldUpdate> outputs_;
    std::jthread thread_; // last: started once everything else exists
};

} // namespace viewer::world
