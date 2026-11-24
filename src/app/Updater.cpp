#include "Updater.hpp"

#include "viewer/core/Log.hpp"

#if defined(VIEWER_VELOPACK)
// Velopack.hpp needs it and, on Windows, doesn't include it.
#include <string.h>

#include <Velopack.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// After windows.h.
#include <tlhelp32.h>

#include <cwchar>
#endif

namespace viewer
{

std::string updateFolder(std::string_view baseUrl, std::string_view packageId)
{
    auto hostIs = [&](std::string_view start)
    {
        return baseUrl.starts_with(start) &&
               (baseUrl.size() == start.size() || baseUrl[start.size()] == ':' ||
                baseUrl[start.size()] == '/') &&
               // http://localhost:password@elsewhere/
               baseUrl.find('@') == std::string_view::npos;
    };
    if(packageId.empty() || !(baseUrl.starts_with("https://") || hostIs("http://localhost") ||
                              hostIs("http://127.0.0.1")))
    {
        return {};
    }
    std::string folder(baseUrl);
    if(!folder.ends_with('/'))
    {
        folder += '/';
    }
    return folder + "updates/" + std::string(packageId) + "/";
}

#if defined(_WIN32)

namespace
{

std::wstring programOf(HANDLE process)
{
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    path.resize(QueryFullProcessImageNameW(process, 0, path.data(), &size) ? size : 0);
    return path;
}

} // namespace

bool anotherInstanceRunning()
{
    const std::wstring ours = programOf(GetCurrentProcess());
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if(ours.empty() || snapshot == INVALID_HANDLE_VALUE)
    {
        return true; // can't tell: the update waits
    }
    bool found = false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for(BOOL more = Process32FirstW(snapshot, &entry); more && !found;
        more = Process32NextW(snapshot, &entry))
    {
        if(entry.th32ProcessID == GetCurrentProcessId())
        {
            continue;
        }
        if(const HANDLE process =
               OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID))
        {
            found = _wcsicmp(programOf(process).c_str(), ours.c_str()) == 0;
            CloseHandle(process);
        }
    }
    CloseHandle(snapshot);
    return found;
}

#else

bool anotherInstanceRunning()
{
    return false;
}

#endif

#if !defined(VIEWER_VELOPACK)

std::unique_ptr<Updater> Updater::create(net::HttpClient&, Options)
{
    return nullptr;
}

#else

namespace
{

constexpr auto kCheckEvery = std::chrono::hours(6);
constexpr auto kRetry = std::chrono::minutes(15);

// Velopack's packages, through our HttpClient. Called on the updater's
// thread; nothing may be thrown through Velopack (it's Rust).
class Source final : public Velopack::IUpdateSource
{
public:
    Source(net::HttpClient& http, std::string userAgent, std::stop_token stop)
        : group_(http), userAgent_(std::move(userAgent)), stop_(std::move(stop))
    {
    }

    std::string folder; // set before it's used

    const std::string GetReleaseFeed(const std::string releasesName) override
    {
        try
        {
            net::HttpRequest request;
            request.url = folder + releasesName;
            request.headers.add("Accept", "application/json");
            const net::HttpResponse response = fetch(std::move(request));
            return response.ok() ? std::string(response.bodyText()) : std::string();
        }
        catch(const std::exception& e)
        {
            log::warn("updates: {}", e.what());
            return {}; // not JSON: the check fails
        }
    }

