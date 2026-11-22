#include "TextureManager.hpp"

#include "AssetResponse.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/image/TextureProcessing.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <thread>

namespace viewer
{

namespace
{

constexpr size_t kMaxDownloads = 16;
constexpr int kMaxAttempts = 3;
constexpr size_t kUploadsPerBatch = 16;
constexpr uint64_t kUploadBytesPerBatch = 8ull << 20;
// Disk cache reads in flight, and decoded bytes waiting for the GPU: reads
// stop while either is reached, so decoding can't run far ahead of uploads.
constexpr size_t kMaxReads = 16;
constexpr uint64_t kMaxPendingBytes = 256ull << 20;
// A texture keeps its size this long after it was last needed.
constexpr auto kShrinkAfter = std::chrono::seconds(5);
// Textures not drawn for this many frames may be evicted, and aren't
// resized meanwhile.
constexpr uint64_t kIdleFrames = 120;
constexpr int kMaxBias = 4;

uint64_t textureBytes(const render::GpuTexture& t)
{
    if(!t)
    {
        return 0;
    }
    const uint64_t base = uint64_t(t.width()) * t.height() * 4;
    return t.levels() > 1 ? base * 4 / 3 : base;
}

uint64_t dataBytes(const image::TextureData& t)
{
    uint64_t bytes = 0;
    for(const auto& level : t.levels)
    {
        bytes += level.byteSize();
    }
    return bytes;
}

uint32_t dataSide(const image::TextureData& t)
{
    return t.levels.empty() ? 0 : std::max(t.width(), t.height());
}

} // namespace

TextureManager::TextureManager(assets::TextureCache& cache, render::Renderer& renderer,
                               net::HttpClient& http, JobSystem& jobs)
    : cache_(cache), renderer_(renderer), http_(http), jobs_(jobs),
      requests_(std::make_unique<net::HttpRequestGroup>(http))
{
}

TextureManager::~TextureManager()
{
    requests_.reset();
    while(jobsRunning_.load() > 0)
    {
        std::this_thread::yield();
    }
}

void TextureManager::setViewerAssetUrl(std::string url)
{
    if(url == viewerAssetUrl_)
    {
        return;
    }
    viewerAssetUrl_ = std::move(url);

    // Downloads belong to the old URL (or session): drop them and queue
    // their textures again.
    requests_ = std::make_unique<net::HttpRequestGroup>(http_);
    downloading_ = 0;
    std::vector<Download> stale;
    completedDownloads_.drain(stale);
    downloadQueue_.clear();
    for(auto& [id, e] : entries_)
    {
        if(e.state == State::Downloading || e.state == State::Queued ||
           e.state == State::WaitingForUrl)
        {
            e.state = viewerAssetUrl_.empty() ? State::WaitingForUrl : State::Queued;
            if(e.state == State::Queued)
            {
                downloadQueue_.push_back(id);
            }
        }
    }
}

void TextureManager::setView(uint32_t heightPixels, float verticalFov)
{
    // The focal length in pixels: pixels per radian near the center.
    pixelsPerRadian_ = static_cast<float>(heightPixels) /
                       (2.0f * std::tan(std::clamp(verticalFov, 0.01f, 3.0f) * 0.5f));
}

float repeatedTexels(float texels, glm::vec2 repeats)
{
    const float r = std::clamp(std::min(std::abs(repeats.x), std::abs(repeats.y)), 0.125f, 8.0f);
    return texels / r;
}

float TextureManager::texelsFor(float size, float distance) const
{
    return size / std::max(distance, 0.01f) * pixelsPerRadian_;
}

TextureManager::Entry& TextureManager::entry(const Uuid& id)
{
    // New entries are looked up in the disk cache by chooseSizes, once
    // their first use says what size they need.
    auto [it, inserted] = entries_.try_emplace(id);
    if(inserted)
    {
        it->second.id = id;
    }
    return it->second;
}

void TextureManager::use(Entry& e, float texels)
{
    if(e.listedFrame != frame_)
    {
        e.listedFrame = frame_;
        used_.push_back(&e);
    }
    e.usedFrame = frame_;
    e.wanted = std::max(e.wanted, texels >= 0.0f ? texels : 0.0f); // NaN: the least
}

const render::GpuTexture* TextureManager::get(const Uuid& id, float texels)
{
    if(id.isNull())
    {
        return nullptr;
    }
    Entry& e = entry(id);
    e.wantColor = true;
    use(e, texels);
    if(recording_)
    {
        recording_->push_back({&e, texels, false, static_cast<bool>(e.color)});
    }
    return e.color ? &e.color : nullptr;
}

void TextureManager::touch(const TextureUse& u, float texels)
{
    Entry& e = *static_cast<Entry*>(u.entry);
    (u.linear ? e.wantLinear : e.wantColor) = true;
    use(e, texels);
}

const render::GpuTexture* TextureManager::get(const Uuid& id, const std::string& url, float texels)
{
    if(id.isNull())
    {
        return nullptr;
    }
    Entry& e = entry(id);
    if(e.url.empty())
    {
        e.url = url;
    }
    return get(id, texels);
}

const render::GpuTexture* TextureManager::getLinear(const Uuid& id, const std::string& url,
                                                    float texels)
{
    if(id.isNull())
    {
        return nullptr;
    }
    Entry& e = entry(id);
    if(e.url.empty())
    {
        e.url = url;
    }
    return getLinear(id, texels);
}

const render::GpuTexture* TextureManager::getLinear(const Uuid& id, float texels)
{
    if(id.isNull())
    {
        return nullptr;
    }
    Entry& e = entry(id);
    e.wantLinear = true;
    use(e, texels);
    if(recording_)
    {
        recording_->push_back({&e, texels, true, static_cast<bool>(e.linear)});
    }
    return e.linear ? &e.linear : nullptr;
}

const image::TextureData* TextureManager::data(const Uuid& id)
{
    if(id.isNull())
    {
        return nullptr;
    }
    Entry& e = entry(id);
    e.keepCpuCopy = true;
    if(e.listedFrame != frame_)
    {
        e.listedFrame = frame_;
        used_.push_back(&e);
    }
    e.usedFrame = frame_;
    return e.cpuCopy.get();
}

std::shared_ptr<const image::Plane> TextureManager::mask(const Uuid& id)
{
    if(id.isNull())
    {
        return nullptr;
    }
    Entry& e = entry(id);
    e.wantMask = true;
    if(e.listedFrame != frame_)
    {
        e.listedFrame = frame_;
        used_.push_back(&e);
    }
    e.usedFrame = frame_;
    return e.mask;
}

std::shared_ptr<const image::Plane> TextureManager::mask(const Uuid& id, const std::string& url)
{
    if(id.isNull())
    {
        return nullptr;
    }
    Entry& e = entry(id);
    if(e.url.empty())
    {
        e.url = url;
    }
    return mask(id);
}

std::optional<image::AlphaMode> TextureManager::alphaMode(const Uuid& id) const
{
    const auto it = entries_.find(id);
    if(it == entries_.end() || !it->second.color)
    {
        return std::nullopt;
    }
    return it->second.alphaMode;
}

uint32_t TextureManager::largerSide(const render::GpuTexture& t)
{
    return t ? std::max(t.width(), t.height()) : 0;
}

bool TextureManager::busy(const Entry& e) const
{
    return e.reading || e.uploading > 0 || e.state == State::Decoding ||
           !e.pending.levels.empty() || !e.pendingLinear.levels.empty();
}

uint32_t TextureManager::targetSize(const Entry& e) const
{
    uint32_t size = 0; // whole
    if(e.kept < kFullSize)
    {
        size = kMinSize;
        while(static_cast<float>(size) < e.kept && size < kMaxSize)
        {
            size <<= 1;
        }
    }
    else if(bias_ > 0)
    {
        size = e.fullSize != 0 ? e.fullSize : kMaxSize;
    }
    if(size != 0)
    {
        size = std::max(size >> bias_, kMinSize);
        if(e.fullSize != 0 && size >= e.fullSize)
        {
            size = 0;
        }
    }
    return size;
}

void TextureManager::update()
{
    cache_.drainResults(cacheResults_);
    for(auto& result : cacheResults_)
    {
        handleCacheResult(result);
    }

    completedDownloads_.drain(downloads_);
    for(auto& download : downloads_)
    {
        handleDownload(download);
    }

    // Failed downloads come back after a pause (looked for now and then:
    // there are thousands of textures).
    const auto now = std::chrono::steady_clock::now();
    for(auto& [id, e] : entries_)
    {
        if(frame_ % 60 != 0)
        {
            break;
        }
        if(e.state == State::Failed && e.attempts < kMaxAttempts && now >= e.retryAt &&
           !viewerAssetUrl_.empty())
        {
            e.state = State::Queued;
            downloadQueue_.push_back(id);
        }
    }
    startDownloads();

    uploaded_.drain(drainedUploads_);
    for(Uploaded& u : drainedUploads_)
    {
        uploadJobs_--;
        pendingBytes_ -= std::min(pendingBytes_, u.bytes);
        for(size_t i = 0; i < u.ids.size(); i++)
        {
            auto it = entries_.find(u.ids[i]);
            if(it == entries_.end())
            {
                continue;
            }
            Entry& e = it->second;
            e.uploading--;
            if(!u.textures[i])
            {
                continue; // the upload failed: the old copy (if any) stays
            }
            if(u.linear)
            {
                e.linear = std::move(u.textures[i]);
            }
            else
            {
                e.color = std::move(u.textures[i]);
                e.alphaMode = u.alphaModes[i];
            }
        }
    }
    drainedUploads_.clear();

    keepToBudget();
    chooseSizes();
    startReads();
    startUploads();
    frame_++; // this frame's get()s come next
}

void TextureManager::chooseSizes()
{
    // Only what was asked for last frame: the rest keeps its size.
    const auto now = std::chrono::steady_clock::now();
    for(Entry* entry : used_)
    {
        Entry& e = *entry;
        const Uuid& id = e.id;
        if(e.usedFrame == frame_)
        {
            // Grow at once; shrink once the smaller size has done for a while.
            if(e.wanted >= e.kept || now - e.keptSince > kShrinkAfter)
            {
                e.kept = e.wanted;
                e.keptSince = now;
            }
            e.wanted = 0.0f;
        }
        if(frame_ - e.usedFrame >= kIdleFrames || busy(e) ||
           (e.state != State::Lookup && e.state != State::Cached))
        {
            continue;
        }
        const bool needCpuCopy = (e.keepCpuCopy && !e.cpuCopy) || (e.wantMask && !e.maskKnown);
        const uint32_t target = targetSize(e);
        bool read = e.state == State::Lookup || needCpuCopy;
        if(!read && (e.wantColor || e.wantLinear))
        {
            const bool missing = (e.wantColor && !e.color) || (e.wantLinear && !e.linear);
            const uint32_t have = std::max(largerSide(e.color), largerSide(e.linear));
            const uint32_t want = target != 0 ? target : e.fullSize;
            // want 0: the whole image, of a size not known yet.
            read = missing || (want == 0 ? have == 0 : have < want || have >= want * 2);
        }
        if(read)
        {
            e.reading = true;
            e.readSize = needCpuCopy ? 0 : target;
            readQueue_.emplace_back(id, e.readSize);
        }
    }
    used_.clear();
}

void TextureManager::keepToBudget()
{
    // Every so many frames: adding up thousands of textures isn't free.
    if(frame_ % 16 != 0)
    {
        return;
    }
    uint64_t total = 0;
    for(const auto& [id, e] : entries_)
    {
        total += textureBytes(e.color) + textureBytes(e.linear);
    }
    const auto now = std::chrono::steady_clock::now();
    if(total > budget_)
    {
        // Least recently drawn first, of those not drawn lately.
        std::vector<std::pair<uint64_t, Entry*>> idle;
        for(auto& [id, e] : entries_)
        {
            if((e.color || e.linear) && frame_ - e.usedFrame >= kIdleFrames)
            {
                idle.emplace_back(e.usedFrame, &e);
            }
        }
        std::ranges::sort(idle, {}, &std::pair<uint64_t, Entry*>::first);
        const uint64_t goal = budget_ / 10 * 9;
        for(auto& [frame, e] : idle)
        {
            if(total <= goal)
            {
                break;
            }
            total -= textureBytes(e->color) + textureBytes(e->linear);
            e->color.reset();
            e->linear.reset();
            evicted_++;
        }
        // What's in view doesn't fit: everything at half the size.
        if(total > budget_ && bias_ < kMaxBias && now - biasChangedAt_ > std::chrono::seconds(2))
        {
            bias_++;
            biasChangedAt_ = now;
            log::info("textures: {:.0f} MiB in view is over the {:.0f} MiB budget; loading at "
                      "1/{} size",
                      static_cast<double>(total) / (1 << 20),
                      static_cast<double>(budget_) / (1 << 20), 1 << bias_);
        }
    }
    else if(bias_ > 0 && total < budget_ / 2 && now - biasChangedAt_ > std::chrono::seconds(10))
    {
        bias_--;
        biasChangedAt_ = now;
        log::info("textures: within budget again; loading at 1/{} size", 1 << bias_);
    }
    gpuBytes_ = total;
}

void TextureManager::startReads()
{
    while(reading_ < kMaxReads && pendingBytes_ < kMaxPendingBytes && !readQueue_.empty())
    {
        const auto [id, size] = readQueue_.front();
        readQueue_.pop_front();
        const auto it = entries_.find(id);
        if(it == entries_.end() || !it->second.reading)
        {
            continue;
        }
        if(!cache_.request(assets::TextureRequest{
               .id = id, .maxDimension = size, .flipRows = true, .wantMask = it->second.wantMask}))
        {
            it->second.reading = false; // one is in flight already: try again later
            continue;
        }
        reading_++;
    }
}

// The copying into GPU memory runs on a worker: on the main thread it took
// several milliseconds per batch. Textures are usable once the worker has
// submitted their upload (later frames go to the GPU after it).
void TextureManager::startUploads()
{
    constexpr size_t kUploadJobs = 2; // batches in flight, which bounds memory
    while(uploadJobs_ < kUploadJobs && (!uploadQueue_.empty() || !linearUploadQueue_.empty()))
    {
        // Color textures first; linear copies (normal maps and the like) in
        // batches of their own.
        const bool linear = uploadQueue_.empty();
        std::deque<Uuid>& queue = linear ? linearUploadQueue_ : uploadQueue_;
        auto batch = std::make_shared<std::vector<image::TextureData>>();
        Uploaded result;
        result.linear = linear;
        while(!queue.empty() && batch->size() < kUploadsPerBatch &&
              result.bytes < kUploadBytesPerBatch)
        {
            const Uuid id = queue.front();
            queue.pop_front();
            auto it = entries_.find(id);
            if(it == entries_.end())
            {
                continue;
            }
            Entry& e = it->second;
            image::TextureData& pending = linear ? e.pendingLinear : e.pending;
            if(pending.levels.empty())
            {
                continue;
            }
            result.bytes += dataBytes(pending);
            result.ids.push_back(id);
            result.alphaModes.push_back(pending.alphaMode);
            batch->push_back(std::move(pending));
            pending = {};
            e.uploading++;
        }
        if(batch->empty())
        {
            continue;
        }
        uploadJobs_++;
        jobsRunning_++;
        jobs_.submit(
            [this, batch, linear, result = std::make_shared<Uploaded>(std::move(result))]
            {
                std::vector<const image::TextureData*> pointers;
                for(const image::TextureData& t : *batch)
                {
                    pointers.push_back(&t);
                }
                result->textures = renderer_.uploadTextures(pointers, !linear);
                uploaded_.push(std::move(*result));
                jobsRunning_--;
            });
    }
}

void TextureManager::handleCacheResult(assets::TextureResult& result)
{
    auto it = entries_.find(result.id);
    if(it == entries_.end())
    {
        return;
    }
    Entry& e = it->second;
    const bool decoded = e.state == State::Decoding;
    if(!decoded && e.reading)
    {
        e.reading = false;
        reading_ -= std::min<size_t>(reading_, 1);
    }
    switch(result.status)
    {
    case assets::TextureResult::Status::LoadedFromCache:
    case assets::TextureResult::Status::Decoded:
    {
        // Flipped already, on the worker (requested so).
        e.state = State::Cached;
        image::TextureData& texture = result.texture;
        const uint32_t side = dataSide(texture);
        if(const uint32_t full = std::max(texture.fullWidth, texture.fullHeight); full != 0)
        {
            e.fullSize = full; // the whole image's, whatever was decoded
        }
        else if(e.readSize == 0 || side < e.readSize)
        {
            e.fullSize = side; // all of it
        }
        // Smaller than wanted, and the cache has nothing to decode a bigger
        // one from: download it (this one shows meanwhile).
        if(result.needsSource && !viewerAssetUrl_.empty())
        {
            e.state = State::Queued;
            downloadQueue_.push_back(result.id);
        }
        if(e.keepCpuCopy && e.readSize == 0)
        {
            e.cpuCopy = std::make_unique<image::TextureData>(texture);
        }
        if(e.wantMask)
        {
            if(!texture.aux.empty())
            {
                e.mask = std::make_shared<const image::Plane>(std::move(texture.aux));
            }
            e.maskKnown = true;
            // Cached before masks were kept: once more from the source.
            if(result.maskUnknown && !e.maskRefetched && e.state == State::Cached &&
               (!e.url.empty() || !viewerAssetUrl_.empty()))
            {
                e.maskRefetched = true;
                e.state = State::Queued;
                downloadQueue_.push_back(result.id);
            }
        }
        // The GPU copies wanted that aren't this size already.
        const bool color = e.wantColor && largerSide(e.color) != side;
        const bool linear = e.wantLinear && largerSide(e.linear) != side;
        if(linear)
        {
            e.pendingLinear = color ? texture : std::move(texture);
            pendingBytes_ += dataBytes(e.pendingLinear);
            linearUploadQueue_.push_back(result.id);
        }
        if(color)
        {
            e.pending = std::move(texture);
            pendingBytes_ += dataBytes(e.pending);
            uploadQueue_.push_back(result.id);
        }
        return;
    }
    case assets::TextureResult::Status::NotFound:
        if(decoded)
        {
            e.state = State::Failed; // the bytes we downloaded vanished
            return;
        }
        // Not cached (or gone from the cache since): download it.
        e.state = viewerAssetUrl_.empty() ? State::WaitingForUrl : State::Queued;
        if(e.state == State::Queued)
        {
            downloadQueue_.push_back(result.id);
        }
        return;
    case assets::TextureResult::Status::Failed:
        log::warn("texture {} couldn't be decoded", result.id.toString());
        e.state = State::Failed;
        e.attempts = kMaxAttempts; // re-downloading won't fix bad data
        return;
    }
}

void TextureManager::startDownloads()
{
    while(downloading_ < kMaxDownloads && !downloadQueue_.empty() && !viewerAssetUrl_.empty())
    {
        const Uuid id = downloadQueue_.front();
        downloadQueue_.pop_front();
        auto it = entries_.find(id);
        if(it == entries_.end() || it->second.state != State::Queued)
        {
            continue;
        }
        it->second.state = State::Downloading;
        it->second.attempts++;
        downloading_++;

        // As the official viewer: <ViewerAsset>/?texture_id=<uuid>.
        net::HttpRequest request{
            .url = it->second.url.empty()
                       ? std::format("{}/?texture_id={}", viewerAssetUrl_, id.toString())
                       : it->second.url};
        request.headers.add("Accept", "image/x-j2c");
        request.timeout = std::chrono::seconds(60);
        requests_->send(std::move(request), [this, id](net::HttpResponse&& response)
                        { completedDownloads_.push({id, std::move(response)}); });
    }
}

void TextureManager::handleDownload(Download& download)
{
    downloading_--;
    auto it = entries_.find(download.id);
    if(it == entries_.end() || it->second.state != State::Downloading)
    {
        return;
    }
    Entry& e = it->second;
    const net::HttpResponse& response = download.response;
    if(assetMissing(response))
    {
        e.state = State::Missing;
        return;
    }
    if(!response.ok() || response.body.empty())
    {
        log::warn("texture {} download failed: {}", download.id.toString(),
                  response.error != net::HttpError::None
                      ? std::string(net::errorName(response.error))
                      : std::format("HTTP {}", response.status));
        e.state = State::Failed;
        e.retryAt = std::chrono::steady_clock::now() + std::chrono::seconds(5 * e.attempts);
        return;
    }

    bytesDownloaded_ += response.body.size();
    e.state = State::Decoding;
    // The cache decodes on a worker, at the size wanted now (keeping the
    // J2K if that's below full resolution), writes the KTX2 and reports back.
    e.readSize = e.keepCpuCopy ? 0 : targetSize(e);
    auto bytes = std::make_shared<std::vector<uint8_t>>(std::move(download.response.body));
    cache_.request(assets::TextureRequest{.id = download.id,
                                          .source = [bytes]() -> std::optional<std::vector<uint8_t>>
                                          { return std::move(*bytes); },
                                          .maxDimension = e.readSize,
                                          .flipRows = true,
                                          .wantMask = e.wantMask});
}

TextureManager::Stats TextureManager::stats() const
{
    Stats s;
    for(const auto& [id, e] : entries_)
    {
        s.cpuBytes += dataBytes(e.pending) + dataBytes(e.pendingLinear) +
                      (e.cpuCopy ? dataBytes(*e.cpuCopy) : 0);
        s.resident += (e.color || e.linear) ? 1 : 0;
        if(busy(e) || e.state == State::Lookup)
        {
            s.loading++;
        }
        switch(e.state)
        {
        case State::Downloading:
            s.downloading++;
            break;
        case State::Queued:
        case State::WaitingForUrl:
            s.queued++;
            break;
        case State::Missing:
        case State::Failed:
            s.failed++;
            break;
        default:
            break;
        }
    }
    s.gpuBytes = gpuBytes_;
    s.evicted = evicted_;
    s.bytesDownloaded = bytesDownloaded_;
    s.budget = budget_;
    s.bias = bias_;
    return s;
}

} // namespace viewer
