#include "SoundAssets.hpp"

#include "AssetResponse.hpp"

#include "viewer/core/Log.hpp"

#include <format>
#include <thread>

namespace viewer
{

namespace
{

constexpr size_t kMaxDownloads = 4;
// Decoded clips not asked for in this long are let go.
constexpr auto kKeepUnused = std::chrono::minutes(5);

} // namespace

SoundAssets::SoundAssets(net::HttpClient& http, JobSystem& jobs, DiskCache& store)
    : http_(http), jobs_(jobs), store_(store),
      requests_(std::make_unique<net::HttpRequestGroup>(http))
{
}

SoundAssets::~SoundAssets()
{
    requests_.reset();
    while(jobsRunning_.load() > 0)
    {
        std::this_thread::yield();
    }
}

void SoundAssets::submit(std::function<void()> job)
{
    jobsRunning_++;
    jobs_.submit(
        [this, job = std::move(job)]
        {
            job();
            jobsRunning_--;
        });
}

void SoundAssets::setViewerAssetUrl(std::string url)
{
    if(url == viewerAssetUrl_)
    {
        return;
    }
    viewerAssetUrl_ = std::move(url);
    // Downloads in flight belong to the old URL: drop them and queue again.
    requests_ = std::make_unique<net::HttpRequestGroup>(http_);
    downloading_ = 0;
    std::vector<Download> stale;
    downloads_.drain(stale);
    for(auto& [id, e] : entries_)
    {
        if(e.state == State::Downloading)
        {
            e.state = State::Queued;
            queue_.push_back(id);
        }
    }
}

std::shared_ptr<const audio::SoundClip> SoundAssets::get(const Uuid& id)
{
    if(id.isNull())
    {
        return nullptr;
    }
    auto [it, inserted] = entries_.try_emplace(id);
    it->second.used = Clock::now();
    if(inserted || (it->second.state == State::Ready && !it->second.clip))
    {
        // Disk cache first.
        it->second.state = State::Loading;
        submit(
            [this, id, results = results_]
            {
                auto bytes = store_.read({CacheKind::Sound, id});
                if(!bytes)
                {
                    results->push({id, false, nullptr});
                    return;
                }
                decode(id, std::move(*bytes), false);
            });
    }
    return it->second.clip;
}

bool SoundAssets::failed(const Uuid& id) const
{
    const auto it = entries_.find(id);
    return it != entries_.end() && it->second.state == State::Failed;
}

// On a worker: decode and (for downloads) save.
void SoundAssets::decode(const Uuid& id, std::vector<uint8_t> bytes, bool save)
{
    Result result{id, true, nullptr};
    if(auto clip = audio::decodeVorbis(bytes))
    {
        result.clip = std::make_shared<const audio::SoundClip>(std::move(*clip));
        if(save)
        {
            store_.write({CacheKind::Sound, id}, bytes);
        }
    }
    results_->push(std::move(result));
}

void SoundAssets::startDownloads()
{
    while(downloading_ < kMaxDownloads && !queue_.empty() && !viewerAssetUrl_.empty())
    {
        const Uuid id = queue_.front();
        queue_.pop_front();
        auto it = entries_.find(id);
        if(it == entries_.end() || it->second.state != State::Queued)
        {
            continue;
        }
        it->second.state = State::Downloading;
        net::HttpRequest http{.url =
                                  std::format("{}/?sound_id={}", viewerAssetUrl_, id.toString())};
        http.timeout = std::chrono::seconds(60);
        downloading_++;
        requests_->send(std::move(http), [this, id](net::HttpResponse&& response)
                        { downloads_.push({id, std::move(response)}); });
    }
}

void SoundAssets::update()
{
    downloads_.drain(drainedDownloads_);
    for(Download& d : drainedDownloads_)
    {
        downloading_--;
        auto it = entries_.find(d.id);
        if(it == entries_.end() || it->second.state != State::Downloading)
        {
            continue;
        }
        if(!d.response.ok() || d.response.body.empty())
        {
            if(!assetMissing(d.response))
            {
                log::warn("sound {} download failed: {}", d.id.toString(),
                          d.response.error != net::HttpError::None
                              ? std::string(net::errorName(d.response.error))
                              : std::format("HTTP {}", d.response.status));
            }
            it->second.state = State::Failed;
            continue;
        }
        it->second.state = State::Loading;
        submit([this, id = d.id, bytes = std::move(d.response.body)]() mutable
               { decode(id, std::move(bytes), true); });
    }
    drainedDownloads_.clear();

    results_->drain(drainedResults_);
    for(Result& r : drainedResults_)
    {
        auto it = entries_.find(r.id);
        if(it == entries_.end() || it->second.state != State::Loading)
        {
            continue;
        }
        if(!r.onDisk)
        {
            it->second.state = State::Queued;
            queue_.push_back(r.id);
        }
        else if(r.clip)
        {
            it->second.clip = std::move(r.clip);
            it->second.state = State::Ready;
        }
        else
        {
            log::warn("sound {} isn't a sound that can be played", r.id.toString());
            it->second.state = State::Failed;
        }
    }
    drainedResults_.clear();

    // Clips not asked for lately: let go (read again if asked).
    const auto now = Clock::now();
    if(now - lastSweep_ > std::chrono::seconds(10))
    {
        lastSweep_ = now;
        for(auto& [id, e] : entries_)
        {
            if(e.state == State::Ready && e.clip && now - e.used > kKeepUnused)
            {
                e.clip.reset();
            }
        }
    }

    startDownloads();
}

SoundAssets::Stats SoundAssets::stats() const
{
    Stats s;
    for(const auto& [id, e] : entries_)
    {
        s.ready += e.state == State::Ready && e.clip;
        s.failed += e.state == State::Failed;
        s.loading += e.state != State::Ready && e.state != State::Failed;
        s.bytes += e.clip ? e.clip->byteSize() : 0;
    }
    return s;
}

void SoundAssets::clear()
{
    requests_ = std::make_unique<net::HttpRequestGroup>(http_);
    downloading_ = 0;
    std::vector<Download> stale;
    downloads_.drain(stale);
    entries_.clear();
    queue_.clear();
}

} // namespace viewer
