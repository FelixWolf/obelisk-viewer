#include "viewer/image/Ktx2.hpp"

#include <zstd.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <string>
#include <string_view>

namespace viewer::image
{
namespace
{

constexpr uint8_t kIdentifier[12] = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32,
                                     0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

constexpr uint32_t kVkFormatR8G8B8A8Unorm = 37;
constexpr uint32_t kSupercompressionNone = 0;
constexpr uint32_t kSupercompressionZstd = 2;

constexpr size_t kHeaderSize = 12 + 9 * 4;     // identifier + header fields
constexpr size_t kIndexSize = 4 * 4 + 2 * 8;   // dfd/kvd offsets+lengths, sgd offset+length
constexpr size_t kLevelIndexEntrySize = 3 * 8; // byteOffset, byteLength, uncompressedByteLength

constexpr std::string_view kKeyWriter = "KTXwriter";
constexpr std::string_view kKeyAlphaMode = "viewer.alphaMode";
constexpr std::string_view kKeyDiscardLevel = "viewer.discardLevel";

// --- little-endian helpers ---------------------------------------------------

void put32(std::vector<uint8_t>& out, size_t at, uint32_t v)
{
    for(int i = 0; i < 4; i++)
    {
        out[at + i] = static_cast<uint8_t>(v >> (8 * i));
    }
}

void put64(std::vector<uint8_t>& out, size_t at, uint64_t v)
{
    for(int i = 0; i < 8; i++)
    {
        out[at + i] = static_cast<uint8_t>(v >> (8 * i));
    }
}

void append32(std::vector<uint8_t>& out, uint32_t v)
{
    out.resize(out.size() + 4);
    put32(out, out.size() - 4, v);
}

uint32_t get32(std::span<const uint8_t> in, size_t at)
{
    uint32_t v = 0;
    for(int i = 0; i < 4; i++)
    {
        v |= static_cast<uint32_t>(in[at + i]) << (8 * i);
    }
    return v;
}

uint64_t get64(std::span<const uint8_t> in, size_t at)
{
    uint64_t v = 0;
    for(int i = 0; i < 8; i++)
    {
        v |= static_cast<uint64_t>(in[at + i]) << (8 * i);
    }
    return v;
}

void padTo4(std::vector<uint8_t>& out)
{
    while(out.size() % 4 != 0)
    {
        out.push_back(0);
    }
}

// --- Data Format Descriptor for R8G8B8A8_UNORM ------------------------------

std::vector<uint8_t> buildDfd()
{
    constexpr uint32_t kSampleCount = 4;
    constexpr uint32_t kBlockSize = 24 + 16 * kSampleCount;
    constexpr uint8_t kColorModelRgbsda = 1;
    constexpr uint8_t kPrimariesBt709 = 1;
    constexpr uint8_t kTransferLinear = 1;
    constexpr uint8_t kChannelIds[kSampleCount] = {0, 1, 2, 15}; // R, G, B, A

    std::vector<uint8_t> dfd;
    append32(dfd, 4 + kBlockSize);          // dfdTotalSize
    append32(dfd, 0);                       // vendorId = Khronos, descriptorType = basic
    append32(dfd, 2u | (kBlockSize << 16)); // versionNumber = 2, descriptorBlockSize
    append32(dfd, kColorModelRgbsda | (kPrimariesBt709 << 8) |
                      (kTransferLinear << 16)); // flags = 0 (straight alpha)
    append32(dfd, 0);                           // texelBlockDimension0..3 = 1x1x1x1
    append32(dfd, 4);                           // bytesPlane0 = 4, bytesPlane1..3 = 0
    append32(dfd, 0);                           // bytesPlane4..7 = 0
    for(uint32_t s = 0; s < kSampleCount; s++)
    {
        const uint32_t bitOffset = s * 8;
        const uint32_t bitLengthMinusOne = 7;
        append32(dfd, bitOffset | (bitLengthMinusOne << 16) |
                          (static_cast<uint32_t>(kChannelIds[s]) << 24));
        append32(dfd, 0);   // sample position
        append32(dfd, 0);   // sampleLower
        append32(dfd, 255); // sampleUpper
    }
    return dfd;
}

// --- Key/value data ----------------------------------------------------------

void appendKeyValue(std::vector<uint8_t>& kvd, std::string_view key, std::string_view value)
{
    // keyAndValueByteLength counts key + NUL + value (+ NUL, by convention for strings).
    const uint32_t length = static_cast<uint32_t>(key.size() + 1 + value.size() + 1);
    append32(kvd, length);
    kvd.insert(kvd.end(), key.begin(), key.end());
    kvd.push_back(0);
    kvd.insert(kvd.end(), value.begin(), value.end());
    kvd.push_back(0);
    padTo4(kvd);
}

std::optional<AlphaMode> parseAlphaMode(std::string_view s)
{
    if(s == "opaque")
    {
        return AlphaMode::Opaque;
    }
    if(s == "mask")
    {
        return AlphaMode::Mask;
    }
    if(s == "blend")
    {
        return AlphaMode::Blend;
    }
    return std::nullopt;
}

} // namespace

std::vector<uint8_t> writeKtx2(const TextureData& texture, const Ktx2WriteOptions& options)
{
    const uint32_t levelCount = static_cast<uint32_t>(texture.levels.size());
    const bool zstd = options.zstdLevel > 0;

    const std::vector<uint8_t> dfd = buildDfd();

    // Keys must be sorted by byte value ("KTX..." sorts before "viewer...").
    std::vector<uint8_t> kvd;
    appendKeyValue(kvd, kKeyWriter, "viewer");
    appendKeyValue(kvd, kKeyAlphaMode, alphaModeName(texture.alphaMode));
    appendKeyValue(kvd, kKeyDiscardLevel, std::to_string(texture.discardLevel));

    const size_t levelIndexOffset = kHeaderSize + kIndexSize;
    const size_t dfdOffset = levelIndexOffset + kLevelIndexEntrySize * levelCount;
    const size_t kvdOffset = dfdOffset + dfd.size();
    const size_t dataStart = kvdOffset + kvd.size();

    std::vector<uint8_t> out(dataStart, 0);
    std::memcpy(out.data(), kIdentifier, sizeof(kIdentifier));
    size_t at = 12;
    for(uint32_t v : {kVkFormatR8G8B8A8Unorm, 1u /*typeSize*/, texture.width(), texture.height(),
                      0u /*pixelDepth*/, 0u /*layerCount*/, 1u /*faceCount*/, levelCount,
                      zstd ? kSupercompressionZstd : kSupercompressionNone})
    {
        put32(out, at, v);
        at += 4;
    }
    put32(out, at + 0, static_cast<uint32_t>(dfdOffset));
    put32(out, at + 4, static_cast<uint32_t>(dfd.size()));
    put32(out, at + 8, static_cast<uint32_t>(kvdOffset));
    put32(out, at + 12, static_cast<uint32_t>(kvd.size()));
    put64(out, at + 16, 0); // sgdByteOffset
    put64(out, at + 24, 0); // sgdByteLength

    std::memcpy(out.data() + dfdOffset, dfd.data(), dfd.size());
    std::memcpy(out.data() + kvdOffset, kvd.data(), kvd.size());

    // Level data is stored smallest level first. Uncompressed RGBA8 levels
    // must start on a 4-byte boundary; supercompressed levels need no padding.
    std::vector<uint8_t> scratch;
    for(uint32_t i = levelCount; i-- > 0;)
    {
        const MipLevel& level = texture.levels[i];
        if(!zstd)
        {
            padTo4(out);
        }
        const size_t levelOffset = out.size();
        size_t levelLength = level.byteSize();
        if(zstd)
        {
            scratch.resize(ZSTD_compressBound(level.byteSize()));
            levelLength = ZSTD_compress(scratch.data(), scratch.size(), level.pixels.data(),
                                        level.byteSize(), options.zstdLevel);
            if(ZSTD_isError(levelLength))
            {
                return {};
            }
            out.insert(out.end(), scratch.begin(),
                       scratch.begin() + static_cast<ptrdiff_t>(levelLength));
        }
        else
        {
            out.insert(out.end(), level.pixels.begin(), level.pixels.end());
        }
        const size_t entry = levelIndexOffset + kLevelIndexEntrySize * i;
        put64(out, entry + 0, levelOffset);
        put64(out, entry + 8, levelLength);
        put64(out, entry + 16, level.byteSize());
    }
    return out;
}

std::optional<TextureData> readKtx2(std::span<const uint8_t> file, const Ktx2ReadOptions& options)
{
    if(file.size() < kHeaderSize + kIndexSize ||
       std::memcmp(file.data(), kIdentifier, sizeof(kIdentifier)) != 0)
    {
        return std::nullopt;
    }

    const uint32_t vkFormat = get32(file, 12);
    const uint32_t width = get32(file, 20);
    const uint32_t height = get32(file, 24);
    const uint32_t pixelDepth = get32(file, 28);
    const uint32_t layerCount = get32(file, 32);
    const uint32_t faceCount = get32(file, 36);
    const uint32_t levelCount = std::max(1u, get32(file, 40));
    const uint32_t supercompression = get32(file, 44);
    const uint32_t kvdOffset = get32(file, 56);
    const uint32_t kvdLength = get32(file, 60);

    if(vkFormat != kVkFormatR8G8B8A8Unorm || width == 0 || height == 0 || pixelDepth != 0 ||
       layerCount > 1 || faceCount != 1 || levelCount > 32 ||
       (supercompression != kSupercompressionNone && supercompression != kSupercompressionZstd))
    {
        return std::nullopt;
    }
    const size_t levelIndexOffset = kHeaderSize + kIndexSize;
    if(file.size() < levelIndexOffset + kLevelIndexEntrySize * levelCount ||
       static_cast<size_t>(kvdOffset) + kvdLength > file.size())
    {
        return std::nullopt;
    }

    TextureData texture;

    // Key/value data: our metadata. Unknown keys are ignored.
    for(size_t at = kvdOffset; at + 4 <= static_cast<size_t>(kvdOffset) + kvdLength;)
    {
        const uint32_t length = get32(file, at);
        if(at + 4 + length > static_cast<size_t>(kvdOffset) + kvdLength)
        {
            return std::nullopt;
        }
        const std::string_view entry(reinterpret_cast<const char*>(file.data() + at + 4), length);
        const size_t nul = entry.find('\0');
        if(nul != std::string_view::npos)
        {
            const std::string_view key = entry.substr(0, nul);
            std::string_view value = entry.substr(nul + 1);
            if(!value.empty() && value.back() == '\0')
            {
                value.remove_suffix(1);
            }
            if(key == kKeyAlphaMode)
            {
                texture.alphaMode = parseAlphaMode(value).value_or(AlphaMode::Opaque);
            }
            else if(key == kKeyDiscardLevel)
            {
                std::from_chars(value.data(), value.data() + value.size(), texture.discardLevel);
            }
        }
        at += 4 + length;
        at = (at + 3) & ~size_t{3};
    }

    texture.fullWidth = width << texture.discardLevel;
    texture.fullHeight = height << texture.discardLevel;

    // Choose the first level that fits within maxDimension (always keep the last).
    uint32_t firstLevel = 0;
    if(options.maxDimension > 0)
    {
        while(firstLevel + 1 < levelCount &&
              std::max(std::max(1u, width >> firstLevel), std::max(1u, height >> firstLevel)) >
                  options.maxDimension)
        {
            firstLevel++;
        }
    }

    for(uint32_t i = firstLevel; i < levelCount; i++)
    {
        const size_t entry = levelIndexOffset + kLevelIndexEntrySize * i;
        const uint64_t offset = get64(file, entry + 0);
        const uint64_t length = get64(file, entry + 8);
        const uint64_t uncompressedLength = get64(file, entry + 16);

        MipLevel level;
        level.width = std::max(1u, width >> i);
        level.height = std::max(1u, height >> i);
        const size_t expected = static_cast<size_t>(level.width) * level.height * 4;
        if(offset + length > file.size() ||
           (uncompressedLength != 0 && uncompressedLength != expected))
        {
            return std::nullopt;
        }
        level.pixels.resize(expected);

        const uint8_t* src = file.data() + offset;
        if(supercompression == kSupercompressionZstd)
        {
            const size_t written = ZSTD_decompress(level.pixels.data(), expected, src, length);
            if(ZSTD_isError(written) || written != expected)
            {
                return std::nullopt;
            }
        }
        else
        {
            if(length != expected)
            {
                return std::nullopt;
            }
            std::memcpy(level.pixels.data(), src, expected);
        }
        texture.levels.push_back(std::move(level));
    }
    return texture;
}

} // namespace viewer::image
