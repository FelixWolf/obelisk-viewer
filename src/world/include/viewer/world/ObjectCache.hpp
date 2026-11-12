#pragma once

// One region's object cache. Simulators mark most objects cacheable and
// send them as compressed updates; we keep each one's block as received,
// with its checksum (CRC). On a later visit the simulator only sends
// (local id, CRC) pairs (ObjectUpdateCached): where the CRC matches we use
// our copy, the rest we ask for (RequestMultipleObjects). Checked against
// llvocache.cpp and llviewerregion.cpp.
//
// A cache belongs to one instance of a region: its handshake's CacheID.
// A different CacheID (the region restarted, so local ids changed) means
// the cache is worthless. Stored through DiskCache (CacheKind::Objects, by
// region id) in our own format: see serialize().

#include "viewer/core/Uuid.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace viewer::world
{

struct CachedObject
{
    uint32_t crc = 0;
    uint32_t updateFlags = 0;
    std::vector<uint8_t> data; // the ObjectUpdateCompressed block
};

class RegionObjectCache
{
public:
    explicit RegionObjectCache(Uuid cacheId = {}) : cacheId_(cacheId) {}

    const Uuid& cacheId() const { return cacheId_; }
    bool empty() const { return objects_.empty(); }
    size_t size() const { return objects_.size(); }
    // Changed since loaded (or since markSaved()).
    bool dirty() const { return dirty_; }
    void markSaved() { dirty_ = false; }

    // The object, if we have it with this CRC.
    const CachedObject* find(uint32_t localId, uint32_t crc) const;
    bool contains(uint32_t localId) const { return objects_.contains(localId); }
    void store(uint32_t localId, CachedObject object);
    void erase(uint32_t localId);

    std::vector<uint8_t> serialize() const;
    // Nothing if the bytes are damaged or for another CacheID.
    static std::optional<RegionObjectCache> parse(std::span<const uint8_t> bytes,
                                                  const Uuid& cacheId);

private:
    Uuid cacheId_;
    std::unordered_map<uint32_t, CachedObject> objects_;
    bool dirty_ = false;
};

} // namespace viewer::world
