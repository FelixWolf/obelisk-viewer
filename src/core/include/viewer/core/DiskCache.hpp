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

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
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
    Sound, // sound assets (Ogg Vorbis) as downloaded
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

    // The most the cache may take on disk, all kinds together and however
    // many viewers share it (0: any amount). Kept to in the background, by
    // removing the entries read or written longest ago, so it may be a
    // little over for a while. Viewers with different limits each keep to
    // their own: the lowest wins.
    virtual void setLimit(uint64_t bytes) = 0;

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
//
// The limit (nothing in memory is relied on, since other processes change the
// files): an entry's modification time is when it was last used (read() renews
// it, at most every few minutes). The entries are worked on in 256 slices, by
// the id's first byte (ids are random, so each is a fair sample): a file
// beside them, "usage", has each slice's size, which each instance adds to
// what it writes and removes. While they're over the limit, slice after slice
// is measured and loses its oldest quarter, by whichever instance has the lock
// on "usage.lock", held for a slice at a time. A slice is measured again now
// and then anyway (viewers that die don't count what they wrote).
class FileCache final : public DiskCache
{
public:
    explicit FileCache(std::filesystem::path root);
    ~FileCache() override;

    FileCache(const FileCache&) = delete;
    FileCache& operator=(const FileCache&) = delete;

    std::optional<std::vector<uint8_t>> read(const CacheKey& key) override;
    bool write(const CacheKey& key, std::span<const uint8_t> bytes) override;
    bool contains(const CacheKey& key) override;
    void remove(const CacheKey& key) override;
    void clear(CacheKind kind) override;
    void setLimit(uint64_t bytes) override;
    std::string describe() const override;

    std::filesystem::path pathFor(const CacheKey& key) const;

private:
    void maintain(std::stop_token stop);
    enum class Step : uint8_t
    {
        Done,
        More,  // go on to the next slice
        Hurry, // the same, soon: well over the limit
        Idle,  // the same, but there was nothing to do in this one
        Busy,  // another instance has the lock
    };
    // Adds what's been written to the shared sizes, and if there's need
    // (or `remeasure`) measures one slice, trimming it if over the limit.
    Step step(bool remeasure, std::stop_token stop);
    // Measures a slice; with a target, also removes up to its oldest quarter
    // (more if they're far over) while it and the others are over that. What's left in it; nothing
    // if it was stopped.
    std::optional<uint64_t> visitSlice(unsigned slice, std::optional<uint64_t> target,
                                       uint64_t others, uint64_t& removed, std::stop_token stop);
    void count(unsigned slice, int64_t bytes);
    uint64_t checkInterval() const;

    std::filesystem::path root_;
    uint64_t writerToken_; // random per instance: temporary names differ between processes
    std::atomic<uint64_t> writes_{0}; // and between this instance's writes

    std::atomic<uint64_t> limit_{0};
    // Since the shared sizes were last updated: the change of each slice's,
    // and the bytes written.
    std::array<std::atomic<int64_t>, 256> deltas_{};
    std::atomic<uint64_t> written_{0};
    bool lockWarned_ = false; // the maintenance thread's
    std::mutex wakeMutex_;
    std::condition_variable_any wake_;
    bool limitChanged_ = false; // guarded by wakeMutex_
    std::jthread maintenance_;  // started by the first limit
};

} // namespace viewer
