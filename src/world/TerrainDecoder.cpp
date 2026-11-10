#include "viewer/world/TerrainDecoder.hpp"

#include <bit>
#include <cmath>
#include <format>

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

std::expected<std::vector<TerrainPatch>, std::string> decodeLandLayer(uint8_t layerType,
                                                                      std::span<const uint8_t> data)
{
    const auto type = static_cast<LayerType>(layerType);
    if(type != LayerType::Land && type != LayerType::LandExtended)
    {
        return std::unexpected(
            std::format("layer type '{}' isn't land", static_cast<char>(layerType)));
    }
    const bool extended = type == LayerType::LandExtended;

    BitReader reader(data);
    reader.bits(16); // stride
    const uint32_t patchSize = reader.bits(8);
    const uint32_t groupType = reader.bits(8);
    (void)groupType;
    if(reader.failed() || patchSize != kPatchSize)
    {
        return std::unexpected(std::format("unexpected patch size {}", patchSize));
    }

    std::vector<TerrainPatch> patches;
    const Tables& t = tables();
    std::array<int32_t, kCount> coefficients{};
    while(true)
    {
        const uint32_t quantWBits = reader.bits(8);
        if(reader.failed())
        {
            return std::unexpected("LayerData ended without an end-of-patches marker");
        }
        if(quantWBits == kEndOfPatches)
        {
            break;
        }
        const float dcOffset = reader.f32();
        const uint32_t range = reader.bits(16);
        const uint32_t patchIds = reader.bits(extended ? 32 : 10);
        TerrainPatch patch;
        patch.x = extended ? patchIds >> 16 : patchIds >> 5;
        patch.y = extended ? patchIds & 0xFFFF : patchIds & 0x1F;

        // Coefficients in zig-zag order: 0 = zero, 10 = end of block,
        // 11s = value with sign s.
        const uint32_t wordBits = (quantWBits & 0x0F) + 2;
        coefficients.fill(0);
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
        patches.push_back(patch);
    }
    return patches;
}

} // namespace viewer::world
