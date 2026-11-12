#include "viewer/assets/TextureCache.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/image/Ktx2.hpp"
#include "viewer/image/TextureProcessing.hpp"

#include <chrono>
#include <thread>

namespace viewer::assets
{

namespace
{

void trimToMaxDimension(image::TextureData& texture, uint32_t maxDimension)
{
    if(maxDimension == 0)
    {
        return;
    }
    size_t first = 0;
    while(first + 1 < texture.levels.size() &&
          std::max(texture.levels[first].width, texture.levels[first].height) > maxDimension)
    {
        first++;
    }
    texture.levels.erase(texture.levels.begin(),
                         texture.levels.begin() + static_cast<ptrdiff_t>(first));
}

} // namespace

TextureCache::TextureCache(DiskCache& store, JobSystem& jobs, const image::J2kDecoder& decoder)
    : store_(store), jobs_(jobs), decoder_(decoder)
{
}

TextureCache::~TextureCache()
{
    std::unique_lock lock(mutex_);
    shuttingDown_ = true;
    drained_.wait(lock, [this] { return inFlight_.empty(); });
}

bool TextureCache::request(TextureRequest request)
{
    {
        std::lock_guard lock(mutex_);
        if(shuttingDown_ || !inFlight_.insert(request.id).second)
        {
            return false;
        }
    }

    jobs_.submit(
        [this, request = std::move(request)]
        {
            bool cancelled;
            {
                std::lock_guard lock(mutex_);
                cancelled = shuttingDown_;
            }
            if(!cancelled)
            {
                results_.push(process(request));
            }
            std::lock_guard lock(mutex_);
            inFlight_.erase(request.id);
            if(inFlight_.empty())
            {
                drained_.notify_all();
            }
        });
    return true;
}

TextureResult TextureCache::process(const TextureRequest& request) const
{
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();

    TextureResult result;
    result.id = request.id;
    auto finish = [&](TextureResult::Status status)
    {
        if(request.flipRows && (status == TextureResult::Status::LoadedFromCache ||
                                status == TextureResult::Status::Decoded))
        {
            image::flipRows(result.texture);
        }
        result.status = status;
        result.milliseconds =
            std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        return std::move(result);
    };

    // 1. Cache hit: read the KTX2 (only the levels the caller wants).
    const CacheKey key{CacheKind::Texture, request.id};
    if(auto file = store_.read(key))
    {
        if(auto texture = image::readKtx2(*file, {.maxDimension = request.maxDimension}))
        {
            // An entry decoded at a coarser level than requested is a miss
            // if we can fetch better data.
            if(texture->discardLevel <= request.discardLevel || !request.source)
            {
                result.texture = std::move(*texture);
                return finish(TextureResult::Status::LoadedFromCache);
            }
        }
        else
        {
            log::warn("texture cache: corrupt entry {}, re-decoding", request.id.toString());
        }
    }

    // 2. Miss: fetch J2K, decode, write the cache entry.
    if(!request.source)
    {
        return finish(TextureResult::Status::NotFound);
    }
    const auto j2k = request.source();
    if(!j2k)
    {
        return finish(TextureResult::Status::NotFound);
    }
    auto texture = image::textureFromJ2k(decoder_, *j2k, request.discardLevel);
    if(!texture)
    {
        return finish(TextureResult::Status::Failed);
    }
    const std::vector<uint8_t> ktx = image::writeKtx2(*texture);
    if(ktx.empty() || !store_.write(key, ktx))
    {
        log::warn("texture cache: failed to store {}", request.id.toString());
    }

    trimToMaxDimension(*texture, request.maxDimension);
    result.texture = std::move(*texture);
    return finish(TextureResult::Status::Decoded);
}

bool TextureCache::contains(const Uuid& id) const
{
    return store_.contains({CacheKind::Texture, id});
}

size_t TextureCache::inFlight() const
{
    std::lock_guard lock(mutex_);
    return inFlight_.size();
}

void TextureCache::clear()
{
    store_.clear(CacheKind::Texture);
}

} // namespace viewer::assets
