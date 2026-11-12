#include "viewer/world/ObjectCache.hpp"

#include <algorithm>
#include <cstring>

namespace viewer::world
{

namespace
{

// "VOC1", CacheID (16 bytes), count, then per object: local id, CRC,
// update flags, size, data. Little-endian.
constexpr uint8_t kMagic[4] = {'V', 'O', 'C', '1'};
constexpr uint32_t kMaxBlock = 4096; // compressed blocks are well under this

void putU32(std::vector<uint8_t>& out, uint32_t v)
{
    for(int i = 0; i < 4; i++)
    {
        out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
}

struct Reader
{
    std::span<const uint8_t> bytes;
    size_t at = 0;
    bool ok = true;

    std::span<const uint8_t> take(size_t n)
    {
        if(!ok || bytes.size() - at < n)
        {
            ok = false;
            return {};
        }
        const auto out = bytes.subspan(at, n);
        at += n;
        return out;
    }
    uint32_t u32()
    {
        const auto b = take(4);
        uint32_t v = 0;
        for(size_t i = 0; i < b.size(); i++)
        {
            v |= static_cast<uint32_t>(b[i]) << (8 * i);
        }
        return v;
    }
};

} // namespace

const CachedObject* RegionObjectCache::find(uint32_t localId, uint32_t crc) const
{
    const auto it = objects_.find(localId);
    return it != objects_.end() && it->second.crc == crc ? &it->second : nullptr;
}

void RegionObjectCache::store(uint32_t localId, CachedObject object)
{
    auto& slot = objects_[localId];
    if(slot.crc == object.crc && slot.data == object.data && slot.updateFlags == object.updateFlags)
    {
        return;
    }
    slot = std::move(object);
    dirty_ = true;
}

void RegionObjectCache::erase(uint32_t localId)
{
    dirty_ = objects_.erase(localId) > 0 || dirty_;
}

std::vector<uint8_t> RegionObjectCache::serialize() const
{
    std::vector<uint8_t> out(std::begin(kMagic), std::end(kMagic));
    out.insert(out.end(), cacheId_.bytes.begin(), cacheId_.bytes.end());
    putU32(out, static_cast<uint32_t>(objects_.size()));
    for(const auto& [id, object] : objects_)
    {
        putU32(out, id);
        putU32(out, object.crc);
        putU32(out, object.updateFlags);
        putU32(out, static_cast<uint32_t>(object.data.size()));
        out.insert(out.end(), object.data.begin(), object.data.end());
    }
    return out;
}

std::optional<RegionObjectCache> RegionObjectCache::parse(std::span<const uint8_t> bytes,
                                                          const Uuid& cacheId)
{
    Reader r{bytes};
    const auto magic = r.take(4);
    if(!r.ok || std::memcmp(magic.data(), kMagic, 4) != 0)
    {
        return std::nullopt;
    }
    const auto id = r.take(16);
    if(!r.ok || !std::equal(id.begin(), id.end(), cacheId.bytes.begin()))
    {
        return std::nullopt;
    }
    RegionObjectCache cache(cacheId);
    const uint32_t count = r.u32();
    for(uint32_t i = 0; i < count && r.ok; i++)
    {
        const uint32_t localId = r.u32();
        CachedObject object;
        object.crc = r.u32();
        object.updateFlags = r.u32();
        const uint32_t size = r.u32();
        if(size > kMaxBlock)
        {
            return std::nullopt;
        }
        const auto data = r.take(size);
        object.data.assign(data.begin(), data.end());
        cache.objects_.emplace(localId, std::move(object));
    }
    if(!r.ok || r.at != bytes.size())
    {
        return std::nullopt;
    }
    return cache;
}

} // namespace viewer::world
