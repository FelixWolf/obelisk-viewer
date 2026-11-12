#pragma once

// Mesh assets by id, ready for drawing. Asking for one starts loading it:
// the header first (a Range request for its first 4 KiB, as the official
// viewer does), then the level of detail asked for (or the closest one the
// asset has), decoded on a worker thread and uploaded. While a level loads,
// get() returns another level that is ready, if any.
//
// Downloads use the region's ViewerAsset capability
// (<cap>/?mesh_id=<uuid>); setViewerAssetUrl() works like TextureManager's.
//
// Downloaded parts are cached on disk as they arrived (the header bytes and
// each level's compressed block) under <cache>/<xx>/<uuid>.header and
// .lod<N>, written atomically; disk reads and writes run on worker threads.

#include "viewer/assets/MeshAsset.hpp"
#include "viewer/core/JobSystem.hpp"
#include "viewer/core/MessageQueue.hpp"
#include "viewer/core/Uuid.hpp"
#include "viewer/net/HttpClient.hpp"
#include "viewer/render/Renderer.hpp"

#include <array>
#include <atomic>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace viewer
{

class MeshManager
{
public:
    MeshManager(net::HttpClient& http, JobSystem& jobs, render::Renderer& renderer,
                std::filesystem::path cacheRoot);
    // Cancels downloads and waits for decode jobs.
    ~MeshManager();

    MeshManager(const MeshManager&) = delete;
    MeshManager& operator=(const MeshManager&) = delete;

    void setViewerAssetUrl(std::string url);

    // Faces (one GPU mesh per texture entry) at level of detail `lod`
    // (0 lowest .. 3 highest), or the nearest ready one; nullptr if none is.
    const std::vector<render::Mesh>* get(const Uuid& id, int lod);

    // Whether the mesh is rigged (has skin weights); nothing until its header
    // has loaded.
    std::optional<bool> isRigged(const Uuid& id) const;
    // A rigged mesh's skin (joints and bind matrices); starts loading it once
    // the header is in. nullptr until then, or if it has none.
    std::shared_ptr<const assets::MeshSkin> skin(const Uuid& id);

    // Handles finished downloads and decodes.
    void update();

    // Bumped whenever a header or skin finishes loading (what isRigged()
    // and skin() answer changes).
    uint64_t revision() const { return revision_; }

    struct Stats
    {
        size_t meshes = 0;
        size_t lodsReady = 0;
        size_t downloading = 0;
        size_t decoding = 0;
        size_t failed = 0;
        uint64_t bytesDownloaded = 0;
        uint64_t diskHits = 0;
    };
    Stats stats() const;

private:
    enum class State : uint8_t
    {
        None,
        Loading, // reading the disk cache
        Queued,
        Downloading,
        Decoding,
        Ready,
        Failed,
    };
    struct Lod
    {
        State state = State::None;
        std::vector<render::Mesh> faces;
    };
    struct Entry
    {
        State header = State::None;
        assets::MeshHeader parsed{};
        std::array<Lod, 4> lods{};
        State skinState = State::None;
        std::shared_ptr<const assets::MeshSkin> skin;
    };
    // What a download or decode was for: the header (lod = kHeader), the
    // skin (kSkin) or a level.
    static constexpr int kHeader = -1;
    static constexpr int kSkin = -2;
    struct Request
    {
        Uuid id;
        int lod = kHeader;
    };
    struct Download
    {
        Request request;
        int64_t begin = 0; // the byte range asked for
        net::HttpResponse response;
    };
    struct DiskResult
    {
        Request request;
        std::optional<std::vector<uint8_t>> bytes;
    };
    struct Decoded
    {
        Request request;
        std::vector<volume::VolumeFace> faces;
        std::shared_ptr<const assets::MeshSkin> skin; // for kSkin
        std::string error;
    };

    void request(const Request& request);
    void startDownloads();
    void handleDownload(Download& download);
    // Bytes of a header or level, from the network or the disk cache.
    void handleBytes(const Request& request, std::vector<uint8_t> bytes);
    std::filesystem::path pathFor(const Request& request) const;
    static State& stateOf(Entry& e, int lod);
    void submit(std::function<void()> job);

    net::HttpClient& http_;
    JobSystem& jobs_;
    render::Renderer& renderer_;
    std::string viewerAssetUrl_;
    std::unordered_map<Uuid, Entry> entries_;
    std::deque<Request> queue_;
    size_t downloading_ = 0;
    uint64_t bytesDownloaded_ = 0;
    std::filesystem::path cacheRoot_;
    std::atomic<int> decoding_{0};
    std::atomic<int> jobsRunning_{0};
    uint64_t diskHits_ = 0;
    uint64_t revision_ = 0;

    MessageQueue<Download> downloads_;
    MessageQueue<Decoded> decoded_;
    MessageQueue<DiskResult> diskResults_;
    std::vector<DiskResult> drainedDisk_;
    std::vector<Download> drainedDownloads_;
    std::vector<Decoded> drainedDecoded_;
    // Last: destroyed first, so no download callback outlives the queues.
    std::unique_ptr<net::HttpRequestGroup> requests_;
};

} // namespace viewer
