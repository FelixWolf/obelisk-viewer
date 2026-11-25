#include "viewer/core/DiskCache.hpp"

#include "ProcessLock.hpp"
#include "viewer/core/Log.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
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
    case CacheKind::TextureMask:
        return "textures";
    case CacheKind::Mesh:
        return "meshes";
    case CacheKind::Animation:
        return "animations";
    case CacheKind::Objects:
        return "objects";
    case CacheKind::Material:
        return "materials";
    case CacheKind::Sound:
        return "sounds";
    }
    return "other";
}

constexpr const char* kDirectories[] = {"textures", "meshes",    "animations",
                                        "objects",  "materials", "sounds"};

using FileClock = fs::file_time_type::clock;

// How old an entry's time gets before reading it renews it.
constexpr auto kRenewAfter = std::chrono::minutes(10);
// Temporary files this old were left by a viewer that died.
constexpr auto kAbandonedAfter = std::chrono::hours(1);
// How often what's written is added to the shared total at least (and one
// slice measured again), how soon to try again when another viewer has the
// lock, and the pause between slices, which leaves the disk to others.
constexpr auto kReportEvery = std::chrono::minutes(5);
constexpr auto kRetryAfter = std::chrono::milliseconds(250);
constexpr auto kSlicePause = std::chrono::milliseconds(50);
constexpr uint64_t kMaxCheckInterval = 64ull << 20;

// About what a file takes on disk.
uint64_t sizeOnDisk(uint64_t bytes)
{
    constexpr uint64_t kBlock = 4096;
    return (bytes + kBlock - 1) / kBlock * kBlock;
}

// Whether a file of this name in directory <shard> is one of ours (the cache
// folder may be anywhere the user chose: nothing else is touched).
bool isEntryName(const std::string& shard, const std::string& name)
{
    if(name.size() < 38 || name[36] != '.' || !name.starts_with(shard))
    {
        return false;
    }
    const auto id = Uuid::parse(name.substr(0, 36));
    return id && id->toString() == name.substr(0, 36);
}

// What the files take, as known to all the viewers (read and written holding
// the lock).
struct Usage
{
    static constexpr int64_t kUnknown = -1;

    std::array<int64_t, 256> slices; // bytes, by the id's first byte
    unsigned next = 0;               // the slice to measure or trim next
    bool trimming = false;           // over the limit, not down to the target yet

    Usage() { slices.fill(kUnknown); }
    // Of those known: at least this much.
    uint64_t total() const
    {
        uint64_t sum = 0;
        for(const int64_t bytes : slices)
        {
            sum += static_cast<uint64_t>(std::max<int64_t>(bytes, 0));
        }
        return sum;
    }
    unsigned firstUnknown() const
    {
        return static_cast<unsigned>(std::ranges::find(slices, kUnknown) - slices.begin());
    }
};

constexpr const char* kUsageMagic = "obelisk-cache-usage-2";

// Nothing known if it can't be read, or makes no sense.
Usage readUsage(const fs::path& file)
{
    std::ifstream in(file);
    std::string magic;
    std::string end;
    Usage usage;
    in >> magic >> usage.next >> usage.trimming;
    for(int64_t& bytes : usage.slices)
    {
        in >> bytes;
        if(bytes < Usage::kUnknown || bytes > int64_t{1} << 50)
        {
            return {};
        }
    }
    in >> end; // a partly written file isn't taken for a whole one
    if(!in || magic != kUsageMagic || end != "end")
    {
        return {};
    }
    usage.next %= usage.slices.size();
    return usage;
}

void writeUsage(const fs::path& file, const Usage& usage)
{
    std::string text = std::format("{} {} {}", kUsageMagic, usage.next, int(usage.trimming));
    for(const int64_t bytes : usage.slices)
    {
        text += std::format(" {}", bytes);
    }
    std::ofstream(file, std::ios::trunc) << text << " end\n";
}

// Calls f(entry) for what's in a directory until it returns false. Files
// coming and going meanwhile is fine.
template <typename F>
void forEachIn(const fs::path& directory, F&& f)
{
    std::error_code ec;
    fs::directory_iterator it(directory, ec);
    while(!ec && it != fs::directory_iterator())
    {
        if(!f(*it))
        {
            return;
        }
        it.increment(ec);
    }
}

} // namespace

FileCache::FileCache(fs::path root) : root_(std::move(root))
{
    std::random_device random;
    writerToken_ = static_cast<uint64_t>(random()) << 32 | random();
}

FileCache::~FileCache()
{
    if(!maintenance_.joinable())
    {
        return;
    }
    maintenance_.request_stop();
    maintenance_.join();
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
    case CacheKind::TextureMask:
        name = id + ".mask";
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
    case CacheKind::Sound:
        name = id + ".ogg";
        break;
    }
    return root_ / directoryFor(key.kind) / id.substr(0, 2) / name;
}

