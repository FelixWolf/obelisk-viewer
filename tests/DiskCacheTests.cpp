#include "TestUtil.hpp"

#include "viewer/core/DiskCache.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <thread>

using namespace viewer;

namespace
{

const Uuid kId = *Uuid::parse("ab345678-2222-3333-4444-555555555555");

std::vector<uint8_t> bytes(std::initializer_list<uint8_t> values)
{
    return values;
}

} // namespace

TEST_CASE("FileCache stores, replaces and clears entries")
{
    test::TempDirectory dir("file-cache");
    FileCache cache(dir.path());
    const CacheKey texture{CacheKind::Texture, kId};
    const CacheKey header{CacheKind::Mesh, kId, CacheKey::kMeshHeader};
    const CacheKey lod{CacheKind::Mesh, kId, 2};

    CHECK_FALSE(cache.contains(texture));
    CHECK_FALSE(cache.read(texture));

    REQUIRE(cache.write(texture, bytes({1, 2, 3})));
    REQUIRE(cache.write(header, bytes({4})));
    REQUIRE(cache.write(lod, bytes({5, 6})));
    CHECK(cache.contains(texture));
    CHECK(*cache.read(texture) == bytes({1, 2, 3}));
    CHECK(*cache.read(header) == bytes({4}));
    CHECK(*cache.read(lod) == bytes({5, 6}));
    // Parts of one asset are separate entries.
    CHECK_FALSE(cache.contains({CacheKind::Mesh, kId, CacheKey::kMeshSkin}));

    // The layout earlier versions used, so their caches stay valid.
    CHECK(cache.pathFor(texture) == dir.path() / "textures" / "ab" / (kId.toString() + ".ktx2"));
    CHECK(cache.pathFor(lod) == dir.path() / "meshes" / "ab" / (kId.toString() + ".lod2"));
    CHECK(cache.pathFor({CacheKind::Animation, kId}) ==
          dir.path() / "animations" / "ab" / (kId.toString() + ".anim"));
    CHECK(cache.pathFor({CacheKind::Objects, kId}) ==
          dir.path() / "objects" / "ab" / (kId.toString() + ".objects"));

    REQUIRE(cache.write(texture, bytes({9})));
    CHECK(*cache.read(texture) == bytes({9}));

    // No temporary files are left behind.
    size_t files = 0;
    for(const auto& entry : std::filesystem::recursive_directory_iterator(dir.path()))
    {
        files += entry.is_regular_file();
    }
    CHECK(files == 3);

    // Clearing one kind leaves the others.
    cache.clear(CacheKind::Texture);
    CHECK_FALSE(cache.contains(texture));
    CHECK(cache.contains(lod));
}

TEST_CASE("FileCache instances writing the same entry don't mix their bytes")
{
    // Two instances stand in for two viewers sharing a cache.
    test::TempDirectory dir("file-cache-shared");
    FileCache first(dir.path());
    FileCache second(dir.path());
    const CacheKey key{CacheKind::Animation, kId};
    const std::vector<uint8_t> a(100000, 0xAA);
    const std::vector<uint8_t> b(50000, 0xBB);
    std::thread writerA(
        [&]
        {
            for(int i = 0; i < 50; i++)
            {
                first.write(key, a);
            }
        });
    std::thread writerB(
        [&]
        {
            for(int i = 0; i < 50; i++)
            {
                second.write(key, b);
            }
        });
    for(int i = 0; i < 200; i++)
    {
        if(const auto read = first.read(key))
        {
            CHECK((*read == a || *read == b));
        }
    }
    writerA.join();
    writerB.join();
    const auto last = second.read(key);
    REQUIRE(last);
    CHECK((*last == a || *last == b));
}

