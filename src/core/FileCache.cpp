#include "viewer/core/DiskCache.hpp"

#include <format>
#include <fstream>
#include <random>

namespace viewer
{

namespace fs = std::filesystem;

namespace
{

const char* directoryFor(CacheKind kind)
{
    switch(kind)
    {
    case CacheKind::Texture:
    case CacheKind::TextureSource:
        return "textures";
    case CacheKind::Mesh:
        return "meshes";
    case CacheKind::Animation:
        return "animations";
    case CacheKind::Objects:
        return "objects";
    case CacheKind::Material:
        return "materials";
    }
    return "other";
}

} // namespace

FileCache::FileCache(fs::path root) : root_(std::move(root))
{
    std::random_device random;
    writerToken_ = static_cast<uint64_t>(random()) << 32 | random();
}

fs::path FileCache::pathFor(const CacheKey& key) const
{
    const std::string id = key.id.toString();
    std::string name;
    switch(key.kind)
    {
    case CacheKind::Texture:
        name = id + ".ktx2";
        break;
    case CacheKind::TextureSource:
        name = id + ".j2c";
        break;
    case CacheKind::Mesh:
        name = key.part == CacheKey::kMeshHeader ? id + ".header"
               : key.part == CacheKey::kMeshSkin ? id + ".skin"
                                                 : std::format("{}.lod{}", id, key.part);
        break;
    case CacheKind::Animation:
        name = id + ".anim";
        break;
    case CacheKind::Objects:
        name = id + ".objects";
        break;
    case CacheKind::Material:
        name = id + ".material";
        break;
    }
    return root_ / directoryFor(key.kind) / id.substr(0, 2) / name;
}

std::optional<std::vector<uint8_t>> FileCache::read(const CacheKey& key)
{
    std::ifstream in(pathFor(key), std::ios::binary | std::ios::ate);
    if(!in)
    {
        return std::nullopt;
    }
    const auto size = in.tellg();
    if(size <= 0)
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

bool FileCache::write(const CacheKey& key, std::span<const uint8_t> bytes)
{
    const fs::path path = pathFor(key);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    // Unique across threads (the count) and processes (the token), then
    // renamed over the entry: it appears all at once.
    const fs::path temporary =
        path.parent_path() /
        std::format("{}.{:016x}.{}.tmp", path.filename().string(), writerToken_, writes_++);
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if(!out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size())))
        {
            out.close();
            fs::remove(temporary, ec);
            return false;
        }
    }
    fs::rename(temporary, path, ec);
    if(ec)
    {
        fs::remove(temporary, ec);
        return false;
    }
    return true;
}

bool FileCache::contains(const CacheKey& key)
{
    std::error_code ec;
    return fs::is_regular_file(pathFor(key), ec);
}

void FileCache::remove(const CacheKey& key)
{
    std::error_code ec;
    fs::remove(pathFor(key), ec);
}

void FileCache::clear(CacheKind kind)
{
    std::error_code ec;
    for(const auto& entry : fs::directory_iterator(root_ / directoryFor(kind), ec))
    {
        if(entry.is_directory() && entry.path().filename().string().size() == 2)
        {
            fs::remove_all(entry.path(), ec);
        }
    }
}

std::string FileCache::describe() const
{
    return root_.string();
}

} // namespace viewer
