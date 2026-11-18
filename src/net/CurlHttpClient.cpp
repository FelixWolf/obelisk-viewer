// libcurl backend for HttpClient: one I/O thread driving a curl multi handle.
// Requests wait here, not in curl, until their pool (HttpPool) has a
// connection free on their host: curl's own per-host queue is one line for
// everything.
// This file defines createHttpClient(); swap it to change backend.

#include "viewer/core/Log.hpp"
#include "viewer/net/HttpClient.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace viewer::net
{

namespace
{

struct Transfer
{
    HttpRequestId id = 0;
    HttpRequest request;
    HttpCallback callback;
    HttpResponse response;
    std::string slot; // its pool and host, whose connections it takes one of
    CURL* easy = nullptr;
    curl_slist* headerList = nullptr;
    char errorBuffer[CURL_ERROR_SIZE] = {};

    ~Transfer()
    {
        if(easy)
        {
            curl_easy_cleanup(easy);
        }
        curl_slist_free_all(headerList);
    }
};

size_t onBody(char* data, size_t size, size_t count, void* user)
{
    auto* transfer = static_cast<Transfer*>(user);
    const size_t bytes = size * count;
    transfer->response.body.insert(transfer->response.body.end(), data, data + bytes);
    return bytes;
}

size_t onHeader(char* data, size_t size, size_t count, void* user)
{
    auto* transfer = static_cast<Transfer*>(user);
    const size_t bytes = size * count;
    std::string_view line(data, bytes);

    // A new status line starts a new response (redirects, 100 Continue):
    // only the final response's headers are kept.
    if(line.starts_with("HTTP/"))
    {
        transfer->response.headers.clear();
        return bytes;
    }
    const size_t colon = line.find(':');
    if(colon == std::string_view::npos)
    {
        return bytes;
    }
    std::string name(line.substr(0, colon));
    std::ranges::transform(name, name.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string_view value = line.substr(colon + 1);
    while(!value.empty() && (value.front() == ' ' || value.front() == '\t'))
    {
        value.remove_prefix(1);
    }
    while(!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' '))
    {
        value.remove_suffix(1);
    }
    transfer->response.headers.add(std::move(name), std::string(value));
    return bytes;
}

HttpError mapError(CURLcode code)
{
    switch(code)
    {
    case CURLE_OK:
        return HttpError::None;
    case CURLE_OPERATION_TIMEDOUT:
        return HttpError::Timeout;
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
        return HttpError::CouldNotResolve;
    case CURLE_COULDNT_CONNECT:
        return HttpError::CouldNotConnect;
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CIPHER:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_ISSUER_ERROR:
    case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
    case CURLE_SSL_INVALIDCERTSTATUS:
    case CURLE_SSL_ENGINE_INITFAILED:
    case CURLE_SSL_ENGINE_NOTFOUND:
    case CURLE_SSL_ENGINE_SETFAILED:
    case CURLE_SSL_CRL_BADFILE:
    case CURLE_SSL_SHUTDOWN_FAILED:
        return HttpError::Tls;
    case CURLE_URL_MALFORMAT:
    case CURLE_UNSUPPORTED_PROTOCOL:
        return HttpError::InvalidRequest;
    default:
        return HttpError::Transport;
    }
}

class CurlHttpClient final : public HttpClient
{
public:
    explicit CurlHttpClient(const HttpClientOptions& options) : options_(options)
    {
        // Reference-counted by libcurl, so several clients may coexist.
        curl_global_init(CURL_GLOBAL_DEFAULT);
        multi_ = curl_multi_init();
        // Per host: by pool, here (waiting).
        curl_multi_setopt(multi_, CURLMOPT_MAX_HOST_CONNECTIONS, 0L);
        curl_multi_setopt(multi_, CURLMOPT_MAX_TOTAL_CONNECTIONS, options_.maxTotalConnections);
        thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
    }

    ~CurlHttpClient() override
    {
        {
            std::lock_guard lock(callbackMutex_);
            active_.clear(); // no callback may run from here on
        }
        thread_.request_stop();
        curl_multi_wakeup(multi_);
        thread_.join();
        curl_multi_cleanup(multi_);
        curl_global_cleanup();
    }

    HttpRequestId send(HttpRequest request, HttpCallback callback) override
    {
        auto transfer = std::make_unique<Transfer>();
        transfer->id = nextId_++;
        transfer->request = std::move(request);
        transfer->callback = std::move(callback);
        const HttpRequestId id = transfer->id;
        {
            std::lock_guard lock(callbackMutex_);
            active_.insert(id);
        }
        {
            std::lock_guard lock(submitMutex_);
            submitted_.push_back(std::move(transfer));
        }
        curl_multi_wakeup(multi_);
        return id;
    }

    bool cancel(HttpRequestId id) override
    {
        bool wasActive;
        {
            // Waits for a callback that is currently running (the I/O thread
            // holds this lock while calling it).
            std::lock_guard lock(callbackMutex_);
            wasActive = active_.erase(id) > 0;
        }
        if(wasActive)
        {
            std::lock_guard lock(submitMutex_);
            cancelled_.push_back(id);
        }
        curl_multi_wakeup(multi_);
        return wasActive;
    }

private:
    void run(std::stop_token stop)
    {
        std::vector<std::unique_ptr<Transfer>> incoming;
        std::vector<HttpRequestId> cancelled;

        while(!stop.stop_requested())
        {
            {
                std::lock_guard lock(submitMutex_);
                incoming.swap(submitted_);
                cancelled.swap(cancelled_);
            }
            for(auto& transfer : incoming)
            {
                transfer->slot = slotOf(transfer->request);
                waiting_.push_back(std::move(transfer));
            }
            incoming.clear();
            for(HttpRequestId id : cancelled)
            {
                abort(id);
            }
            cancelled.clear();
            startWaiting();

            int stillRunning = 0;
            curl_multi_perform(multi_, &stillRunning);

            int queued = 0;
            bool finished = false;
            while(CURLMsg* message = curl_multi_info_read(multi_, &queued))
            {
                if(message->msg == CURLMSG_DONE)
                {
                    finish(message->easy_handle, message->data.result);
                    finished = true;
                }
            }
            if(finished)
            {
                startWaiting(); // connections freed: started on the next turn
                curl_multi_wakeup(multi_);
            }

            curl_multi_poll(multi_, nullptr, 0, 1000, nullptr);
        }

        for(auto& [easy, transfer] : running_)
        {
            curl_multi_remove_handle(multi_, easy);
        }
        running_.clear();
        waiting_.clear();
    }

    // A request's pool and host ("scheme://host:port").
    static std::string slotOf(const HttpRequest& request)
    {
        std::string_view url = request.url;
        const size_t scheme = url.find("://");
        const size_t hostEnd = url.find_first_of("/?#", scheme == url.npos ? 0 : scheme + 3);
        url = url.substr(0, hostEnd);
        std::string slot(1, static_cast<char>('0' + static_cast<int>(request.pool)));
        for(char c : url)
        {
            slot += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return slot;
    }

    // Connections a pool may have to one host (0: any number).
    long limitOf(HttpPool pool) const
    {
        switch(pool)
        {
        case HttpPool::Inventory:
            return options_.inventoryConnectionsPerHost;
        case HttpPool::LongPoll:
            return 0;
        case HttpPool::Default:
            break;
        }
        return options_.maxConnectionsPerHost;
    }

    // Those waiting whose pool has a connection free on their host, in the
    // order they came.
    void startWaiting()
    {
        for(auto it = waiting_.begin(); it != waiting_.end();)
        {
            const long limit = limitOf((*it)->request.pool);
            long& busy = busy_[(*it)->slot];
            if(limit > 0 && busy >= limit)
            {
                it++;
                continue;
            }
            std::unique_ptr<Transfer> transfer = std::move(*it);
            it = waiting_.erase(it);
            start(std::move(transfer));
        }
    }

    void release(const Transfer& transfer)
    {
        const auto it = busy_.find(transfer.slot);
        if(it != busy_.end() && --it->second <= 0)
        {
            busy_.erase(it);
        }
    }

    void start(std::unique_ptr<Transfer> transfer)
    {
        {
            std::lock_guard lock(callbackMutex_);
            if(!active_.contains(transfer->id))
            {
                return; // cancelled before it started
            }
        }

        CURL* easy = curl_easy_init();
        transfer->easy = easy;
        const HttpRequest& request = transfer->request;

        curl_easy_setopt(easy, CURLOPT_URL, request.url.c_str());
        curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, transfer->errorBuffer);
        curl_easy_setopt(easy, CURLOPT_PRIVATE, transfer.get());
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, onBody);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, transfer.get());
        curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, onHeader);
        curl_easy_setopt(easy, CURLOPT_HEADERDATA, transfer.get());
        curl_easy_setopt(easy, CURLOPT_USERAGENT, options_.userAgent.c_str());
        curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout.count()));
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS,
                         static_cast<long>(request.connectTimeout.count()));
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, request.followRedirects ? 1L : 0L);
        curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 10L);
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, options_.verifyPeer ? 1L : 0L);
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, options_.verifyPeer ? 2L : 0L);
        if(!options_.caBundlePath.empty())
        {
            curl_easy_setopt(easy, CURLOPT_CAINFO, options_.caBundlePath.c_str());
        }

        switch(request.method)
        {
        case HttpMethod::Get:
            curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L);
            break;
        case HttpMethod::Head:
            curl_easy_setopt(easy, CURLOPT_NOBODY, 1L);
            break;
        case HttpMethod::Post:
            curl_easy_setopt(easy, CURLOPT_POST, 1L);
            break;
        default:
            curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST,
                             std::string(methodName(request.method)).c_str());
            break;
        }
        if(request.method == HttpMethod::Post || !request.body.empty())
        {
            // The body lives in the Transfer, which outlives the easy handle.
            curl_easy_setopt(easy, CURLOPT_POSTFIELDS, request.body.data());
            curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE,
                             static_cast<curl_off_t>(request.body.size()));
        }

        for(const auto& [name, value] : request.headers)
        {
            const std::string line = name + ": " + value;
            transfer->headerList = curl_slist_append(transfer->headerList, line.c_str());
        }
        // Don't wait for "100 Continue" before sending POST bodies.
        transfer->headerList = curl_slist_append(transfer->headerList, "Expect:");
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, transfer->headerList);

        curl_multi_add_handle(multi_, easy);
        busy_[transfer->slot]++;
        running_.emplace(easy, std::move(transfer));
    }

    void abort(HttpRequestId id)
    {
        for(auto it = running_.begin(); it != running_.end(); it++)
        {
            if(it->second->id == id)
            {
                curl_multi_remove_handle(multi_, it->first);
                release(*it->second);
                running_.erase(it);
                return;
            }
        }
        std::erase_if(waiting_, [id](const auto& transfer) { return transfer->id == id; });
    }

    void finish(CURL* easy, CURLcode code)
    {
        curl_multi_remove_handle(multi_, easy);
        const auto it = running_.find(easy);
        if(it == running_.end())
        {
            return;
        }
        std::unique_ptr<Transfer> transfer = std::move(it->second);
        running_.erase(it);
        release(*transfer);

        HttpResponse& response = transfer->response;
        response.requestId = transfer->id;
        response.error = mapError(code);
        if(code != CURLE_OK)
        {
            response.errorMessage =
                transfer->errorBuffer[0] ? transfer->errorBuffer : curl_easy_strerror(code);
        }
        long status = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
        response.status = static_cast<int>(status);

        // The callback runs under callbackMutex_ so cancel() can wait for it.
        std::lock_guard lock(callbackMutex_);
        if(active_.erase(transfer->id) > 0 && transfer->callback)
        {
            transfer->callback(std::move(response));
        }
    }

    HttpClientOptions options_;
    CURLM* multi_ = nullptr;
    std::atomic<HttpRequestId> nextId_{1};

    // Guards submitted_ and cancelled_ (handoff to the I/O thread).
    std::mutex submitMutex_;
    std::vector<std::unique_ptr<Transfer>> submitted_;
    std::vector<HttpRequestId> cancelled_;

    // Guards active_ and is held while a callback runs. Recursive so a
    // callback may call send() or cancel().
    std::recursive_mutex callbackMutex_;
    std::unordered_set<HttpRequestId> active_;

    // I/O thread only.
    std::unordered_map<CURL*, std::unique_ptr<Transfer>> running_;
    std::deque<std::unique_ptr<Transfer>> waiting_; // for a connection (startWaiting)
    std::unordered_map<std::string, long> busy_;    // slot: connections taken

    std::jthread thread_; // last: started after everything above exists
};

} // namespace

std::unique_ptr<HttpClient> createHttpClient(const HttpClientOptions& options)
{
    return std::make_unique<CurlHttpClient>(options);
}

} // namespace viewer::net