std::optional<std::vector<uint8_t>> FileCache::read(const CacheKey& key)
{
    const fs::path path = pathFor(key);
    std::ifstream in(path, std::ios::binary | std::ios::ate);
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
    in.close();
    // It's been used now: kept longer than those that haven't. Whatever our
    // own limit, as another viewer may have one. (A time in the future, from
    // a clock set wrong, is put right too.)
    std::error_code ec;
    const auto now = FileClock::now();
    const auto used = fs::last_write_time(path, ec);
    if(!ec && (used > now + kRenewAfter || used < now - kRenewAfter))
    {
        fs::last_write_time(path, now, ec);
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
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        out.close(); // the last of it may only fail to be written here (a full disk)
        if(!out)
        {
            fs::remove(temporary, ec);
            return false;
        }
    }
    std::error_code none;
    const uint64_t replaced = fs::file_size(path, none);
    fs::rename(temporary, path, ec);
    if(ec)
    {
        fs::remove(temporary, ec);
        return false;
    }
    const uint64_t size = sizeOnDisk(bytes.size());
    count(key.id.bytes[0],
          static_cast<int64_t>(size) - (none ? 0 : static_cast<int64_t>(sizeOnDisk(replaced))));
    const uint64_t before = written_.fetch_add(size);
    const uint64_t interval = checkInterval();
    if(limit_ != 0 && before < interval && before + size >= interval)
    {
        // Under the mutex, so the thread isn't between looking and waiting.
        std::lock_guard lock(wakeMutex_);
        wake_.notify_all();
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
    const fs::path path = pathFor(key);
    std::error_code ec;
    const uint64_t size = fs::file_size(path, ec);
    if(!ec && fs::remove(path, ec))
    {
        count(key.id.bytes[0], -static_cast<int64_t>(sizeOnDisk(size)));
    }
}

void FileCache::clear(CacheKind kind)
{
    std::error_code ec;
    for(unsigned slice = 0; slice < deltas_.size(); slice++)
    {
        const fs::path directory = root_ / directoryFor(kind) / std::format("{:02x}", slice);
        int64_t size = 0;
        forEachIn(directory,
                  [&](const fs::directory_entry& entry)
                  {
                      std::error_code gone;
                      const uint64_t bytes = entry.file_size(gone);
                      size += gone ? 0 : static_cast<int64_t>(sizeOnDisk(bytes));
                      return true;
                  });
        fs::remove_all(directory, ec);
        if(!ec)
        {
            count(slice, -size);
        }
    }
}

void FileCache::count(unsigned slice, int64_t bytes)
{
    deltas_[slice] += bytes;
}

void FileCache::setLimit(uint64_t bytes)
{
    std::lock_guard lock(wakeMutex_);
    if(limit_.exchange(bytes) == bytes)
    {
        return;
    }
    limitChanged_ = true;
    if(!maintenance_.joinable())
    {
        maintenance_ = std::jthread([this](std::stop_token stop) { maintain(std::move(stop)); });
    }
    wake_.notify_all();
}

// How much to write before adding it to the shared total: what each viewer
// may be over the limit by.
uint64_t FileCache::checkInterval() const
{
    return std::clamp<uint64_t>(limit_ / 50, 1, kMaxCheckInterval);
}

void FileCache::maintain(std::stop_token stop)
{
    Step last = Step::Done;
    unsigned idle = 0; // slices in a row with nothing to remove
    while(!stop.stop_requested())
    {
        bool remeasure = false;
        {
            std::unique_lock lock(wakeMutex_);
            if(last == Step::Done)
            {
                remeasure = !wake_.wait_for(
                    lock, stop, kReportEvery, [this]
                    { return limitChanged_ || (limit_ != 0 && written_ >= checkInterval()); });
            }
            else
            {
                wake_.wait_for(lock, stop,
                               last == Step::Busy ? kRetryAfter
                               : last == Step::Idle || last == Step::Hurry
                                   ? std::chrono::milliseconds(1)
                                   : kSlicePause,
                               [this] { return limitChanged_; });
            }
            limitChanged_ = false;
        }
        last = step(remeasure, stop);
        // None of them can be removed (all in use): not until later.
        idle = last == Step::Idle ? idle + 1 : last == Step::Busy ? idle : 0;
        if(idle >= deltas_.size())
        {
            idle = 0;
            last = Step::Done;
        }
    }
    // Closing: what was written since is only counted. If another viewer keeps
    // the lock, it's found when the slices are next measured.
    for(int i = 0; i < 10 && step(false, stop) == Step::Busy; i++)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

FileCache::Step FileCache::step(bool remeasure, std::stop_token stop)
{
    const uint64_t limit = limit_;
    std::error_code ec;
    fs::create_directories(root_, ec);
    const ProcessLock lock(root_ / "usage.lock");
    if(lock.state() != ProcessLock::State::Held)
    {
        if(lock.state() == ProcessLock::State::Failed && !lockWarned_)
        {
            lockWarned_ = true;
            log::warn("disk cache: {} can't be locked; its size isn't being limited",
                      (root_ / "usage.lock").string());
        }
        return Step::Busy;
    }

    const fs::path file = root_ / "usage";
    Usage usage = readUsage(file);
    written_ = 0;
    for(size_t i = 0; i < deltas_.size(); i++)
    {
        const int64_t delta = deltas_[i].exchange(0);
        if(usage.slices[i] != Usage::kUnknown) // else it's yet to be measured
        {
            usage.slices[i] = std::max<int64_t>(usage.slices[i] + delta, 0);
        }
    }
    Step result = Step::Done;
    if(limit != 0 && !stop.stop_requested())
    {
        // Some way under, so it isn't full again straight away.
        const uint64_t target = limit - limit / 10;
        const uint64_t total = usage.total();
        usage.trimming = total > limit || (usage.trimming && total > target);
        const unsigned unknown = usage.firstUnknown();
        const bool measuring = unknown < usage.slices.size();
        if(usage.trimming || measuring || remeasure)
        {
            const unsigned slice = !usage.trimming && measuring ? unknown : usage.next;
            if(slice == usage.next)
            {
                usage.next = (slice + 1) % static_cast<unsigned>(usage.slices.size());
            }
            uint64_t removed = 0;
            try
            {
                // Over the target by this much, counting the slice as it is.
                const uint64_t others =
                    total - static_cast<uint64_t>(std::max<int64_t>(usage.slices[slice], 0));
                const auto left =
                    visitSlice(slice, usage.trimming ? std::optional(target) : std::nullopt, others,
                               removed, stop);
                if(left)
                {
                    usage.slices[slice] = static_cast<int64_t>(*left);
                }
            }
            catch(const std::exception& e)
            {
                log::warn("disk cache: couldn't measure part {:02x}: {}", slice, e.what());
                usage.slices[slice] = std::max<int64_t>(usage.slices[slice], 0);
            }
            if(usage.trimming && usage.total() <= target)
            {
                usage.trimming = false;
                log::info("disk cache: trimmed to {} MiB (limit {} MiB)", usage.total() >> 20,
                          limit >> 20);
            }
            if(usage.trimming)
            {
                // Well over (a lot being downloaded, a lower limit): hurry.
                result = removed == 0                         ? Step::Idle
                         : usage.total() > limit + limit / 10 ? Step::Hurry
                                                              : Step::More;
            }
            else if(usage.firstUnknown() < usage.slices.size())
            {
                result = usage.slices[slice] > 0 ? Step::More : Step::Idle;
            }
        }
    }
    writeUsage(file, usage);
    return result;
}

std::optional<uint64_t> FileCache::visitSlice(unsigned slice, std::optional<uint64_t> target,
                                              uint64_t others, uint64_t& removed,
                                              std::stop_token stop)
{
    struct File
    {
        fs::file_time_type used;
        uint64_t size;
        fs::path path;
    };
    const std::string prefix = std::format("{:02x}", slice);
    const auto now = FileClock::now();
    std::vector<File> files;
    uint64_t total = 0;
    for(const char* directory : kDirectories)
    {
        const fs::path shard = root_ / directory / prefix;
        std::error_code ec;
        if(fs::is_symlink(shard, ec))
        {
            continue;
        }
        forEachIn(shard,
                  [&](const fs::directory_entry& entry)
                  {
                      const std::string name = entry.path().filename().string();
                      std::error_code error;
                      if(!isEntryName(prefix, name) || entry.is_symlink(error) ||
                         !entry.is_regular_file(error))
                      {
                          return !stop.stop_requested();
                      }
                      const auto used = fs::last_write_time(entry.path(), error);
                      const uint64_t size = error ? 0 : fs::file_size(entry.path(), error);
                      if(error)
                      {
                          return !stop.stop_requested(); // gone meanwhile
                      }
                      if(!name.ends_with(".tmp"))
                      {
                          total += sizeOnDisk(size);
                          files.push_back({used, sizeOnDisk(size), entry.path()});
                      }
                      // Being written, unless it's old.
                      else if(now - used >= kAbandonedAfter)
                      {
                          fs::remove(entry.path(), error);
                      }
                      return !stop.stop_requested();
                  });
    }
    if(stop.stop_requested())
    {
        return std::nullopt;
    }
    if(!target || others + total <= *target)
    {
        return total;
    }

    // The slice's oldest quarter at most, so the next slices give up theirs
    // rather than this one its recent entries; or its share of what's over,
    // if that's more.
    const uint64_t all = others + total;
    const uint64_t over = all - *target;
    const auto share = static_cast<uint64_t>(
        static_cast<double>(total) * (static_cast<double>(over) / static_cast<double>(all)));
    const uint64_t quota = std::min(over, std::max((total + 3) / 4, share));
    std::ranges::sort(files, {}, &File::used);
    for(const File& f : files)
    {
        if(removed >= quota || stop.stop_requested())
        {
            break;
        }
        // Used (or replaced) since it was measured, by any viewer: keep it.
        // One replaced between this and removing it is only missing from the
        // cache, to be downloaded again.
        std::error_code ec;
        const auto used = fs::last_write_time(f.path, ec);
        if(!ec && used != f.used)
        {
            continue;
        }
        fs::remove(f.path, ec);
        if(!ec)
        {
            removed += f.size; // or it had gone already
        }
    }
    return total - removed;
}

std::string FileCache::describe() const
{
    return root_.string();
}

} // namespace viewer
