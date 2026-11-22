#include "TestUtil.hpp"

#include "viewer/assets/TextureCache.hpp"

#include <doctest/doctest.h>

using namespace viewer;
using Status = assets::TextureResult::Status;

namespace
{

const Uuid kId = *Uuid::parse("11111111-2222-3333-4444-555555555555");

std::vector<assets::TextureResult> runToCompletion(JobSystem& jobs, assets::TextureCache& cache)
{
    jobs.waitIdle();
    std::vector<assets::TextureResult> results;
    cache.drainResults(results);
    return results;
}

} // namespace

TEST_CASE("TextureCache decodes once, then serves from the KTX2 cache")
{
    test::TempDirectory dir("texture-cache");
    JobSystem jobs(2);
    const auto decoder = image::createJ2kDecoder();
    FileCache store(dir.path());
    assets::TextureCache cache(store, jobs, *decoder);

    int sourceCalls = 0;
    auto source = [&sourceCalls]() -> std::optional<std::vector<uint8_t>>
    {
        sourceCalls++;
        return test::readTestFile("rgba_64x48.j2c");
    };

    CHECK_FALSE(cache.contains(kId));
    REQUIRE(cache.request({.id = kId, .source = source}));
    auto first = runToCompletion(jobs, cache);
    REQUIRE(first.size() == 1);
    CHECK(first[0].status == Status::Decoded);
    CHECK(first[0].texture.width() == 64);
    CHECK(cache.contains(kId));
    CHECK(std::filesystem::is_regular_file(store.pathFor({CacheKind::Texture, kId})));

    REQUIRE(cache.request({.id = kId, .source = source, .maxDimension = 16}));
    auto second = runToCompletion(jobs, cache);
    REQUIRE(second.size() == 1);
    CHECK(second[0].status == Status::LoadedFromCache);
    CHECK(second[0].texture.width() == 16);
    CHECK(second[0].texture.alphaMode == image::AlphaMode::Blend);
    CHECK(sourceCalls == 1);

    // Same pixels whether decoded or read back from disk.
    CHECK((second[0].texture.levels.back().pixels == first[0].texture.levels.back().pixels));
}

TEST_CASE("TextureCache reports missing and undecodable textures")
{
    test::TempDirectory dir("texture-cache-errors");
    JobSystem jobs(1);
    const auto decoder = image::createJ2kDecoder();
    FileCache store(dir.path());
    assets::TextureCache cache(store, jobs, *decoder);

    cache.request({.id = kId});
    auto noSource = runToCompletion(jobs, cache);
    REQUIRE(noSource.size() == 1);
    CHECK(noSource[0].status == Status::NotFound);

    cache.request({.id = kId, .source = [] { return std::optional(std::vector<uint8_t>(32, 1)); }});
    auto garbage = runToCompletion(jobs, cache);
    REQUIRE(garbage.size() == 1);
    CHECK(garbage[0].status == Status::Failed);
    CHECK_FALSE(cache.contains(kId));
}

TEST_CASE("TextureCache can be destroyed with requests still queued")
{
    test::TempDirectory dir("texture-cache-shutdown");
    JobSystem jobs(1);
    const auto decoder = image::createJ2kDecoder();
    {
        FileCache store(dir.path());
        assets::TextureCache cache(store, jobs, *decoder);
        for(int i = 0; i < 50; i++)
        {
            Uuid id = kId;
            id.bytes[15] = static_cast<uint8_t>(i);
            cache.request({.id = id, .source = [] { return test::readTestFile("rgb_40x40.j2c"); }});
        }
    } // must not hang or crash
    jobs.waitIdle();
}

