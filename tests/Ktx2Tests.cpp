#include "viewer/image/Ktx2.hpp"
#include "viewer/image/TextureProcessing.hpp"

#include <doctest/doctest.h>

#include <algorithm>

using namespace viewer::image;

namespace
{

TextureData makeTexture(uint32_t width, uint32_t height)
{
    MipLevel base;
    base.width = width;
    base.height = height;
    base.pixels.resize(static_cast<size_t>(width) * height * 4);
    for(size_t i = 0; i < base.pixels.size(); i++)
    {
        base.pixels[i] = static_cast<uint8_t>(i * 7);
    }
    TextureData texture;
    texture.alphaMode = AlphaMode::Mask;
    texture.discardLevel = 2;
    texture.levels = buildMipChain(std::move(base));
    return texture;
}

uint32_t read32(const std::vector<uint8_t>& bytes, size_t at)
{
    return bytes[at] | (bytes[at + 1] << 8) | (bytes[at + 2] << 16) |
           (static_cast<uint32_t>(bytes[at + 3]) << 24);
}

} // namespace

TEST_CASE("KTX2 header fields")
{
    const auto file = writeKtx2(makeTexture(16, 8));
    REQUIRE(file.size() > 80);
    CHECK(file[0] == 0xAB);
    CHECK(file[1] == 'K');
    CHECK(read32(file, 12) == 37); // VK_FORMAT_R8G8B8A8_UNORM
    CHECK(read32(file, 20) == 16); // width
    CHECK(read32(file, 24) == 8);  // height
    CHECK(read32(file, 40) == 5);  // levels: 16x8, 8x4, 4x2, 2x1, 1x1
    CHECK(read32(file, 44) == 2);  // zstd
}

TEST_CASE("KTX2 round-trips pixels and metadata")
{
    const TextureData original = makeTexture(33, 17);

    for(int zstdLevel : {0, 3})
    {
        CAPTURE(zstdLevel);
        const auto file = writeKtx2(original, {.zstdLevel = zstdLevel});
        const auto loaded = readKtx2(file);
        REQUIRE(loaded.has_value());
        CHECK(loaded->alphaMode == AlphaMode::Mask);
        CHECK(loaded->discardLevel == 2);
        REQUIRE(loaded->levels.size() == original.levels.size());
        for(size_t i = 0; i < original.levels.size(); i++)
        {
            CHECK(loaded->levels[i].width == original.levels[i].width);
            CHECK(loaded->levels[i].height == original.levels[i].height);
            CHECK((loaded->levels[i].pixels == original.levels[i].pixels));
        }
    }
}

TEST_CASE("KTX2 maxDimension skips the largest levels")
{
    const auto file = writeKtx2(makeTexture(64, 32));

    const auto loaded = readKtx2(file, {.maxDimension = 16});
    REQUIRE(loaded.has_value());
    CHECK(loaded->width() == 16);
    CHECK(loaded->height() == 8);

    const auto tiny = readKtx2(file, {.maxDimension = 1});
    REQUIRE(tiny.has_value());
    CHECK(tiny->levels.size() == 1);
    CHECK(tiny->width() == 1);
}

TEST_CASE("KTX2 reader rejects corrupt input")
{
    auto file = writeKtx2(makeTexture(8, 8));
    CHECK_FALSE(readKtx2(std::vector<uint8_t>(file.begin(), file.begin() + 40)).has_value());

    auto badMagic = file;
    badMagic[1] = 'X';
    CHECK_FALSE(readKtx2(badMagic).has_value());

    auto truncated = file;
    truncated.resize(file.size() - 4);
    CHECK_FALSE(readKtx2(truncated).has_value());
}

TEST_CASE("KTX2: files from before the alpha classification changed are classified again")
{
    const TextureData original = makeTexture(16, 8); // alpha all over: blend
    auto file = writeKtx2(original, {});
    REQUIRE(readKtx2(file)->alphaMode == AlphaMode::Mask); // as written
    // An old file has no "viewer.alpha" key (rename it, same length).
    const std::string key = "viewer.alpha";
    auto at = std::search(file.begin(), file.end(), key.begin(), key.end());
    REQUIRE(at != file.end());
    *(at + static_cast<ptrdiff_t>(key.size()) - 1) = 'X';
    const auto loaded = readKtx2(file);
    REQUIRE(loaded.has_value());
    CHECK(loaded->alphaMode == AlphaMode::Blend);
}
