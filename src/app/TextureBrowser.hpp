#pragma once

// Developer panel that exercises the whole texture pipeline:
//   J2K files on disk -> TextureCache (worker threads: decode, mips, KTX2)
//   -> results drained here on the main thread -> uploaded to the GPU -> shown.
// Importing the same directory a second time should hit the KTX2 cache.

#include "viewer/assets/TextureCache.hpp"
#include "viewer/render/Renderer.hpp"

#include <chrono>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>

namespace viewer
{

class TextureBrowser
{
public:
    TextureBrowser(assets::TextureCache& cache, render::Renderer& renderer);

    // Queues every UUID-named file in `directory` (e.g. <uuid>.j2c / .jp2).
    void importDirectory(const std::filesystem::path& directory);

    // Drains finished requests and uploads a bounded amount per frame.
    void update();

    void draw();

private:
    void logImportSummary() const;

    struct Entry
    {
        Uuid id;
        render::GpuTexture texture;
        uint32_t sourceWidth = 0; // largest level the result contained
        uint32_t sourceHeight = 0;
        image::AlphaMode alphaMode = image::AlphaMode::Opaque;
        assets::TextureResult::Status status = assets::TextureResult::Status::Failed;
        double milliseconds = 0;
    };

    struct Stats
    {
        size_t requested = 0;
        size_t skippedNames = 0;
        size_t fromCache = 0;
        size_t decoded = 0;
        size_t failed = 0;
        double workerMilliseconds = 0;
        std::chrono::steady_clock::time_point started;
        std::chrono::steady_clock::time_point lastCompleted;

        size_t completed() const { return fromCache + decoded + failed; }
    };

    assets::TextureCache& cache_;
    render::Renderer& renderer_;

    std::deque<assets::TextureResult> pendingUpload_;
    std::vector<assets::TextureResult> drainScratch_;
    std::vector<Entry> entries_;
    Stats stats_;
    bool importActive_ = false; // log a summary when the current import finishes

    std::string directoryInput_;
    int maxDimension_ = 256;
    float thumbnailSize_ = 96.0f;
};

} // namespace viewer