    bool DownloadReleaseEntry(const Velopack::VelopackAsset& asset,
                              const std::string localFilePath,
                              Velopack::vpkc_progress_send_t progress) override
    {
        try
        {
            return download(asset, localFilePath, progress);
        }
        catch(const std::exception& e)
        {
            log::warn("updates: {}", e.what());
            return false;
        }
    }

private:
    bool download(const Velopack::VelopackAsset& asset, const std::string& to,
                  const Velopack::vpkc_progress_send_t& progress)
    {
        if(asset.FileName.find_first_of("/\\?#%") != std::string::npos)
        {
            log::warn("updates: not a package's name: {}", asset.FileName);
            return false;
        }
        const auto close = [](std::FILE* f) { std::fclose(f); };
        // Velopack's paths are UTF-8.
        const std::filesystem::path path(std::u8string(to.begin(), to.end()));
#if defined(_WIN32)
        std::unique_ptr<std::FILE, decltype(close)> file(_wfopen(path.c_str(), L"wb"), close);
#else
        std::unique_ptr<std::FILE, decltype(close)> file(std::fopen(path.c_str(), "wb"), close);
#endif
        if(!file)
        {
            log::warn("updates: {} couldn't be written", to);
            return false;
        }
        // The I/O thread's, until fetch() returns.
        uint64_t got = 0;
        bool written = true;
        std::atomic<int> percent = 0;

        net::HttpRequest request;
        request.url = folder + asset.FileName;
        request.timeout = std::chrono::milliseconds(0); // however long it takes
        request.stallTimeout = std::chrono::seconds(60);
        request.pool = net::HttpPool::Stream;
        request.onData = [&](int status, const net::HttpHeaders&, std::span<const uint8_t> piece)
        {
            if(status < 200 || status >= 300)
            {
                return;
            }
            got += piece.size();
            // Never more than was announced.
            if(got > asset.Size)
            {
                written = false;
            }
            written = written &&
                      std::fwrite(piece.data(), 1, piece.size(), file.get()) == piece.size();
            percent = asset.Size ? static_cast<int>(std::min<uint64_t>(got * 100 / asset.Size, 100))
                                 : 0;
        };
        const net::HttpResponse response =
            fetch(std::move(request),
                  [&] { progress(static_cast<int16_t>(percent.load())); });
        written = std::fclose(file.release()) == 0 && written;
        if(!response.ok())
        {
            return false;
        }
        if(!written || got != asset.Size)
        {
            log::warn("updates: {}: {} of {} bytes", asset.FileName, got, asset.Size);
            return false;
        }
        return true;
    }

    // Waits for the answer (or to be stopped: then it's a failure). The
    // callbacks run on the client's I/O thread and are over, or cancelled,
    // when this returns.
    net::HttpResponse fetch(net::HttpRequest request, const std::function<void()>& tick = {})
    {
        struct Wait
        {
            std::mutex mutex;
            std::condition_variable_any changed;
            std::optional<net::HttpResponse> response;
        };
        const auto wait = std::make_shared<Wait>();
        const std::string url = request.url;
        request.headers.add("User-Agent", userAgent_);
        request.followRedirects = false; // only from the address checked
        const net::HttpRequestId id = group_.send(std::move(request),
                                                  [wait](net::HttpResponse&& response)
                                                  {
                                                      const std::scoped_lock lock(wait->mutex);
                                                      wait->response = std::move(response);
                                                      wait->changed.notify_all();
                                                  });
        std::unique_lock lock(wait->mutex);
        while(!wait->response && !stop_.stop_requested())
        {
            wait->changed.wait_for(lock, stop_, std::chrono::milliseconds(250),
                                   [&] { return wait->response.has_value(); });
            if(tick)
            {
                lock.unlock();
                tick();
                lock.lock();
            }
        }
        if(!wait->response)
        {
            lock.unlock();
            group_.cancel(id);
            net::HttpResponse stopped;
            stopped.error = net::HttpError::Transport;
            return stopped;
        }
        if(!wait->response->ok())
        {
            log::warn("updates: {}: {}", url,
                      wait->response->error != net::HttpError::None
                          ? std::string(net::errorName(wait->response->error))
                          : std::format("answered {}", wait->response->status));
        }
        return std::move(*wait->response);
    }

