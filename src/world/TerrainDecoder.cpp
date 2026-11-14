#include "viewer/world/TerrainDecoder.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <optional>

namespace viewer::world
{

namespace
{

constexpr uint32_t kEndOfPatches = 97;
constexpr uint32_t kCount = kPatchSize * kPatchSize;
constexpr float kOneOverSqrt2 = 0.7071067811865475f;

// Bits are read most significant first. Values wider than 8 bits are
// assembled a byte at a time into a little-endian integer: the first 8 bits
// read become the low byte. (That's how the encoder packs them.)
class BitReader
{
public:
    explicit BitReader(std::span<const uint8_t> data) : data_(data) {}

    bool failed() const { return failed_; }

    uint32_t bits(uint32_t count)
    {
        uint8_t out[4] = {};
        size_t outByte = 0;
        uint32_t outBit = 0;
        while(count > 0)
        {
            uint32_t chunk = std::min<uint32_t>(count, 8);
            count -= chunk;
            while(chunk > 0)
            {
                if(byte_ >= data_.size())
                {
                    failed_ = true;
                    return 0;
                }
                out[outByte] = static_cast<uint8_t>(out[outByte] << 1);
                if(data_[byte_] & (0x80u >> bit_))
                {
                    out[outByte]++;
                }
                bit_++;
                chunk--;
                outBit++;
                if(bit_ >= 8)
                {
                    bit_ = 0;
                    byte_++;
                }
                if(outBit >= 8)
                {
                    outBit = 0;
                    outByte++;
                }
            }
        }
        return static_cast<uint32_t>(out[0]) | static_cast<uint32_t>(out[1]) << 8 |
               static_cast<uint32_t>(out[2]) << 16 | static_cast<uint32_t>(out[3]) << 24;
    }

    float f32() { return std::bit_cast<float>(bits(32)); }

private:
    std::span<const uint8_t> data_;
    size_t byte_ = 0;
    uint32_t bit_ = 0;
    bool failed_ = false;
};

struct Tables
{
    std::array<float, kCount> dequantize{};
    std::array<uint32_t, kCount> zigzag{}; // matrix position -> stream index
    std::array<float, kCount> cosines{};

