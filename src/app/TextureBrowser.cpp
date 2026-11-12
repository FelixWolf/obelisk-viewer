#include "TextureBrowser.hpp"

#include "viewer/core/Log.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <fstream>

namespace viewer
{
namespace fs = std::filesystem;
using Status = assets::TextureResult::Status;

namespace
{

// Per-frame upload budget, so a burst of completed decodes can't stall a frame.
constexpr size_t kMaxUploadsPerFrame = 32;
constexpr size_t kMaxUploadBytesPerFrame = 64u << 20;

std::optional<std::vector<uint8_t>> readFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if(!in)
    {
        return std::nullopt;
    }
    const auto size = in.tellg();
    std::vector<uint8_t> bytes(static_cast<size_t>(std::max<std::streamoff>(size, 0)));
    in.seekg(0);
    if(!in.read(reinterpret_cast<char*>(bytes.data()), size))
    {
        return std::nullopt;
    }
    return bytes;
}

const char* statusLabel(Status status)
{
    switch(status)
    {
    case Status::LoadedFromCache:
        return "cache hit";
    case Status::Decoded:
        return "decoded";
    case Status::NotFound:
        return "not found";
    case Status::Failed:
        return "failed";
    }
    return "?";
}

} // namespace

TextureBrowser::TextureBrowser(assets::TextureCache& cache, render::Renderer& renderer)
    : cache_(cache), renderer_(renderer)
{
}

void TextureBrowser::importDirectory(const fs::path& directory)
{
    std::error_code ec;
    fs::directory_iterator it(directory, ec);
    if(ec)
    {
        log::error("texture browser: cannot open {}: {}", directory.string(), ec.message());
        return;
    }

    if(cache_.inFlight() == 0 && pendingUpload_.empty())
    {
        stats_ = {};
        stats_.started = std::chrono::steady_clock::now();
    }

    for(const auto& entry : it)
    {
        if(!entry.is_regular_file())
        {
            continue;
        }
        const auto id = Uuid::parse(entry.path().stem().string());
        if(!id)
        {
            stats_.skippedNames++;
            continue;
        }
        assets::TextureRequest request;
        request.id = *id;
        request.maxDimension = static_cast<uint32_t>(maxDimension_);
        request.source = [path = entry.path()] { return readFile(path); };
        if(cache_.request(std::move(request)))
        {
            stats_.requested++;
        }
    }
    importActive_ = true;
}

void TextureBrowser::logImportSummary() const
{
    const double wallMs =
        std::chrono::duration<double, std::milli>(stats_.lastCompleted - stats_.started).count();
    log::info("texture import done: {} textures in {:.0f} ms ({:.1f}/s): {} cache hits, {} "
              "decoded, {} failed; avg worker time {:.1f} ms",
              stats_.completed(), wallMs,
              static_cast<double>(stats_.completed()) * 1000.0 / std::max(wallMs, 1.0),
              stats_.fromCache, stats_.decoded, stats_.failed,
              stats_.workerMilliseconds /
                  static_cast<double>(std::max<size_t>(stats_.completed(), 1)));
}

void TextureBrowser::update()
{
    cache_.drainResults(drainScratch_);
    for(auto& result : drainScratch_)
    {
        switch(result.status)
        {
        case Status::LoadedFromCache:
            stats_.fromCache++;
            break;
        case Status::Decoded:
            stats_.decoded++;
            break;
        default:
            stats_.failed++;
            break;
        }
        stats_.workerMilliseconds += result.milliseconds;
        stats_.lastCompleted = std::chrono::steady_clock::now();
        pendingUpload_.push_back(std::move(result));
    }

    // Take a bounded batch and upload it in one copy pass.
    std::vector<assets::TextureResult> batch;
    size_t batchBytes = 0;
    while(!pendingUpload_.empty() && batch.size() < kMaxUploadsPerFrame)
    {
        size_t bytes = 0;
        for(const auto& level : pendingUpload_.front().texture.levels)
        {
            bytes += level.byteSize();
        }
        if(!batch.empty() && batchBytes + bytes > kMaxUploadBytesPerFrame)
        {
            break;
        }
        batchBytes += bytes;
        batch.push_back(std::move(pendingUpload_.front()));
        pendingUpload_.pop_front();
    }
    if(batch.empty())
    {
        if(importActive_ && cache_.inFlight() == 0 && stats_.completed() > 0)
        {
            importActive_ = false;
            logImportSummary();
        }
        return;
    }

    std::vector<const image::TextureData*> uploads;
    for(const auto& result : batch)
    {
        uploads.push_back(&result.texture);
    }
    // UNORM: ImGui draws into a non-sRGB swapchain, so show the bytes as-is.
    std::vector<render::GpuTexture> textures = renderer_.uploadTextures(uploads, false);

    for(size_t i = 0; i < batch.size(); i++)
    {
        Entry entry;
        entry.id = batch[i].id;
        entry.texture = std::move(textures[i]);
        entry.sourceWidth = batch[i].texture.width();
        entry.sourceHeight = batch[i].texture.height();
        entry.alphaMode = batch[i].texture.alphaMode;
        entry.status = batch[i].status;
        entry.milliseconds = batch[i].milliseconds;
        entries_.push_back(std::move(entry));
    }
}