namespace
{

namespace fs = std::filesystem;

constexpr size_t kEntrySize = 8192;
const std::vector<uint8_t> kEntryBytes(kEntrySize, 0x55);

// `slice`: the id's first byte.
CacheKey numbered(int i, CacheKind kind = CacheKind::Texture, int slice = -1)
{
    return {kind, *Uuid::parse(std::format("{:02x}345678-2222-3333-4444-{:012x}",
                                           slice < 0 ? i % 7 : slice, i))};
}

// The slices' total in the usage file, once all have been measured.
std::optional<uint64_t> recordedSize(const fs::path& root)
{
    std::ifstream in(root / "usage");
    std::string magic;
    int next = 0;
    int trimming = 0;
    in >> magic >> next >> trimming;
    uint64_t total = 0;
    for(int i = 0; i < 256; i++)
    {
        int64_t bytes = -1;
        if(!(in >> bytes) || bytes < 0)
        {
            return std::nullopt;
        }
        total += static_cast<uint64_t>(bytes);
    }
    return trimming ? std::nullopt : std::optional(total);
}

void setAge(const fs::path& path, std::chrono::minutes age)
{
    fs::last_write_time(path, fs::file_time_type::clock::now() - age);
}

uint64_t entriesSize(const fs::path& root)
{
    uint64_t total = 0;
    std::error_code ec;
    for(const char* kind : {"textures", "sounds"})
    {
        fs::recursive_directory_iterator it(root / kind, ec);
        while(!ec && it != fs::recursive_directory_iterator())
        {
            std::error_code gone;
            const uint64_t size = it->is_regular_file(gone) ? it->file_size(gone) : 0;
            total += gone ? 0 : size;
            it.increment(ec);
        }
    }
    return total;
}

template <typename F>
bool eventually(F&& condition)
{
    for(int i = 0; i < 6000; i++)
    {
        if(condition())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

} // namespace

TEST_CASE("FileCache keeps to its limit, removing what was used longest ago")
{
    test::TempDirectory dir("file-cache-limit");
    FileCache cache(dir.path());
    // Ten entries, the first the oldest, of two kinds (one limit for all).
    auto key = [](int i)
    { return numbered(i, i % 2 ? CacheKind::Sound : CacheKind::Texture, 0xab); };
    for(int i = 0; i < 10; i++)
    {
        REQUIRE(cache.write(key(i), kEntryBytes));
        setAge(cache.pathFor(key(i)), std::chrono::minutes(1000 - i));
    }
    // Reading the oldest makes it the newest, here from another viewer.
    FileCache other(dir.path());
    REQUIRE(other.read(key(0)));

    // Not entries: never touched.
    const fs::path strangers[] = {dir.path() / "viewerconfig" / "config.json",
                                  dir.path() / "textures" / "01" / "holiday.jpg",
                                  dir.path() / "mine" / "00" / cache.pathFor(key(0)).filename()};
    for(const fs::path& path : strangers)
    {
        fs::create_directories(path.parent_path());
        std::ofstream(path) << "keep";
        setAge(path, std::chrono::minutes(100000));
    }
    // Left by a viewer that died; and being written by another.
    const fs::path abandoned = cache.pathFor(key(1)).string() + ".00.1.tmp";
    const fs::path writing = cache.pathFor(key(1)).string() + ".00.2.tmp";
    std::ofstream(abandoned) << "x";
    std::ofstream(writing) << "x";
    setAge(abandoned, std::chrono::minutes(120));

    // 90% of 7.5 leaves six (one slice's quarter: three, by rounding, then
    // one more).
    cache.setLimit(7 * kEntrySize + kEntrySize / 2);
    REQUIRE(eventually([&] { return recordedSize(dir.path()).has_value(); }));
    for(int i = 0; i < 10; i++)
    {
        CAPTURE(i);
        CHECK(cache.contains(key(i)) == (i == 0 || i >= 5));
    }
    CHECK_FALSE(fs::exists(abandoned));
    CHECK(fs::exists(writing));
    for(const fs::path& path : strangers)
    {
        CHECK(fs::exists(path));
    }
}

TEST_CASE("FileCache instances sharing a cache keep it under one limit together")
{
    test::TempDirectory dir("file-cache-limit-shared");
    constexpr uint64_t kLimit = 100 * kEntrySize;
    {
        FileCache first(dir.path());
        FileCache second(dir.path());
        first.setLimit(kLimit);
        second.setLimit(kLimit);
        auto writer = [](FileCache& cache, int from)
        {
            for(int i = from; i < from + 300; i++)
            {
                cache.write(numbered(i), kEntryBytes);
                const auto read = cache.read(numbered(i - 5));
                // Whole, or gone.
                if(read && *read != kEntryBytes)
                {
                    FAIL_CHECK("part of an entry");
                }
            }
        };
        std::thread a([&] { writer(first, 10000); });
        std::thread b([&] { writer(second, 20000); });
        a.join();
        b.join();
    }
    // Closing counts the last of it; opening again with the limit trims what
    // was over.
    FileCache third(dir.path());
    third.setLimit(kLimit);
    CHECK(eventually([&] { return entriesSize(dir.path()) <= kLimit; }));
    CHECK(entriesSize(dir.path()) > 0);
}

TEST_CASE("FileCache counts what other viewers wrote, and recovers a damaged total")
{
    test::TempDirectory dir("file-cache-usage");
    const fs::path usage = dir.path() / "usage";
    {
        FileCache cache(dir.path());
        for(int i = 0; i < 10; i++)
        {
            REQUIRE(cache.write(numbered(i), kEntryBytes));
        }
        std::ofstream(usage) << "obelisk-cache-usage-2 0 0 12 ";
        cache.setLimit(20 * kEntrySize);
        // Measured afresh: all ten.
        REQUIRE(eventually([&] { return recordedSize(dir.path()) == 10 * kEntrySize; }));
        REQUIRE(cache.write(numbered(10), kEntryBytes));
    }
    // Another limited viewer adds to the same total until it's over.
    FileCache other(dir.path());
    other.setLimit(20 * kEntrySize);
    for(int i = 11; i < 25; i++)
    {
        REQUIRE(other.write(numbered(i), kEntryBytes));
    }
    CHECK(eventually([&] { return entriesSize(dir.path()) <= 20 * kEntrySize; }));
}

TEST_CASE("FileCache without a limit removes nothing")
{
    test::TempDirectory dir("file-cache-unlimited");
    FileCache cache(dir.path());
    cache.setLimit(0);
    for(int i = 0; i < 50; i++)
    {
        REQUIRE(cache.write(numbered(i), kEntryBytes));
    }
    CHECK(entriesSize(dir.path()) == 50 * kEntrySize);
    CHECK_FALSE(fs::exists(dir.path() / "usage"));
}
