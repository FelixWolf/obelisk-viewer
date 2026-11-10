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
    assets::TextureCache cache(dir.path(), jobs, *decoder);

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
    CHECK((cache.pathFor(kId).extension() == ".ktx2"));

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
    assets::TextureCache cache(dir.path(), jobs, *decoder);

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
        assets::TextureCache cache(dir.path(), jobs, *decoder);
        for(int i = 0; i < 50; i++)
        {
            Uuid id = kId;
            id.bytes[15] = static_cast<uint8_t>(i);
            cache.request({.id = id, .source = [] { return test::readTestFile("rgb_40x40.j2c"); }});
        }
    } // must not hang or crash
    jobs.waitIdle();
}
