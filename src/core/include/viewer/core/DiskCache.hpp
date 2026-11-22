#pragma once

// Where downloaded and processed assets are kept between sessions. Everything
// that reads or writes the disk cache goes through this interface, so how
// entries are stored (one file each today; a database or pack files later)
// can change without touching its users.
//
// Entries are byte blobs addressed by asset kind, asset id and part (a mesh
// asset has several: its header, skin and levels of detail). Several viewers
// may share one cache: implementations must stay consistent when other
// processes read and write it at the same time.

#include "viewer/core/Uuid.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace viewer
{

enum class CacheKind : uint8_t
{
    Texture,   // decoded KTX2
    Mesh,      // a mesh asset's blocks as downloaded
    Animation, // .anim bytes as downloaded
    Objects,   // a region's cacheable objects (world::RegionObjectCache), by region id
    Material,  // PBR material assets as downloaded
    // A texture's J2K as downloaded, kept while its KTX2 is below full
    // resolution (for decoding a bigger one). Stored with the textures:
    // clearing either kind clears both.
    TextureSource,
    // An avatar bake's morph mask (its fifth component; the KTX2 has four):
    // width and height (little-endian 32-bit) then the bytes, top row
    // first. Stored with the textures too.
    TextureMask,
};

struct CacheKey
{
    CacheKind kind = CacheKind::Texture;
    Uuid id;
    int part = 0; // meaning per kind (meshes: kMeshHeader, kMeshSkin or a level)

    static constexpr int kMeshHeader = -1;
    static constexpr int kMeshSkin = -2;
};

class DiskCache
{
public:
    virtual ~DiskCache() = default;

    // All of these are safe to call from any thread.

    // The entry's bytes; nothing if there's none (or it can't be read).
    virtual std::optional<std::vector<uint8_t>> read(const CacheKey& key) = 0;
    // Stores the entry, replacing any. Readers (in any process) see the old
    // entry or the new one, never part of one. False if it couldn't be
    // stored; the cache is still consistent then.
    virtual bool write(const CacheKey& key, std::span<const uint8_t> bytes) = 0;
    virtual bool contains(const CacheKey& key) = 0;
    // Removes one entry (nothing if there's none).
    virtual void remove(const CacheKey& key) = 0;
    // Removes every entry of one kind.
    virtual void clear(CacheKind kind) = 0;

    // Where the cache is, for showing to the user.
    virtual std::string describe() const = 0;
};

// One file per entry under a root directory:
//   textures/<xx>/<uuid>.ktx2, .j2c (TextureSource)
//   meshes/<xx>/<uuid>.header, .skin, .lod<N>
//   animations/<xx>/<uuid>.anim
//   objects/<xx>/<region uuid>.objects
//   materials/<xx>/<uuid>.material
// (<xx>: the id's first two hex digits). Writes go to a uniquely named
// temporary file that is renamed over the entry, so concurrent writers, in
// this process or another, never mix their bytes.
class FileCache final : public DiskCache
{
public:
    explicit FileCache(std::filesystem::path root);

    std::optional<std::vector<uint8_t>> read(const CacheKey& key) override;
    bool write(const CacheKey& key, std::span<const uint8_t> bytes) override;
    bool contains(const CacheKey& key) override;
    void remove(const CacheKey& key) override;
    void clear(CacheKind kind) override;
    std::string describe() const override;

    std::filesystem::path pathFor(const CacheKey& key) const;

private:
    std::filesystem::path root_;
    uint64_t writerToken_; // random per instance: temporary names differ between processes
    std::atomic<uint64_t> writes_{0}; // and between this instance's writes
};

} // namespace viewer
