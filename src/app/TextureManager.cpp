#include "TextureManager.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/image/TextureProcessing.hpp"

#include <format>

namespace viewer
{

namespace
{

constexpr size_t kMaxDownloads = 16;
constexpr int kMaxAttempts = 3;
constexpr size_t kUploadsPerFrame = 16;
constexpr uint64_t kUploadBytesPerFrame = 32ull << 20;

} // namespace

TextureManager::TextureManager(assets::TextureCache& cache, render::Renderer& renderer,
                               net::HttpClient& http)
    : cache_(cache), renderer_(renderer), http_(http),
      requests_(std::make_unique<net::HttpRequestGroup>(http))
{
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
        cache_.request(assets::TextureRequest{id});
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
            cache_.request(assets::TextureRequest{id});
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

    // GPU uploads, bounded per frame.
    std::vector<Uuid> ids;
    std::vector<const image::TextureData*> batch;
    uint64_t bytes = 0;
    while(!uploadQueue_.empty() && batch.size() < kUploadsPerFrame && bytes < kUploadBytesPerFrame)
    {
        const Uuid id = uploadQueue_.front();
        uploadQueue_.pop_front();
        auto it = entries_.find(id);
        if(it == entries_.end() || it->second.state != State::Uploading)
        {
            continue;
        }
        for(const auto& level : it->second.pending.levels)
        {
            bytes += level.byteSize();
        }
        ids.push_back(id);
        batch.push_back(&it->second.pending);
    }
    if(batch.empty())
    {
        return;
    }
    auto textures = renderer_.uploadTextures(batch, true);
    for(size_t i = 0; i < ids.size(); i++)
    {
        Entry& e = entries_[ids[i]];
        e.texture = std::move(textures[i]);
        e.pending = {};
        e.state = e.texture ? State::Resident : State::Failed;
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
        image::flipRows(result.texture);
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
            .url = std::format("{}/?texture_id={}", viewerAssetUrl_, id.toString())};
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
    if(response.error == net::HttpError::None && response.status == 404)
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
    cache_.request(assets::TextureRequest{download.id,
                                          [bytes]() -> std::optional<std::vector<uint8_t>>
                                          { return std::move(*bytes); }});
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
