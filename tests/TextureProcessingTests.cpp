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