    net::HttpRequestGroup group_;
    std::string userAgent_;
    std::stop_token stop_;
};

class VelopackUpdater final : public Updater
{
public:
    VelopackUpdater(net::HttpClient& http, Options options) : arguments_(options.restartArguments)
    {
        auto source =
            std::make_unique<Source>(http, options.userAgent, stop_.get_token());
        Source* s = source.get();
        manager_ = std::make_unique<Velopack::UpdateManager>(std::move(source)); // may throw
        std::string id = manager_->GetAppId();
        id.resize(std::min(id.size(), id.find('\0'))); // it counts the C string's end
        s->folder = updateFolder(options.baseUrl, id);
        if(s->folder.empty())
        {
            throw std::runtime_error("only from an https address");
        }
        // Downloaded before, and not yet put in place (another was running).
        if(auto pending = manager_->UpdatePendingRestart())
        {
            ready(*pending);
        }
        thread_ = std::jthread([this] { run(); });
    }

    ~VelopackUpdater() override
    {
        stop_.request_stop();
        if(thread_.joinable())
        {
            thread_.join(); // before the manager and its source go
        }
    }

    Status status() const override
    {
        const std::scoped_lock lock(mutex_);
        Status s = status_;
        s.progress = progress_;
        return s;
    }

    bool restart() override
    {
        if(anotherInstanceRunning())
        {
            log::warn("updates: not while another viewer is running");
            return false;
        }
        std::optional<Velopack::VelopackAsset> asset;
        {
            const std::scoped_lock lock(mutex_);
            asset = asset_;
        }
        if(!asset)
        {
            return false;
        }
        try
        {
            manager_->WaitExitThenApplyUpdates(*asset, false, true, arguments_);
            return true;
        }
        catch(const std::exception& e)
        {
            log::error("updates: {}", e.what());
            return false;
        }
    }

private:
    void ready(const Velopack::VelopackAsset& asset)
    {
        const std::scoped_lock lock(mutex_);
        asset_ = asset;
        status_ = {Status::Stage::Ready, asset.Version, 100};
    }

    void run()
    {
        const std::stop_token stop = stop_.get_token();
        while(!stop.stop_requested())
        {
            const bool good = check();
            std::unique_lock lock(mutex_);
            wake_.wait_for(lock, stop, good ? std::chrono::minutes(kCheckEvery) : kRetry,
                           [] { return false; });
        }
    }

    // False: to be tried again soon.
    bool check()
    {
        try
        {
            const auto update = manager_->CheckForUpdates();
            if(!update || stop_.stop_requested())
            {
                return true;
            }
            const std::string& version = update->TargetFullRelease.Version;
            {
                const std::scoped_lock lock(mutex_);
                if(asset_ && asset_->Version == version)
                {
                    return true; // have it
                }
                progress_ = 0;
                if(!asset_) // one that's ready stays offered meanwhile
                {
                    status_ = {Status::Stage::Downloading, version, 0};
                }
            }
            log::info("updates: downloading {}", version);
            manager_->DownloadUpdates(
                *update, [](void* self, size_t percent)
                { static_cast<VelopackUpdater*>(self)->progress_ = static_cast<int>(percent); },
                this);
            log::info("updates: {} is ready", version);
            ready(update->TargetFullRelease);
            return true;
        }
        catch(const std::exception& e)
        {
            if(!stop_.stop_requested())
            {
                log::warn("updates: {}", e.what());
            }
            const std::scoped_lock lock(mutex_);
            if(!asset_)
            {
                status_ = {};
            }
            return false;
        }
    }

    std::vector<std::string> arguments_;
    std::stop_source stop_;
    std::unique_ptr<Velopack::UpdateManager> manager_;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    Status status_;
    std::optional<Velopack::VelopackAsset> asset_; // what's ready
    std::atomic<int> progress_ = 0;
    std::jthread thread_;
};

} // namespace

std::unique_ptr<Updater> Updater::create(net::HttpClient& http, Options options)
{
    if(options.baseUrl.empty())
    {
        return nullptr;
    }
    try
    {
        return std::make_unique<VelopackUpdater>(http, std::move(options));
    }
    catch(const std::exception& e)
    {
        log::info("no updates: {}", e.what());
        return nullptr;
    }
}

#endif

} // namespace viewer
