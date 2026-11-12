#include "viewer/world/WorldThread.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/protocol/Messages.hpp"

#include <utility>

namespace viewer::world
{

namespace msg = protocol::msg;

WorldThread::WorldThread(DiskCache* store)
    : published_(std::make_shared<const World>()), store_(store),
      thread_([this](std::stop_token stop) { run(stop); })
{
}

WorldThread::~WorldThread()
{
    thread_.request_stop();
    wake_.notify_all();
    thread_.join();
    for(const auto& [handle, cache] : caches_)
    {
        saveCache(cache);
    }
}

void WorldThread::saveCache(const RegionCache& cache)
{
    if(!store_ || !cache.objects.dirty() || cache.regionId.isNull())
    {
        return;
    }
    if(!store_->write({CacheKind::Objects, cache.regionId}, cache.objects.serialize()))
    {
        log::warn("couldn't save the object cache of region {}", cache.regionId.toString());
    }
}

void WorldThread::loadCache(uint64_t region, const protocol::CircuitEvent& event)
{
    const auto handshake = event.decode<msg::RegionHandshake>();
    if(!handshake)
    {
        return;
    }
    const Uuid regionId = handshake->RegionInfo2.RegionID;
    const Uuid cacheId = handshake->RegionInfo.CacheID;
    auto it = caches_.find(region);
    if(it == caches_.end() || it->second.regionId != regionId ||
       it->second.objects.cacheId() != cacheId)
    {
        if(it != caches_.end())
        {
            saveCache(it->second);
        }
        RegionCache cache{regionId, RegionObjectCache(cacheId)};
        if(store_ && !regionId.isNull())
        {
            if(const auto bytes = store_->read({CacheKind::Objects, regionId}))
            {
                if(auto loaded = RegionObjectCache::parse(*bytes, cacheId))
                {
                    cache.objects = std::move(*loaded);
                }
                // else: another instance of the region (or damaged): start over.
            }
        }
        log::info("object cache for region {}: {} objects", regionId.toString(),
                  cache.objects.size());
        it = caches_.insert_or_assign(region, std::move(cache)).first;
    }
    requests_.push(
        WorldRequest{WorldRequest::Kind::HandshakeReply, region, !it->second.objects.empty(), {}});
}

void WorldThread::answerProbes(uint64_t region, const protocol::CircuitEvent& event)
{
    const auto probes = event.decode<msg::ObjectUpdateCached>();
    if(!probes)
    {
        return;
    }
    const auto it = caches_.find(region);
    WorldUpdate::Objects found;
    found.received = event.received;
    found.timeDilation = static_cast<float>(probes->RegionData.TimeDilation) / 65535.0f;
    WorldRequest missing{WorldRequest::Kind::SendObjects, region, false, {}};
    for(const auto& probe : probes->ObjectData)
    {
        const CachedObject* cached =
            it != caches_.end() ? it->second.objects.find(probe.ID, probe.CRC) : nullptr;
        auto update =
            cached ? decodeCompressedObject(cached->data, probe.UpdateFlags) : std::nullopt;
        if(update)
        {
            hits_++;
            if(!world_.object(region, probe.ID))
            {
                found.compressed++;
                found.updates.push_back(std::move(*update));
            }
            continue;
        }
        misses_++;
        const bool known = it != caches_.end() && it->second.objects.contains(probe.ID);
        missing.objects.push_back(
            {probe.ID, known ? WorldRequest::Miss::Crc : WorldRequest::Miss::Total});
    }
    if(!found.updates.empty())
    {
        world_.apply(WorldUpdate{.region = region, .change = std::move(found)});
    }
    if(!missing.objects.empty())
    {
        requests_.push(std::move(missing));
    }
}

void WorldThread::cacheUpdates(uint64_t region, const WorldUpdate& update)
{
    const auto it = caches_.find(region);
    if(it == caches_.end())
    {
        return;
    }
    RegionObjectCache& cache = it->second.objects;
    if(const auto* objects = std::get_if<WorldUpdate::Objects>(&update.change))
    {
        for(const ObjectUpdateData& u : objects->updates)
        {
            if(!u.compressed.empty())
            {
                cache.store(u.localId, CachedObject{u.crc, u.full.updateFlags, u.compressed});
                stored_++;
            }
            else
            {
                // Terse or uncompressed updates: it moves or changes too
                // often to be worth caching (as llviewerregion.cpp).
                cache.erase(u.localId);
            }
        }
    }
    else if(const auto* kill = std::get_if<WorldUpdate::Kill>(&update.change))
    {
        for(const uint32_t id : kill->localIds)
        {
            cache.erase(id);
        }
    }
}

void WorldThread::handle(Input& in)
{
    if(in.removed)
    {
        if(const auto it = caches_.find(in.region); it != caches_.end())
        {
            saveCache(it->second);
            caches_.erase(it);
        }
        WorldUpdate removed;
        removed.region = in.region;
        removed.change = WorldUpdate::RegionRemoved{};
        world_.apply(std::move(removed));
        return;
    }
    if(in.event.is<msg::RegionHandshake>())
    {
        loadCache(in.region, in.event);
    }
    else if(in.event.is<msg::ObjectUpdateCached>())
    {
        answerProbes(in.region, in.event);
        return;
    }
    WorldUpdate update = decodeMessage(in.region, in.event);
    if(!std::holds_alternative<std::monostate>(update.change))
    {
        cacheUpdates(in.region, update);
        world_.apply(std::move(update));
    }
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
            handle(in);
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
