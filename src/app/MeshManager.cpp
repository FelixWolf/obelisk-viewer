#include "MeshManager.hpp"

#include "FaceUpload.hpp"

#include "viewer/core/Log.hpp"

#include <format>
#include <thread>

namespace viewer
{

namespace
{

constexpr size_t kMaxDownloads = 8;

} // namespace

MeshManager::MeshManager(net::HttpClient& http, JobSystem& jobs, render::Renderer& renderer)
    : http_(http), jobs_(jobs), renderer_(renderer),
      requests_(std::make_unique<net::HttpRequestGroup>(http))
{
}

MeshManager::~MeshManager()
{
    requests_.reset();
    while(decoding_.load() > 0)
    {
        std::this_thread::yield();
    }
}

void MeshManager::setViewerAssetUrl(std::string url)
{
    if(url == viewerAssetUrl_)
    {
        return;
    }
    viewerAssetUrl_ = std::move(url);
    // Downloads in flight belong to the old URL: drop them and start over.
    requests_ = std::make_unique<net::HttpRequestGroup>(http_);
    downloading_ = 0;
    std::vector<Download> stale;
    downloads_.drain(stale);
    queue_.clear();
    for(auto& [id, e] : entries_)
    {
        if(e.header == State::Downloading || e.header == State::Queued)
        {
            e.header = State::None;
        }
        for(Lod& lod : e.lods)
        {
            if(lod.state == State::Downloading || lod.state == State::Queued)
            {
                lod.state = State::None;
            }
        }
    }
}

void MeshManager::queue(const Request& request)
{
    queue_.push_back(request);
}

const std::vector<render::Mesh>* MeshManager::get(const Uuid& id, int lod)
{
    if(id.isNull())
    {
        return nullptr;
    }
    Entry& e = entries_[id];
    if(e.header == State::None && !viewerAssetUrl_.empty())
    {
        e.header = State::Queued;
        queue({id, -1});
    }
    if(e.header != State::Ready)
    {
        return nullptr;
    }

    const int wanted = e.parsed.closestLod(lod);
    if(wanted < 0)
    {
        return nullptr;
    }
    Lod& slot = e.lods[static_cast<size_t>(wanted)];
    if(slot.state == State::None && !viewerAssetUrl_.empty())
    {
        slot.state = State::Queued;
        queue({id, wanted});
    }
    if(slot.state == State::Ready)
    {
        return &slot.faces;
    }
    // Meanwhile, the closest level that is ready.
    for(int distance = 1; distance < 4; distance++)
    {
        for(const int other : {wanted + distance, wanted - distance})
        {
            if(other >= 0 && other < 4 && e.lods[static_cast<size_t>(other)].state == State::Ready)
            {
                return &e.lods[static_cast<size_t>(other)].faces;
            }
        }
    }
    return nullptr;
}

void MeshManager::startDownloads()
{
    while(downloading_ < kMaxDownloads && !queue_.empty() && !viewerAssetUrl_.empty())
    {
        const Request request = queue_.front();
        queue_.pop_front();
        auto it = entries_.find(request.id);
        if(it == entries_.end())
        {
            continue;
        }
        Entry& e = it->second;

        int64_t begin = 0;
        int64_t end = static_cast<int64_t>(assets::kMeshHeaderFetchSize) - 1;
        if(request.lod < 0)
        {
            if(e.header != State::Queued)
            {
                continue;
            }
            e.header = State::Downloading;
        }
        else
        {
            Lod& slot = e.lods[static_cast<size_t>(request.lod)];
            if(slot.state != State::Queued)
            {
                continue;
            }
            const assets::MeshBlock& block = e.parsed.lods[static_cast<size_t>(request.lod)];
            begin = static_cast<int64_t>(e.parsed.headerSize) + block.offset;
            end = begin + block.size - 1;
            slot.state = State::Downloading;
        }

        // As the official viewer: <ViewerAsset>/?mesh_id=<uuid>, byte ranges.
        net::HttpRequest http{
            .url = std::format("{}/?mesh_id={}", viewerAssetUrl_, request.id.toString())};
        http.headers.add("Accept", "application/vnd.ll.mesh");
        http.headers.add("Range", std::format("bytes={}-{}", begin, end));
        http.timeout = std::chrono::seconds(60);
        downloading_++;
        requests_->send(std::move(http), [this, request, begin](net::HttpResponse&& response)
                        { downloads_.push({request, begin, std::move(response)}); });
    }
}

void MeshManager::handleDownload(Download& download)
{
    downloading_--;
    auto it = entries_.find(download.request.id);
    if(it == entries_.end())
    {
        return;
    }
    Entry& e = it->second;
    const int lod = download.request.lod;
    State& state = lod < 0 ? e.header : e.lods[static_cast<size_t>(lod)].state;
    if(state != State::Downloading)
    {
        return;
    }
    net::HttpResponse& response = download.response;
    if(!response.ok() || response.body.empty())
    {
        if(response.status != 404)
        {
            log::warn("mesh {} download failed: {}", download.request.id.toString(),
                      response.error != net::HttpError::None
                          ? std::string(net::errorName(response.error))
                          : std::format("HTTP {}", response.status));
        }
        state = State::Failed;
        return;
    }
    bytesDownloaded_ += response.body.size();

    // A server may ignore the range and send the whole asset (200).
    std::vector<uint8_t> bytes = std::move(response.body);
    if(response.status == 200 && download.begin > 0)
    {
        if(static_cast<int64_t>(bytes.size()) <= download.begin)
        {
            state = State::Failed;
            return;
        }
        bytes.erase(bytes.begin(), bytes.begin() + download.begin);
    }

    if(lod < 0)
    {
        auto header = assets::parseMeshHeader(bytes);
        if(!header)
        {
            log::warn("mesh {}: {}", download.request.id.toString(), header.error());
            state = State::Failed;
            return;
        }
        e.parsed = *header;
        state = State::Ready;
        return;
    }

    // Decode on a worker.
    state = State::Decoding;
    const size_t size = static_cast<size_t>(e.parsed.lods[static_cast<size_t>(lod)].size);
    if(bytes.size() > size)
    {
        bytes.resize(size);
    }
    decoding_++;
    jobs_.submit(
        [this, request = download.request, data = std::move(bytes)]
        {
            Decoded result{request, {}, {}};
            if(auto faces = assets::decodeMeshLod(data))
            {
                result.faces = std::move(*faces);
            }
            else
            {
                result.error = faces.error();
            }
            decoded_.push(std::move(result));
            decoding_--;
        });
}

void MeshManager::update()
{
    downloads_.drain(drainedDownloads_);
    for(Download& d : drainedDownloads_)
    {
        handleDownload(d);
    }
    drainedDownloads_.clear();

    decoded_.drain(drainedDecoded_);
    for(Decoded& d : drainedDecoded_)
    {
        auto it = entries_.find(d.request.id);
        if(it == entries_.end())
        {
            continue;
        }
        Lod& slot = it->second.lods[static_cast<size_t>(d.request.lod)];
        if(!d.error.empty())
        {
            log::warn("mesh {}: {}", d.request.id.toString(), d.error);
            slot.state = State::Failed;
            continue;
        }
        slot.faces = uploadFaces(renderer_, d.faces);
        slot.state = State::Ready;
    }
    drainedDecoded_.clear();

    startDownloads();
}

MeshManager::Stats MeshManager::stats() const
{
    Stats s;
    s.meshes = entries_.size();
    s.downloading = downloading_;
    s.decoding = static_cast<size_t>(std::max(decoding_.load(), 0));
    s.bytesDownloaded = bytesDownloaded_;
    for(const auto& [id, e] : entries_)
    {
        s.failed += e.header == State::Failed;
        for(const Lod& lod : e.lods)
        {
            s.lodsReady += lod.state == State::Ready;
            s.failed += lod.state == State::Failed;
        }
    }
    return s;
}

} // namespace viewer
