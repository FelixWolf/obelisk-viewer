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

    // 1. The KTX2, if it's big enough (or whole).
    const CacheKey key{CacheKind::Texture, request.id};
    const CacheKey sourceKey{CacheKind::TextureSource, request.id};
    std::optional<image::TextureData> cached;
    if(auto file = store_.read(key))
    {
        cached = image::readKtx2(*file, {.maxDimension = request.maxDimension});
        if(!cached)
        {
            log::warn("texture cache: corrupt entry {}, re-decoding", request.id.toString());
        }
    }
    if(cached)
    {
        const uint32_t stored =
            std::max(cached->fullWidth, cached->fullHeight) >> cached->discardLevel;
        if(cached->discardLevel == 0 ||
           (request.maxDimension != 0 && stored >= request.maxDimension))
        {
            result.texture = std::move(*cached);
            return finish(TextureResult::Status::LoadedFromCache);
        }
    }

    // 2. Decode a bigger one: from the J2K kept beside it, else the source.
    std::optional<std::vector<uint8_t>> j2k = store_.read(sourceKey);
    const bool kept = j2k.has_value();
    if(!j2k && request.source)
    {
        j2k = request.source();
    }
    if(!j2k)
    {
        if(cached)
        {
            // The best there is without downloading.
            result.texture = std::move(*cached);
            result.needsSource = true;
            return finish(TextureResult::Status::LoadedFromCache);
        }
        return finish(TextureResult::Status::NotFound);
    }
    const auto info = image::readJ2kInfo(*j2k);
    const uint32_t discard = info ? image::discardLevelFor(*info, request.maxDimension) : 0;
    auto texture = image::textureFromJ2k(decoder_, *j2k, discard);
    if(!texture && discard > 0)
    {
        texture = image::textureFromJ2k(decoder_, *j2k, 0); // whole, then
    }
    if(!texture)
    {
        if(kept)
        {
            store_.remove(sourceKey); // bad bytes: download them again next time
        }
        return finish(TextureResult::Status::Failed);
    }
    const std::vector<uint8_t> ktx = image::writeKtx2(*texture);
    if(ktx.empty() || !store_.write(key, ktx))
    {
        log::warn("texture cache: failed to store {}", request.id.toString());
    }
    // Keep the J2K while the KTX2 is partial; whole, it's no longer needed.
    if(texture->discardLevel > 0 && !kept)
    {
        store_.write(sourceKey, *j2k);
    }
    else if(texture->discardLevel == 0 && kept)
    {
        store_.remove(sourceKey);
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