void TextureBrowser::draw()
{
    if(!ImGui::Begin("Texture Browser"))
    {
        ImGui::End();
        return;
    }

    // --- Import controls -----------------------------------------------------
    ImGui::SetNextItemWidth(-90.0f);
    const bool enter =
        ImGui::InputTextWithHint("##dir", "directory of <uuid>.j2c / .jp2 files", &directoryInput_,
                                 ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if((ImGui::Button("Import") || enter) && !directoryInput_.empty())
    {
        importDirectory(directoryInput_);
    }

    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderInt("Max upload size", &maxDimension_, 16, 4096, "%d px",
                     ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip("Largest mip uploaded to the GPU for new imports.\n"
                          "The disk cache always keeps the full mip chain.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderFloat("Thumbnail", &thumbnailSize_, 32.0f, 512.0f, "%.0f px");

    if(ImGui::Button("Unload GPU textures"))
    {
        entries_.clear();
    }
    ImGui::SetItemTooltip("Drop uploaded textures; importing again will then load from the cache.");
    ImGui::SameLine();
    if(ImGui::Button("Clear disk cache"))
    {
        cache_.clear();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", cache_.describe().c_str());

    // --- Stats -----------------------------------------------------------------
    ImGui::Separator();
    const size_t inFlight = cache_.inFlight();
    ImGui::Text("requested %zu   cache hits %zu   decoded %zu   failed %zu   in flight %zu   "
                "awaiting upload %zu",
                stats_.requested, stats_.fromCache, stats_.decoded, stats_.failed, inFlight,
                pendingUpload_.size());
    if(stats_.skippedNames > 0)
    {
        ImGui::TextDisabled("skipped %zu files without a UUID name", stats_.skippedNames);
    }
    if(stats_.completed() > 0)
    {
        const double wallMs =
            std::chrono::duration<double, std::milli>(
                (inFlight > 0 ? std::chrono::steady_clock::now() : stats_.lastCompleted) -
                stats_.started)
                .count();
        ImGui::Text("wall %.0f ms   %.1f textures/s   avg worker time %.1f ms/texture", wallMs,
                    static_cast<double>(stats_.completed()) * 1000.0 / std::max(wallMs, 1.0),
                    stats_.workerMilliseconds / static_cast<double>(stats_.completed()));
    }

    // --- Thumbnail grid -----------------------------------------------------------
    ImGui::Separator();
    if(ImGui::BeginChild("grid"))
    {
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float available = ImGui::GetContentRegionAvail().x;
        const int columns =
            std::max(1, static_cast<int>((available + spacing) / (thumbnailSize_ + spacing)));
        int column = 0;
        for(const Entry& entry : entries_)
        {
            if(column++ % columns != 0)
            {
                ImGui::SameLine();
            }
            const ImVec2 size(thumbnailSize_, thumbnailSize_);
            if(entry.texture)
            {
                ImGui::Image(ImTextureRef(static_cast<ImTextureID>(
                                 reinterpret_cast<intptr_t>(entry.texture.get()))),
                             size);
            }
            else
            {
                ImGui::Dummy(size);
            }
            if(ImGui::BeginItemTooltip())
            {
                ImGui::TextUnformatted(entry.id.toString().c_str());
                ImGui::Text("%s in %.1f ms", statusLabel(entry.status), entry.milliseconds);
                ImGui::Text("uploaded %u x %u, %u mips", entry.sourceWidth, entry.sourceHeight,
                            entry.texture.levels());
                ImGui::Text("alpha: %s",
                            std::string(image::alphaModeName(entry.alphaMode)).c_str());
                ImGui::EndTooltip();
            }
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace viewer
