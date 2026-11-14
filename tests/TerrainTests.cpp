#include "TestUtil.hpp"

#include "viewer/world/TerrainDecoder.hpp"
#include "viewer/world/TerrainLayers.hpp"
#include "viewer/world/World.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
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

// Reference values from the official viewer's noise code (noise.cpp), built
// with the Microsoft C runtime's rand() as on Windows.
TEST_CASE("Terrain noise matches the official viewer")
{
    struct Reference
    {
        float x, y, noise, turbulence;
    };
    const Reference references[] = {
        {0.5f, 0.25f, -0.356576264f, -0.409460783f},
        {12.3f, 45.6f, -0.186340228f, -0.0302914977f},
        {51862.3984f, 52067.8984f, -0.0790669918f, -0.0128827393f},
        {11524.7998f, 11570.2002f, 0.277029067f, 0.257352948f},
        {200.75f, 3.5f, 0.513804793f, 0.530683875f},
        {1234.567f, 8901.2344f, -0.271889329f, -0.432364345f},
    };
    for(const Reference& r : references)
    {
        CAPTURE(r.x);
        CAPTURE(r.y);
        CHECK(terrainNoise2(r.x, r.y) == r.noise);
        CHECK(terrainTurbulence2(r.x, r.y) == r.turbulence);
    }
}

TEST_CASE("Terrain layer values follow height, settings and noise")
{
    RegionTerrain region;
    region.handle = static_cast<uint64_t>(1000 * 256) << 32 | (1000 * 256);
    region.hasComposition = true;
    region.composition.startHeight = {10.0f, 10.0f, 10.0f, 10.0f};
    region.composition.heightRange = {40.0f, 40.0f, 40.0f, 40.0f};
    for(float& h : region.heights)
    {
        h = 30.0f;
    }
    // (30 - 10) * 4 / 40 = 2, give or take noise of at most ~(6.5 + 2.25 * 1.5) * 2.
    float lowest = 3.0f;
    float highest = 0.0f;
    for(const float v : terrainLayerValues(region))
    {
        lowest = std::min(lowest, v);
        highest = std::max(highest, v);
    }
    CHECK(lowest >= 0.0f);
    CHECK(highest <= 3.0f);
    CHECK(highest - lowest > 0.1f); // the noise varies it

    for(float& h : region.heights)
    {
        h = 500.0f; // far above every range: always the top layer
    }
    CHECK(terrainLayerValue(region, 17, 200) == 3.0f);
    const float blend = terrainBlendNoise(256000.0, 256000.0);
    CHECK(blend >= 0.0f);
    CHECK(blend <= 1.0f);
}

namespace
{

// Writes bits as the encoder does: most significant first, values wider
// than 8 bits a byte at a time from the low byte.
class BitWriter
{
public:
    void bits(uint32_t value, uint32_t count)
    {
        for(uint32_t byte = 0; count > 0; byte++)
        {
            const uint32_t chunk = std::min<uint32_t>(count, 8);
            const uint32_t b = (value >> (8 * byte)) & 0xFF;
            for(uint32_t i = chunk; i > 0; i--)
            {
                bit((b >> (i - 1)) & 1);
            }
            count -= chunk;
        }
    }
    std::vector<uint8_t> data;

private:
    void bit(uint32_t v)
    {
        if(used_ == 0)
        {
            data.push_back(0);
        }
        if(v)
        {
            data.back() |= static_cast<uint8_t>(0x80u >> used_);
        }
        used_ = (used_ + 1) % 8;
    }
    uint32_t used_ = 0;
};

// A wind layer of two flat patches: x velocity `x`, y velocity `y`.
std::vector<uint8_t> flatWind(float x, float y)
{
    BitWriter w;
    w.bits(16, 16); // stride
    w.bits(16, 8);  // patch size
    w.bits('7', 8);
    for(const float v : {x, y})
    {
        w.bits(0, 8);                           // quantization
        w.bits(std::bit_cast<uint32_t>(v), 32); // offset
        w.bits(0, 16);                          // range: none
        w.bits(0, 10);                          // patch ids
        w.bits(0b10, 2);                        // end of block: all zero
    }
    return w.data;
}

} // namespace

TEST_CASE("Wind: two patches decode to the grid, looked up between its points")
{
    const auto wind = decodeWindLayer('7', flatWind(1.5f, -0.5f));
    REQUIRE(wind.has_value());
    CHECK(wind->x[0] == doctest::Approx(1.5f));
    CHECK(wind->y[255] == doctest::Approx(-0.5f));
    // Twice the grid's (WIND_SCALE_HACK), anywhere in the region and
    // clamped or wrapped outside it; never vertical.
    for(const glm::vec2 p :
        {glm::vec2(10.0f, 200.0f), glm::vec2(255.9f, 0.0f), glm::vec2(-5.0f, 300.0f)})
    {
        const glm::vec3 v = windVelocity(*wind, p, 256.0f);
        CHECK(v.x == doctest::Approx(3.0f));
        CHECK(v.y == doctest::Approx(-1.0f));
        CHECK(v.z == 0.0f);
    }
    // Not wind, or too short.
    CHECK_FALSE(decodeWindLayer('L', flatWind(1.0f, 1.0f)).has_value());
    auto shortLayer = flatWind(1.0f, 1.0f);
    shortLayer.resize(12);
    CHECK_FALSE(decodeWindLayer('7', shortLayer).has_value());
}
