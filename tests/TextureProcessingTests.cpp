#include "TestUtil.hpp"

#include "viewer/image/TextureProcessing.hpp"

#include <doctest/doctest.h>

using namespace viewer::image;

TEST_CASE("toRgba8 expands 1, 2 and 3 component images")
{
    Image gray{.width = 1, .height = 1, .components = 1, .pixels = {77}};
    CHECK((toRgba8(gray).pixels == std::vector<uint8_t>{77, 77, 77, 255}));

    Image grayAlpha{.width = 1, .height = 1, .components = 2, .pixels = {77, 9}};
    CHECK((toRgba8(grayAlpha).pixels == std::vector<uint8_t>{77, 77, 77, 9}));

    Image rgb{.width = 1, .height = 1, .components = 3, .pixels = {1, 2, 3}};
    CHECK((toRgba8(rgb).pixels == std::vector<uint8_t>{1, 2, 3, 255}));
}

TEST_CASE("classifyAlpha distinguishes opaque, mask and blend")
{
    MipLevel level{.width = 2, .height = 1, .pixels = {0, 0, 0, 255, 0, 0, 0, 255}};
    CHECK(classifyAlpha(level) == AlphaMode::Opaque);

    level.pixels[7] = 0;
    CHECK(classifyAlpha(level) == AlphaMode::Mask);

    level.pixels[3] = 128;
    CHECK(classifyAlpha(level) == AlphaMode::Blend);

    level.pixels[3] = 0;
    CHECK(classifyAlpha(level) == AlphaMode::Transparent);
}

TEST_CASE("classifyAlpha: a cut-out with soft edges is a mask; an even haze isn't")
{
    // 128x128, the left half clear, the right opaque, its edge column soft:
    // few mid-range values (the official viewer's rule: a 48th at most).
    constexpr uint32_t kSide = 128;
    MipLevel level{
        .width = kSide, .height = kSide, .pixels = std::vector<uint8_t>(kSide * kSide * 4, 255)};
    for(uint32_t y = 0; y < kSide; y++)
    {
        for(uint32_t x = 0; x < kSide / 2; x++)
        {
            level.pixels[(y * kSide + x) * 4 + 3] = 0;
        }
        level.pixels[(y * kSide + kSide / 2) * 4 + 3] = 128;
    }
    CHECK(classifyAlpha(level) == AlphaMode::Mask);

    // All of it a little see-through: wanted that way, blended.
    for(size_t i = 3; i < level.pixels.size(); i += 4)
    {
        level.pixels[i] = 200;
    }
    CHECK(classifyAlpha(level) == AlphaMode::Blend);
}

TEST_CASE("buildMipChain halves down to 1x1, including odd sizes")
{
    MipLevel base{.width = 5, .height = 3, .pixels = std::vector<uint8_t>(5 * 3 * 4, 200)};
    const auto levels = buildMipChain(base);
    REQUIRE(levels.size() == 3);
    CHECK(levels[1].width == 2);
    CHECK(levels[1].height == 1);
    CHECK(levels[2].width == 1);
    CHECK(levels[2].height == 1);
    CHECK((levels[2].pixels == std::vector<uint8_t>{200, 200, 200, 200}));
}

TEST_CASE("textureFromJ2k decodes an RGBA fixture with partial alpha")
{
    const auto decoder = createJ2kDecoder();
    const auto j2k = viewer::test::readTestFile("rgba_64x48.j2c");
    REQUIRE_FALSE(j2k.empty());

    const auto texture = textureFromJ2k(*decoder, j2k);
    REQUIRE(texture.has_value());
    CHECK(texture->width() == 64);
    CHECK(texture->height() == 48);
    CHECK(texture->levels.size() == 7); // 64x48 -> 32x24 -> ... -> 1x1
    CHECK(texture->alphaMode == AlphaMode::Blend);

    const auto reduced = textureFromJ2k(*decoder, j2k, 1);
    REQUIRE(reduced.has_value());
    CHECK(reduced->width() == 32);
    CHECK(reduced->discardLevel == 1);
}

TEST_CASE("textureFromJ2k rejects garbage")
{
    const auto decoder = createJ2kDecoder();
    const std::vector<uint8_t> garbage(64, 0x5A);
    CHECK_FALSE(textureFromJ2k(*decoder, garbage).has_value());
}

TEST_CASE("J2K header: size and resolution levels; the discard level for a size")
{
    const auto j2k = viewer::test::readTestFile("rgba_64x48.j2c");
    const auto info = readJ2kInfo(j2k);
    REQUIRE(info.has_value());
    CHECK(info->width == 64);
    CHECK(info->height == 48);
    CHECK(info->levels == 5);
    CHECK(discardLevelFor(*info, 0) == 0); // whole
    CHECK(discardLevelFor(*info, 64) == 0);
    CHECK(discardLevelFor(*info, 32) == 1);
    CHECK(discardLevelFor(*info, 20) == 1); // 32 is the smallest at least 20
    CHECK(discardLevelFor(*info, 1) == 5);  // no further than the levels go
    const std::vector<uint8_t> garbage(64, 0x5A);
    CHECK_FALSE(readJ2kInfo(garbage).has_value());

    // The decode knows the whole image's size.
    const auto decoder = createJ2kDecoder();
    const auto reduced = textureFromJ2k(*decoder, j2k, 2);
    REQUIRE(reduced.has_value());
    CHECK(reduced->width() == 16);
    CHECK(reduced->fullWidth == 64);
    CHECK(reduced->fullHeight == 48);
}
