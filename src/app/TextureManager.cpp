#include "TextureManager.hpp"

#include "AssetResponse.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/image/TextureProcessing.hpp"

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

TextureManager::Entry& TextureManager::entry(const Uuid& id)
{
    auto [it, inserted] = entries_.try_emplace(id);
    if(inserted)
    {
        // Cache first; a miss comes back as NotFound and moves on to download.
        cache_.request(assets::TextureRequest{.id = id, .flipRows = true});
    }
    return it->second;
}

const render::GpuTexture* TextureManager::get(const Uuid& id)
{
    if(id.isNull())
    {
        return nullptr;
    }
    Entry& e = entry(id);
    return e.state == State::Resident ? &e.texture : nullptr;
}

const render::GpuTexture* TextureManager::get(const Uuid& id, const std::string& url)
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
    return e.state == State::Resident ? &e.texture : nullptr;
}

const image::TextureData* TextureManager::data(const Uuid& id)
{
    if(id.isNull())
    {
        return nullptr;
    }
    Entry& e = entry(id);
    if(!e.keepCpuCopy)
    {
        e.keepCpuCopy = true;
        if(e.state == State::Resident && !e.cpuCopy)
        {
            // Already uploaded without a copy: read it back from the cache.
            e.state = State::CacheLookup;
            cache_.request(assets::TextureRequest{.id = id, .flipRows = true});
        }
    }
    return e.cpuCopy.get();
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

    // Failed downloads come back after a pause.
    const auto now = std::chrono::steady_clock::now();
    for(auto& [id, e] : entries_)
    {
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
        for(size_t i = 0; i < u.ids.size(); i++)
        {
            auto it = entries_.find(u.ids[i]);
            if(it == entries_.end() || it->second.state != State::Uploading)
            {
                continue;
            }
            Entry& e = it->second;
            e.texture = std::move(u.textures[i]);
            e.alphaMode = u.alphaModes[i];
            e.state = e.texture ? State::Resident : State::Failed;
        }
    }
    drainedUploads_.clear();
    startUploads();
}

// The copying into GPU memory runs on a worker: on the main thread it took
// several milliseconds per batch. Textures are usable once the worker has
// submitted their upload (later frames go to the GPU after it).
void TextureManager::startUploads()
{
    constexpr size_t kUploadJobs = 2; // batches in flight, which bounds memory
    while(uploadJobs_ < kUploadJobs && !uploadQueue_.empty())
    {
        auto batch = std::make_shared<std::vector<image::TextureData>>();
        Uploaded result;
        uint64_t bytes = 0;
        while(!uploadQueue_.empty() && batch->size() < kUploadsPerBatch &&
              bytes < kUploadBytesPerBatch)
        {
            const Uuid id = uploadQueue_.front();
            uploadQueue_.pop_front();
            auto it = entries_.find(id);
            if(it == entries_.end() || it->second.state != State::Uploading ||
               it->second.pending.levels.empty())
            {
                continue;
            }
            for(const auto& level : it->second.pending.levels)
            {
                bytes += level.byteSize();
            }
            result.ids.push_back(id);
            result.alphaModes.push_back(it->second.pending.alphaMode);
            batch->push_back(std::move(it->second.pending));
            it->second.pending = {};
        }
        if(batch->empty())
        {
            return;
        }
        uploadJobs_++;
        jobsRunning_++;
        jobs_.submit(
            [this, batch, result = std::make_shared<Uploaded>(std::move(result))]
            {
                std::vector<const image::TextureData*> pointers;
                for(const image::TextureData& t : *batch)
                {
                    pointers.push_back(&t);
                }
                result->textures = renderer_.uploadTextures(pointers, true);
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
    switch(result.status)
    {
    case assets::TextureResult::Status::LoadedFromCache:
    case assets::TextureResult::Status::Decoded:
        // Flipped already, on the worker (requested so).
        if(e.keepCpuCopy)
        {
            e.cpuCopy = std::make_unique<image::TextureData>(result.texture);
        }
        if(e.texture)
        {
            e.state = State::Resident; // was only re-read for the CPU copy
            return;
        }
        e.pending = std::move(result.texture);
        e.state = State::Uploading;
        uploadQueue_.push_back(result.id);
        return;
    case assets::TextureResult::Status::NotFound:
        if(e.state == State::Decoding)
        {
            e.state = State::Failed; // the bytes we downloaded vanished
            return;
        }
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
    // The cache decodes on a worker, writes the KTX2 and reports back.
    auto bytes = std::make_shared<std::vector<uint8_t>>(std::move(download.response.body));
    cache_.request(assets::TextureRequest{.id = download.id,
                                          .source = [bytes]() -> std::optional<std::vector<uint8_t>>
                                          { return std::move(*bytes); },
                                          .flipRows = true});
}

std::optional<image::AlphaMode> TextureManager::alphaMode(const Uuid& id) const
{
    const auto it = entries_.find(id);
    if(it == entries_.end() || it->second.state != State::Resident)
    {
        return std::nullopt;
    }
    return it->second.alphaMode;
}

TextureManager::Stats TextureManager::stats() const
{
    Stats s;
    for(const auto& [id, e] : entries_)
    {
        switch(e.state)
        {
        case State::Resident:
            s.resident++;
            break;
        case State::CacheLookup:
        case State::Decoding:
        case State::Uploading:
            s.loading++;
            break;
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
        }
    }
    s.bytesDownloaded = bytesDownloaded_;
    return s;
}

} // namespace viewer