TEST_CASE("TextureCache decodes the size asked for, keeping the J2K to decode bigger later")
{
    test::TempDirectory dir("texture-cache-partial");
    JobSystem jobs(2);
    const auto decoder = image::createJ2kDecoder();
    FileCache store(dir.path());
    assets::TextureCache cache(store, jobs, *decoder);
    const CacheKey source{CacheKind::TextureSource, kId};

    int sourceCalls = 0;
    auto download = [&sourceCalls]() -> std::optional<std::vector<uint8_t>>
    {
        sourceCalls++;
        return test::readTestFile("rgba_64x48.j2c");
    };

    // 16 wanted: decoded at a quarter (discard 2), the J2K kept.
    REQUIRE(cache.request({.id = kId, .source = download, .maxDimension = 16}));
    auto small = runToCompletion(jobs, cache);
    REQUIRE(small.size() == 1);
    CHECK(small[0].status == Status::Decoded);
    CHECK(small[0].texture.width() == 16);
    CHECK(small[0].texture.fullWidth == 64);
    CHECK(store.contains(source));

    // As small or smaller: from the KTX2.
    REQUIRE(cache.request({.id = kId, .maxDimension = 8}));
    auto smaller = runToCompletion(jobs, cache);
    REQUIRE(smaller.size() == 1);
    CHECK(smaller[0].status == Status::LoadedFromCache);
    CHECK(smaller[0].texture.width() == 8);

    // Bigger: decoded from the kept J2K, no download; whole, it goes.
    REQUIRE(cache.request({.id = kId, .source = download}));
    auto whole = runToCompletion(jobs, cache);
    REQUIRE(whole.size() == 1);
    CHECK(whole[0].status == Status::Decoded);
    CHECK(whole[0].texture.width() == 64);
    CHECK(sourceCalls == 1);
    CHECK_FALSE(store.contains(source));

    // Whole now: served from the KTX2 at any size.
    REQUIRE(cache.request({.id = kId}));
    auto again = runToCompletion(jobs, cache);
    REQUIRE(again.size() == 1);
    CHECK(again[0].status == Status::LoadedFromCache);
    CHECK(again[0].texture.width() == 64);
}

TEST_CASE("TextureCache says when a bigger decode needs the J2K downloaded again")
{
    test::TempDirectory dir("texture-cache-needs-source");
    JobSystem jobs(2);
    const auto decoder = image::createJ2kDecoder();
    FileCache store(dir.path());
    assets::TextureCache cache(store, jobs, *decoder);
    auto download = []() -> std::optional<std::vector<uint8_t>>
    { return test::readTestFile("rgba_64x48.j2c"); };

    REQUIRE(cache.request({.id = kId, .source = download, .maxDimension = 16}));
    runToCompletion(jobs, cache);
    store.remove({CacheKind::TextureSource, kId}); // lost (or the cache was trimmed)

    REQUIRE(cache.request({.id = kId, .maxDimension = 32}));
    auto result = runToCompletion(jobs, cache);
    REQUIRE(result.size() == 1);
    CHECK(result[0].status == Status::LoadedFromCache);
    CHECK(result[0].texture.width() == 16); // the best there is
    CHECK(result[0].needsSource);
}

TEST_CASE("TextureCache keeps an avatar bake's morph mask (its fifth component) beside the KTX2")
{
    test::TempDirectory dir("texture-cache-mask");
    JobSystem jobs(2);
    const auto decoder = image::createJ2kDecoder();
    FileCache store(dir.path());
    assets::TextureCache cache(store, jobs, *decoder);
    auto source = []() -> std::optional<std::vector<uint8_t>>
    { return test::readTestFile("bake_mask_16x8.j2c"); };

    // The top four rows are masked (255), the bottom four not; flipped, the
    // bottom rows come first.
    REQUIRE(cache.request({.id = kId, .source = source, .flipRows = true, .wantMask = true}));
    auto first = runToCompletion(jobs, cache);
    REQUIRE(first.size() == 1);
    REQUIRE(first[0].status == Status::Decoded);
    const image::Plane& mask = first[0].texture.aux;
    REQUIRE(mask.width == 16);
    REQUIRE(mask.height == 8);
    CHECK(mask.pixels[0] == 0);                         // the bottom row
    CHECK(mask.pixels[7 * 16 + 3] == 255);              // the top row
    CHECK(first[0].texture.levels[0].pixels[0] == 200); // RGBA as before

    // Read back from the cache with it; without asking, none.
    REQUIRE(cache.request({.id = kId, .flipRows = true, .wantMask = true}));
    auto second = runToCompletion(jobs, cache);
    REQUIRE(second.size() == 1);
    CHECK(second[0].status == Status::LoadedFromCache);
    CHECK_FALSE(second[0].maskUnknown);
    CHECK((second[0].texture.aux.pixels == mask.pixels));
    REQUIRE(cache.request({.id = kId}));
    auto third = runToCompletion(jobs, cache);
    REQUIRE(third.size() == 1);
    CHECK(third[0].texture.aux.empty());

    // Cached before masks were kept: unknown, until decoded again.
    store.remove({CacheKind::TextureMask, kId});
    REQUIRE(cache.request({.id = kId, .wantMask = true}));
    auto fourth = runToCompletion(jobs, cache);
    REQUIRE(fourth.size() == 1);
    CHECK(fourth[0].maskUnknown);
    REQUIRE(cache.request({.id = kId, .source = source, .wantMask = true}));
    auto fifth = runToCompletion(jobs, cache);
    REQUIRE(fifth.size() == 1);
    CHECK(fifth[0].status == Status::Decoded);
    CHECK(fifth[0].texture.aux.width == 16);
}