    Tables()
    {
        for(uint32_t j = 0; j < kPatchSize; j++)
        {
            for(uint32_t i = 0; i < kPatchSize; i++)
            {
                dequantize[j * kPatchSize + i] = 1.0f + 2.0f * static_cast<float>(i + j);
            }
        }

        bool diagonal = false;
        bool right = true;
        uint32_t i = 0;
        uint32_t j = 0;
        uint32_t count = 0;
        while(i < kPatchSize && j < kPatchSize)
        {
            zigzag[j * kPatchSize + i] = count++;
            if(!diagonal)
            {
                if(right)
                {
                    if(i < kPatchSize - 1)
                    {
                        i++;
                    }
                    else
                    {
                        j++;
                    }
                    right = false;
                    diagonal = true;
                }
                else
                {
                    if(j < kPatchSize - 1)
                    {
                        j++;
                    }
                    else
                    {
                        i++;
                    }
                    right = true;
                    diagonal = true;
                }
            }
            else
            {
                if(right)
                {
                    i++;
                    j--;
                    if(i == kPatchSize - 1 || j == 0)
                    {
                        diagonal = false;
                    }
                }
                else
                {
                    i--;
                    j++;
                    if(j == kPatchSize - 1 || i == 0)
                    {
                        diagonal = false;
                    }
                }
            }
        }

        const float halfPiOverSize = 3.14159265358979f * 0.5f / static_cast<float>(kPatchSize);
        for(uint32_t u = 0; u < kPatchSize; u++)
        {
            for(uint32_t n = 0; n < kPatchSize; n++)
            {
                cosines[u * kPatchSize + n] = std::cos((2.0f * static_cast<float>(n) + 1.0f) *
                                                       static_cast<float>(u) * halfPiOverSize);
            }
        }
    }
};

const Tables& tables()
{
    static const Tables t;
    return t;
}

void inverseDct(std::array<float, kCount>& block)
{
    const auto& cosines = tables().cosines;
    std::array<float, kCount> temp{};
    // Columns.
    for(uint32_t column = 0; column < kPatchSize; column++)
    {
        for(uint32_t n = 0; n < kPatchSize; n++)
        {
            float total = kOneOverSqrt2 * block[column];
            for(uint32_t u = 1; u < kPatchSize; u++)
            {
                total += block[u * kPatchSize + column] * cosines[u * kPatchSize + n];
            }
            temp[kPatchSize * n + column] = total;
        }
    }
    // Rows.
    constexpr float kScale = 2.0f / static_cast<float>(kPatchSize);
    for(uint32_t line = 0; line < kPatchSize; line++)
    {
        const uint32_t row = line * kPatchSize;
        for(uint32_t n = 0; n < kPatchSize; n++)
        {
            float total = kOneOverSqrt2 * temp[row];
            for(uint32_t u = 1; u < kPatchSize; u++)
            {
                total += temp[row + u] * cosines[u * kPatchSize + n];
            }
            block[row + n] = total * kScale;
        }
    }
}

} // namespace

namespace
{

// The group header (stride, patch size, layer type), checked.
std::expected<void, std::string> readGroupHeader(BitReader& reader)
{
    reader.bits(16); // stride
    const uint32_t patchSize = reader.bits(8);
    reader.bits(8); // layer type
    if(reader.failed() || patchSize != kPatchSize)
    {
        return std::unexpected(std::format("unexpected patch size {}", patchSize));
    }
    return {};
}

// The next patch; nothing at the end-of-patches marker.
std::expected<std::optional<TerrainPatch>, std::string> readPatch(BitReader& reader, bool extended)
{
    const Tables& t = tables();
    const uint32_t quantWBits = reader.bits(8);
    if(reader.failed())
    {
        return std::unexpected("LayerData ended without an end-of-patches marker");
    }
    if(quantWBits == kEndOfPatches)
    {
        return std::nullopt;
    }
    const float dcOffset = reader.f32();
    const uint32_t range = reader.bits(16);
    const uint32_t patchIds = reader.bits(extended ? 32 : 10);
    TerrainPatch patch;
    patch.x = extended ? patchIds >> 16 : patchIds >> 5;
    patch.y = extended ? patchIds & 0xFFFF : patchIds & 0x1F;

    // Coefficients in zig-zag order: 0 = zero, 10 = end of block, 11s =
    // value with sign s.
    const uint32_t wordBits = (quantWBits & 0x0F) + 2;
    std::array<int32_t, kCount> coefficients{};
    for(uint32_t n = 0; n < kCount; n++)
    {
        if(reader.bits(1) == 0)
        {
            continue;
        }
        if(reader.bits(1) == 0)
        {
            break; // the rest are zero
        }
        const bool negative = reader.bits(1) != 0;
        const auto value = static_cast<int32_t>(reader.bits(wordBits));
        coefficients[n] = negative ? -value : value;
    }
    if(reader.failed())
    {
        return std::unexpected("LayerData ended in the middle of a patch");
    }

    // Dequantize, inverse DCT, scale and offset.
    const uint32_t prequant = (quantWBits >> 4) + 2;
    const float quantize = static_cast<float>(1u << prequant);
    const float multiplier = static_cast<float>(range) / quantize;
    const float add = multiplier * static_cast<float>(1u << (prequant - 1)) + dcOffset;
    std::array<float, kCount> block{};
    for(uint32_t i = 0; i < kCount; i++)
    {
        block[i] = static_cast<float>(coefficients[t.zigzag[i]]) * t.dequantize[i];
    }
    inverseDct(block);
    for(uint32_t i = 0; i < kCount; i++)
    {
        patch.heights[i] = block[i] * multiplier + add;
    }
    return patch;
}

} // namespace

std::expected<std::vector<TerrainPatch>, std::string> decodeLandLayer(uint8_t layerType,
                                                                      std::span<const uint8_t> data)
{
    const auto type = static_cast<LayerType>(layerType);
    if(type != LayerType::Land && type != LayerType::LandExtended)
    {
        return std::unexpected(
            std::format("layer type '{}' isn't land", static_cast<char>(layerType)));
    }
    BitReader reader(data);
    if(auto header = readGroupHeader(reader); !header)
    {
        return std::unexpected(header.error());
    }
    std::vector<TerrainPatch> patches;
    while(true)
    {
        auto patch = readPatch(reader, type == LayerType::LandExtended);
        if(!patch)
        {
            return std::unexpected(patch.error());
        }
        if(!*patch)
        {
            break;
        }
        patches.push_back(**patch);
    }
    return patches;
}

std::expected<WindField, std::string> decodeWindLayer(uint8_t layerType,
                                                      std::span<const uint8_t> data)
{
    const auto type = static_cast<LayerType>(layerType);
    if(type != LayerType::Wind && type != LayerType::WindExtended)
    {
        return std::unexpected(
            std::format("layer type '{}' isn't wind", static_cast<char>(layerType)));
    }
    BitReader reader(data);
    if(auto header = readGroupHeader(reader); !header)
    {
        return std::unexpected(header.error());
    }
    WindField wind;
    for(auto* component : {&wind.x, &wind.y})
    {
        auto patch = readPatch(reader, type == LayerType::WindExtended);
        if(!patch)
        {
            return std::unexpected(patch.error());
        }
        if(!*patch)
        {
            return std::unexpected("wind layer with fewer than two patches");
        }
        *component = (*patch)->heights;
    }
    return wind;
}

glm::vec3 windVelocity(const WindField& wind, glm::vec2 position, float width)
{
    constexpr int kSize = static_cast<int>(kPatchSize);
    // Outside: clamped below, wrapped above (as the official viewer).
    for(int c = 0; c < 2; c++)
    {
        position[c] = position[c] < 0.0f ? 0.0f : std::fmod(position[c], width);
    }
    const glm::vec2 grid = position * (static_cast<float>(kSize) / width);
    const int i = std::clamp(static_cast<int>(std::floor(grid.x)), 0, kSize - 1);
    const int j = std::clamp(static_cast<int>(std::floor(grid.y)), 0, kSize - 1);
    const float dx = grid.x - static_cast<float>(i);
    const float dy = grid.y - static_cast<float>(j);
    const size_t k = static_cast<size_t>(i + j * kSize);
    glm::vec2 v;
    if(i < kSize - 1 && j < kSize - 1)
    {
        const auto blend = [&](const std::array<float, kPatchSize * kPatchSize>& a)
        {
            return a[k] * (1.0f - dx) * (1.0f - dy) + a[k + 1] * dx * (1.0f - dy) +
                   a[k + kSize] * dy * (1.0f - dx) + a[k + kSize + 1] * dx * dy;
        };
        v = glm::vec2(blend(wind.x), blend(wind.y));
    }
    else
    {
        v = glm::vec2(wind.x[k], wind.y[k]);
    }
    constexpr float kScaleHack = 2.0f; // WIND_SCALE_HACK
    return glm::vec3(v * kScaleHack, 0.0f);
}

} // namespace viewer::world
