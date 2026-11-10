#include "TestUtil.hpp"

#include "viewer/world/TerrainDecoder.hpp"
#include "viewer/world/World.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <format>

using namespace viewer::world;

namespace
{

// Real LayerData captured from the Aditi region Ahern: each file is one byte
// of layer type followed by the packed data.
std::vector<std::vector<uint8_t>> ahernLayers()
{
    std::vector<std::vector<uint8_t>> layers;
    for(int i = 0;; i++)
    {
        auto bytes = viewer::test::readTestFile(std::format("terrain/ahern_{:02}.bin", i));
        if(bytes.empty())
        {
            break;
        }
        layers.push_back(std::move(bytes));
    }
    return layers;
}

} // namespace

TEST_CASE("Terrain: real LayerData decodes into a complete, continuous heightmap")
{
    const auto layers = ahernLayers();
    REQUIRE(layers.size() > 10);

    RegionTerrain terrain;
    constexpr uint32_t kPatches = kRegionWidth / kPatchSize;
    size_t patchCount = 0;
    for(const auto& layer : layers)
    {
        const auto patches = decodeLandLayer(layer[0], std::span(layer).subspan(1));
        INFO((patches ? std::string() : patches.error()));
        REQUIRE(patches.has_value());
        for(const TerrainPatch& patch : *patches)
        {
            REQUIRE(patch.x < kPatches);
            REQUIRE(patch.y < kPatches);
            for(uint32_t y = 0; y < kPatchSize; y++)
            {
                for(uint32_t x = 0; x < kPatchSize; x++)
                {
                    terrain.heights[(patch.y * kPatchSize + y) * kRegionWidth +
                                    patch.x * kPatchSize + x] = patch.heights[y * kPatchSize + x];
                }
            }
            terrain.patchesReceived.set(patch.y * kPatches + patch.x);
            patchCount++;
        }
    }
    CHECK(patchCount >= kPatches * kPatches);
    CHECK(terrain.complete());

    float lowest = 1e9f;
    float highest = -1e9f;
    for(const float h : terrain.heights)
    {
        REQUIRE(std::isfinite(h));
        lowest = std::min(lowest, h);
        highest = std::max(highest, h);
    }
    INFO("heights " << lowest << " .. " << highest);
    CHECK(lowest > -50.0f);
    CHECK(highest < 500.0f);
    CHECK(highest - lowest > 0.5f); // not flat: the decoder did something

    // Seams: steps across patch boundaries should look like steps inside
    // patches. A wrong decoder (bit order, zig-zag, IDCT) breaks this badly.
    float insideMax = 0.0f;
    float boundaryMax = 0.0f;
    auto record = [&](float step, bool acrossBoundary)
    {
        float& target = acrossBoundary ? boundaryMax : insideMax;
        target = std::max(target, step);
    };
    for(uint32_t y = 0; y + 1 < kRegionWidth; y++)
    {
        for(uint32_t x = 0; x + 1 < kRegionWidth; x++)
        {
            record(std::abs(terrain.height(x + 1, y) - terrain.height(x, y)),
                   (x + 1) % kPatchSize == 0);
            record(std::abs(terrain.height(x, y + 1) - terrain.height(x, y)),
                   (y + 1) % kPatchSize == 0);
        }
    }
    INFO("largest step inside patches " << insideMax << ", across boundaries " << boundaryMax);
    CHECK(boundaryMax < insideMax * 2.0f + 0.5f);
}

TEST_CASE("Terrain: malformed and non-land layers are rejected")
{
    const std::vector<uint8_t> empty;
    CHECK_FALSE(decodeLandLayer('L', empty).has_value());
    CHECK_FALSE(decodeLandLayer('7', std::vector<uint8_t>{0, 0, 16, '7', 97}).has_value()); // wind

    auto layer = ahernLayers().at(0);
    layer.resize(layer.size() / 2); // truncated
    CHECK_FALSE(decodeLandLayer(layer[0], std::span(layer).subspan(1)).has_value());
}
