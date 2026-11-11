#include "viewer/world/WorldThread.hpp"

#include <utility>

namespace viewer::world
{

WorldThread::WorldThread()
    : published_(std::make_shared<const World>()),
      thread_([this](std::stop_token stop) { run(stop); })
{
}

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

std::shared_ptr<const World> WorldThread::snapshot()
{
    bool publish = false;
    std::shared_ptr<const World> latest;
    {
        std::lock_guard lock(mutex_);
        latest = published_;
        publish = !wanted_ && dirty_;
        wanted_ = true;
    }
    if(publish)
    {
        wake_.notify_one(); // changes are waiting for a snapshot
    }
    return latest;
}

void WorldThread::retire(std::shared_ptr<const World> snapshot)
{
    if(!snapshot)
    {
        return;
    }
    std::lock_guard lock(mutex_);
    retired_.push_back(std::move(snapshot));
}

void WorldThread::flush()
{
    std::unique_lock lock(mutex_);
    while(!inputs_.empty() || busy_ || dirty_)
    {
        // Keep asking: a batch that lands after a snapshot needs another.
        wanted_ = true;
        wake_.notify_one();
        idle_.wait(lock);
    }
}

void WorldThread::run(std::stop_token stop)
{
    std::vector<Input> batch;
    std::vector<std::shared_ptr<const World>> retired;
    while(!stop.stop_requested())
    {
        bool publish = false;
        {
            std::unique_lock lock(mutex_);
            busy_ = false;
            idle_.notify_all();
            if(!wake_.wait(
                   lock, stop,
                   [&] { return !inputs_.empty() || (wanted_ && dirty_) || !retired_.empty(); }))
            {
                return; // stopping
            }
            batch.assign(std::make_move_iterator(inputs_.begin()),
                         std::make_move_iterator(inputs_.end()));
            inputs_.clear();
            std::swap(retired, retired_);
            busy_ = true;
        }
        retired.clear(); // released here, not on the render thread

        // The work, outside the lock: decode and apply.
        for(Input& in : batch)
        {
            if(in.removed)
            {
                WorldUpdate removed;
                removed.region = in.region;
                removed.change = WorldUpdate::RegionRemoved{};
                world_.apply(std::move(removed));
                continue;
            }
            WorldUpdate update = decodeMessage(in.region, in.event);
            if(!std::holds_alternative<std::monostate>(update.change))
            {
                world_.apply(std::move(update));
            }
        }
        const bool changed = !batch.empty();
        batch.clear();

        {
            std::lock_guard lock(mutex_);
            dirty_ = dirty_ || changed;
            publish = wanted_ && dirty_;
        }
        if(publish)
        {
            // Copying shares everything; only the containers are new.
            auto copy = std::make_shared<const World>(world_);
            std::shared_ptr<const World> old;
            {
                std::lock_guard lock(mutex_);
                old = std::exchange(published_, std::move(copy));
                wanted_ = false;
                dirty_ = false;
            }
            // `old` is released here, on this thread.
        }
    }
}

} // namespace viewer::world
