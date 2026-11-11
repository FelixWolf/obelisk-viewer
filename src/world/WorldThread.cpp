#include "viewer/world/WorldThread.hpp"

#include <utility>

namespace viewer::world
{

WorldThread::WorldThread() : thread_([this](std::stop_token stop) { run(stop); }) {}

WorldThread::~WorldThread()
{
    thread_.request_stop();
    wake_.notify_all();
    // jthread joins on destruction (it's the last member, so it goes first).
}

void WorldThread::post(uint64_t regionHandle, protocol::CircuitEvent event)
{
    {
        std::lock_guard lock(mutex_);
        inputs_.push_back({regionHandle, false, std::move(event)});
    }
    wake_.notify_one();
}

void WorldThread::postRegionRemoved(uint64_t regionHandle)
{
    {
        std::lock_guard lock(mutex_);
        inputs_.push_back({regionHandle, true, {}});
    }
    wake_.notify_one();
}

void WorldThread::drain(std::vector<WorldUpdate>& out)
{
    out.clear();
    std::lock_guard lock(mutex_);
    std::swap(out, outputs_);
}

void WorldThread::flush()
{
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [&] { return inputs_.empty() && !busy_; });
}

void WorldThread::run(std::stop_token stop)
{
    std::vector<Input> batch;
    std::vector<WorldUpdate> decoded;
    while(!stop.stop_requested())
    {
        {
            std::unique_lock lock(mutex_);
            busy_ = false;
            idle_.notify_all();
            if(!wake_.wait(lock, stop, [&] { return !inputs_.empty(); }))
            {
                return; // stopping
            }
            batch.assign(std::make_move_iterator(inputs_.begin()),
                         std::make_move_iterator(inputs_.end()));
            inputs_.clear();
            busy_ = true;
        }

        // The work, outside the lock.
        decoded.clear();
        for(Input& in : batch)
        {
            if(in.removed)
            {
                WorldUpdate removed;
                removed.region = in.region;
                removed.change = WorldUpdate::RegionRemoved{};
                decoded.push_back(std::move(removed));
                continue;
            }
            WorldUpdate update = decodeMessage(in.region, in.event);
            if(!std::holds_alternative<std::monostate>(update.change))
            {
                decoded.push_back(std::move(update));
            }
        }
        batch.clear();

        std::lock_guard lock(mutex_);
        for(WorldUpdate& u : decoded)
        {
            outputs_.push_back(std::move(u));
        }
    }
}

} // namespace viewer::world
