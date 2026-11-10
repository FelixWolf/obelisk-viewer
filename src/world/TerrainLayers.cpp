#include "viewer/world/TerrainLayers.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace viewer::world
{

namespace
{

constexpr int kTableSize = 256;

// The Microsoft C runtime's rand(): a linear congruential generator with a
// 15-bit output.
class MsvcRandom
{
public:
    explicit MsvcRandom(uint32_t seed) : state_(seed) {}
    int next()
    {
        state_ = state_ * 214013u + 2531011u;
        return static_cast<int>((state_ >> 16) & 0x7FFF);
    }

private:
    uint32_t state_;
};

// Permutation and 2D gradient tables. Built in the same order as the
// official viewer (which also draws 1D and 3D gradients in between, so
// those draws are made and discarded to keep the sequence identical).
struct NoiseTables
{
    std::array<int, kTableSize * 2 + 2> permutation{};
    std::array<std::array<float, 2>, kTableSize * 2 + 2> gradient{};

    NoiseTables()
    {
        MsvcRandom random(42);
        auto component = [&]
        {
            return static_cast<float>((random.next() % (kTableSize * 2)) - kTableSize) /
                   static_cast<float>(kTableSize);
        };
        for(int i = 0; i < kTableSize; i++)
        {
            permutation[static_cast<size_t>(i)] = i;
            (void)component(); // 1D gradient (unused here)
            std::array<float, 2> g = {component(), component()};
            const float scale = 1.0f / std::sqrt(g[0] * g[0] + g[1] * g[1]);
            gradient[static_cast<size_t>(i)] = {g[0] * scale, g[1] * scale};
            (void)component(); // 3D gradient (unused here)
            (void)component();
            (void)component();
        }
        for(int i = kTableSize - 1; i > 0; i--)
        {
            const int j = random.next() % kTableSize;
            std::swap(permutation[static_cast<size_t>(i)], permutation[static_cast<size_t>(j)]);
        }
        for(int i = 0; i < kTableSize + 2; i++)
        {
            permutation[static_cast<size_t>(kTableSize + i)] = permutation[static_cast<size_t>(i)];
            gradient[static_cast<size_t>(kTableSize + i)] = gradient[static_cast<size_t>(i)];
        }
    }
};

const NoiseTables& tables()
{
    static const NoiseTables t;
    return t;
}

// Lattice cell and offsets along one axis. The +4096 keeps the truncation
// positive; cells wrap at 256.
struct Axis
{
    uint8_t cell0;
    uint8_t cell1;
    float offset0;
    float offset1;
};

Axis axis(float v)
{
    const float shifted = v + 4096.0f;
    const int whole = static_cast<int>(shifted);
    Axis a;
    a.cell0 = static_cast<uint8_t>(whole);
    a.cell1 = static_cast<uint8_t>(a.cell0 + 1);
    a.offset0 = shifted - static_cast<float>(whole);
    a.offset1 = a.offset0 - 1.0f;
    return a;
}

float sCurve(float t)
{
    return t * t * (3.0f - 2.0f * t);
}

float lerp(float t, float a, float b)
{
    return a + t * (b - a);
}

// Corner positions of a region (both in meters, global).
double regionOriginX(uint64_t handle)
{
    return static_cast<double>(handle >> 32);
}

double regionOriginY(uint64_t handle)
{
    return static_cast<double>(handle & 0xFFFFFFFFu);
}

// Bilinear over the corners as RegionHandshake numbers them: the first digit
// is x, the second y (00, 01, 10, 11 = our array's 0, 1, 2, 3).
float corners(const std::array<float, 4>& c, float fx, float fy)
{
    return (1.0f - fx) * (1.0f - fy) * c[0] + fx * (1.0f - fy) * c[2] + (1.0f - fx) * fy * c[1] +
           fx * fy * c[3];
}

} // namespace

float terrainNoise2(float x, float y)
{
    const NoiseTables& t = tables();
    const Axis ax = axis(x);
    const Axis ay = axis(y);
    const int i = t.permutation[ax.cell0];
    const int j = t.permutation[ax.cell1];
    const auto& g00 =
        t.gradient[static_cast<size_t>(t.permutation[static_cast<size_t>(i + ay.cell0)])];
    const auto& g10 =
        t.gradient[static_cast<size_t>(t.permutation[static_cast<size_t>(j + ay.cell0)])];
    const auto& g01 =
        t.gradient[static_cast<size_t>(t.permutation[static_cast<size_t>(i + ay.cell1)])];
    const auto& g11 =
        t.gradient[static_cast<size_t>(t.permutation[static_cast<size_t>(j + ay.cell1)])];

    const float sx = sCurve(ax.offset0);
    const float sy = sCurve(ay.offset0);
    const float a = lerp(sx, ax.offset0 * g00[0] + ay.offset0 * g00[1],
                         ax.offset1 * g10[0] + ay.offset0 * g10[1]);
    const float b = lerp(sx, ax.offset0 * g01[0] + ay.offset1 * g01[1],
                         ax.offset1 * g11[0] + ay.offset1 * g11[1]);
    return lerp(sy, a, b);
}

float terrainTurbulence2(float x, float y)
{
    float total = 0.0f;
    for(float frequency = 2.0f; frequency >= 1.0f; frequency *= 0.5f)
    {
        total += terrainNoise2(frequency * x, frequency * y) / frequency;
    }
    return total;
}

float terrainLayerValue(const RegionTerrain& region, uint32_t x, uint32_t y)
{
    // Noise lattice: 4.9215 m horizontally; the low-frequency part is 4.5x
    // coarser. Amplitudes: 6.5 (low) + 1.5^2 (high), all doubled.
    constexpr float kLatticeScale = 1.0f / 4.9215f;
    constexpr float kLowFrequency = 0.2222222222f;
    constexpr float kHighAmplitude = 1.5f * 1.5f;
    constexpr float kMagnitude = 2.0f;

    TerrainComposition settings = region.composition;
    if(!region.hasComposition)
    {
        settings.startHeight = {10.0f, 10.0f, 10.0f, 10.0f};
        settings.heightRange = {60.0f, 60.0f, 60.0f, 60.0f};
    }
    const float fx = static_cast<float>(x) / static_cast<float>(kRegionWidth);
    const float fy = static_cast<float>(y) / static_cast<float>(kRegionWidth);
    const float start = corners(settings.startHeight, fx, fy);
    const float range = corners(settings.heightRange, fx, fy);
    const float height = region.height(x, y);

    const float nx = static_cast<float>(regionOriginX(region.handle) + x) * kLatticeScale;
    const float ny = static_cast<float>(regionOriginY(region.handle) + y) * kLatticeScale;
    float twiddle = terrainNoise2(nx * kLowFrequency, ny * kLowFrequency) * 6.5f;
    twiddle += terrainTurbulence2(nx, ny) * kHighAmplitude;
    twiddle *= kMagnitude;

    const float value = (height + twiddle - start) * 4.0f / range;
    return std::clamp(value, 0.0f, 3.0f);
}

std::vector<float> terrainLayerValues(const RegionTerrain& region)
{
    std::vector<float> values(kRegionWidth * kRegionWidth);
    for(uint32_t y = 0; y < kRegionWidth; y++)
    {
        for(uint32_t x = 0; x < kRegionWidth; x++)
        {
            values[y * kRegionWidth + x] = terrainLayerValue(region, x, y);
        }
    }
    return values;
}

float terrainBlendNoise(double globalX, double globalY)
{
    constexpr float kScale = (1.0f / (4.9215f * 7.0f)) * 0.2222222222f;
    const float x = std::fmod(static_cast<float>(globalX) * kScale, 256.0f);
    const float y = std::fmod(static_cast<float>(globalY) * kScale, 256.0f);
    return std::clamp(terrainNoise2(x, y) * 0.75f + 0.5f, 0.0f, 1.0f);
}

} // namespace viewer::world
