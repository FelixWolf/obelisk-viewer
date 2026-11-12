#include "TestUtil.hpp"

#include "viewer/core/DiskCache.hpp"

#include <doctest/doctest.h>

#include <filesystem>
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
