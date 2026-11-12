#include "viewer/assets/TextureCache.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/image/Ktx2.hpp"
#include "viewer/image/TextureProcessing.hpp"

#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

namespace viewer::assets
{
namespace fs = std::filesystem;

namespace
{

std::optional<std::vector<uint8_t>> readWholeFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if(!in)
    {
        return std::nullopt;
    }
    const auto size = in.tellg();
    if(size < 0)
    {
        return std::nullopt;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    in.seekg(0);
    if(!in.read(reinterpret_cast<char*>(bytes.data()), size))
    {
        return std::nullopt;
    }
    return bytes;
}

void trimToMaxDimension(image::TextureData& texture, uint32_t maxDimension)
{
    if(maxDimension == 0)
    {
        return;
    }
    size_t first = 0;
    while(first + 1 < texture.levels.size() &&
          std::max(texture.levels[first].width, texture.levels[first].height) > maxDimension)
    {
        first++;
    }
    texture.levels.erase(texture.levels.begin(),
                         texture.levels.begin() + static_cast<ptrdiff_t>(first));
}

} // namespace

TextureCache::TextureCache(fs::path root, JobSystem& jobs, const image::J2kDecoder& decoder)
    : root_(std::move(root)), jobs_(jobs), decoder_(decoder)
{
    std::error_code ec;
    fs::create_directories(root_, ec);
    if(ec)
    {
        log::error("texture cache: cannot create {}: {}", root_.string(), ec.message());
    }
}

TextureCache::~TextureCache()
{
    std::unique_lock lock(mutex_);
    shuttingDown_ = true;
    drained_.wait(lock, [this] { return inFlight_.empty(); });
}

bool TextureCache::request(TextureRequest request)
{
    {
        std::lock_guard lock(mutex_);
        if(shuttingDown_ || !inFlight_.insert(request.id).second)
        {
            return false;
        }
    }

    jobs_.submit(
        [this, request = std::move(request)]
        {
            bool cancelled;
            {
                std::lock_guard lock(mutex_);
                cancelled = shuttingDown_;
            }
            if(!cancelled)
            {
                results_.push(process(request));
            }
            std::lock_guard lock(mutex_);
            inFlight_.erase(request.id);
            if(inFlight_.empty())
            {
                drained_.notify_all();
            }
        });
    return true;
}

TextureResult TextureCache::process(const TextureRequest& request) const
{
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();

    TextureResult result;
    result.id = request.id;
    auto finish = [&](TextureResult::Status status)
    {
        if(request.flipRows && (status == TextureResult::Status::LoadedFromCache ||
                                status == TextureResult::Status::Decoded))
        {
            image::flipRows(result.texture);
        }
        result.status = status;
        result.milliseconds =
            std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        return std::move(result);
    };

    // 1. Cache hit: read the KTX2 (only the levels the caller wants).
    const fs::path path = pathFor(request.id);
    if(auto file = readWholeFile(path))
    {
        if(auto texture = image::readKtx2(*file, {.maxDimension = request.maxDimension}))
        {
            // An entry decoded at a coarser level than requested is a miss
            // if we can fetch better data.
            if(texture->discardLevel <= request.discardLevel || !request.source)
            {
                result.texture = std::move(*texture);
                return finish(TextureResult::Status::LoadedFromCache);
            }
        }
        else
        {
            log::warn("texture cache: corrupt entry {}, re-decoding", path.string());
        }
    }

    // 2. Miss: fetch J2K, decode, write the cache entry.
    if(!request.source)
    {
        return finish(TextureResult::Status::NotFound);
    }
    const auto j2k = request.source();
    if(!j2k)
    {
        return finish(TextureResult::Status::NotFound);
    }
    auto texture = image::textureFromJ2k(decoder_, *j2k, request.discardLevel);
    if(!texture)
    {
        return finish(TextureResult::Status::Failed);
    }
    const std::vector<uint8_t> ktx = image::writeKtx2(*texture);
    if(ktx.empty() || !writeAtomically(request.id, ktx))
    {
        log::warn("texture cache: failed to write {}", path.string());
    }

    trimToMaxDimension(*texture, request.maxDimension);
    result.texture = std::move(*texture);
    return finish(TextureResult::Status::Decoded);
}

bool TextureCache::writeAtomically(const Uuid& id, const std::vector<uint8_t>& bytes) const
{
    const fs::path finalPath = pathFor(id);
    std::error_code ec;
    fs::create_directories(finalPath.parent_path(), ec);

    // Unique temp name per thread so concurrent writers never collide; the
    // rename makes the entry appear all at once (no half-written files).
    std::ostringstream tempName;
    tempName << finalPath.filename().string() << ".tmp" << std::this_thread::get_id();
    const fs::path tempPath = finalPath.parent_path() / tempName.str();
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if(!out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size())))
        {
            fs::remove(tempPath, ec);
            return false;
        }
    }
    fs::rename(tempPath, finalPath, ec);
    if(ec)
    {
        fs::remove(tempPath, ec);
        return false;
    }
    return true;
}

bool TextureCache::contains(const Uuid& id) const
{
    std::error_code ec;
    return fs::is_regular_file(pathFor(id), ec);
}

fs::path TextureCache::pathFor(const Uuid& id) const
{
    const std::string name = id.toString();
    return root_ / name.substr(0, 2) / (name + ".ktx2");
}

size_t TextureCache::inFlight() const
{
    std::lock_guard lock(mutex_);
    return inFlight_.size();
}

void TextureCache::clear()
{
    std::error_code ec;
    for(const auto& entry : fs::directory_iterator(root_, ec))
    {
        if(entry.is_directory() && entry.path().filename().string().size() == 2)
        {
            fs::remove_all(entry.path(), ec);
        }
    }
}

} // namespace viewer::assets
