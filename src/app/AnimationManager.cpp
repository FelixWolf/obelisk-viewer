#include "AnimationManager.hpp"

#include "AssetResponse.hpp"

#include "viewer/core/Log.hpp"

#include <format>
#include <thread>

namespace viewer
{

namespace
{

constexpr size_t kMaxDownloads = 4;

} // namespace

AnimationManager::AnimationManager(net::HttpClient& http, JobSystem& jobs,
                                   avatar::Skeleton skeleton, DiskCache& store)
    : http_(http), jobs_(jobs), skeleton_(std::move(skeleton)), store_(store),
      requests_(std::make_unique<net::HttpRequestGroup>(http))
{
}

AnimationManager::~AnimationManager()
{
    requests_.reset();
    while(jobsRunning_.load() > 0)
    {
        std::this_thread::yield();
    }
}

void AnimationManager::submit(std::function<void()> job)
{
    jobsRunning_++;
    jobs_.submit(
        [this, job = std::move(job)]
        {
            job();
            jobsRunning_--;
        });
}

void AnimationManager::setViewerAssetUrl(std::string url)
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

std::shared_ptr<const avatar::Animation> AnimationManager::get(const Uuid& id)
{
    if(id.isNull())
    {
        return nullptr;
    }
    auto [it, inserted] = entries_.try_emplace(id);
    if(inserted)
    {
        // Disk cache first.
        submit(
            [this, id]
            {
                auto bytes = store_.read({CacheKind::Animation, id});
                if(!bytes)
                {
                    results_.push({id, false, nullptr, {}});
                    return;
                }
                parse(id, std::move(*bytes), false);
            });
    }
    return it->second.animation;
}

// On a worker: parse, bind to the skeleton and (for downloads) save.
void AnimationManager::parse(const Uuid& id, std::vector<uint8_t> bytes, bool save)
{
    Result result{id, true, nullptr, {}};
    auto animation = avatar::parseAnimation(bytes);
    if(animation)
    {
        animation->bind(skeleton_);
        result.animation = std::make_shared<const avatar::Animation>(std::move(*animation));
        if(save)
        {
            store_.write({CacheKind::Animation, id}, bytes);
        }
    }
    else
    {
        result.error = animation.error();
    }
    results_.push(std::move(result));
}

void AnimationManager::startDownloads()
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
                                  std::format("{}/?animatn_id={}", viewerAssetUrl_, id.toString())};
        http.timeout = std::chrono::seconds(60);
        downloading_++;
        requests_->send(std::move(http), [this, id](net::HttpResponse&& response)
                        { downloads_.push({id, std::move(response)}); });
    }
}

void AnimationManager::update()
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
                log::warn("animation {} download failed: {}", d.id.toString(),
                          d.response.error != net::HttpError::None
                              ? std::string(net::errorName(d.response.error))
                              : std::format("HTTP {}", d.response.status));
            }
            it->second.state = State::Failed; // missing or failed: not asked again
            continue;
        }
        it->second.state = State::Loading;
        submit([this, id = d.id, bytes = std::move(d.response.body)]() mutable
               { parse(id, std::move(bytes), true); });
    }
    drainedDownloads_.clear();

    results_.drain(drainedResults_);
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
        else if(r.animation)
        {
            it->second.animation = std::move(r.animation);
            it->second.state = State::Ready;
        }
        else
        {
            log::warn("animation {}: {}", r.id.toString(), r.error);
            it->second.state = State::Failed;
        }
    }
    drainedResults_.clear();

    startDownloads();
}

AnimationManager::Stats AnimationManager::stats() const
{
    Stats s;
    for(const auto& [id, e] : entries_)
    {
        s.ready += e.state == State::Ready;
        s.failed += e.state == State::Failed;
        s.loading += e.state != State::Ready && e.state != State::Failed;
    }
    return s;
}

} // namespace viewer
