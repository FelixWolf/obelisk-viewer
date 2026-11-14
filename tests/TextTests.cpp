#include "viewer/text/Font.hpp"
#include "viewer/text/GlyphAtlas.hpp"
#include "viewer/text/TextLayout.hpp"

#include <doctest/doctest.h>

#include <algorithm>

using namespace viewer::text;

TEST_CASE("Text: UTF-8 decoding, malformed bytes replaced")
{
    CHECK(decodeUtf8("Hi") == U"Hi");
    CHECK(decodeUtf8("\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80") == U"é€\U0001F600");
    CHECK(decodeUtf8("a\xFF"
                     "b") == U"a�b");
    CHECK(decodeUtf8("\xE2\x82") == U"�");     // cut short
    CHECK(decodeUtf8("\xC0\xAF") == U"�");     // overlong
    CHECK(decodeUtf8("\xED\xA0\x80") == U"�"); // a surrogate
}

TEST_CASE("Text: the built-in font lays out lines, wrapped and centered")
{
    const auto font = Font::builtin();
    REQUIRE(font);
    GlyphAtlas atlas(256);
    LayoutOptions options;
    options.pixelSize = 16.0f;

    const TextLayout one = layoutText(atlas, *font, "Hello", options);
    CHECK(one.complete);
    CHECK(one.lines == 1);
    CHECK(one.glyphs.size() == 5);
    CHECK(one.size.x > 20.0f);
    CHECK(one.size.y == one.lineHeight);
    // Whole pixels, left to right.
    for(size_t i = 1; i < one.glyphs.size(); i++)
    {
        CHECK(one.glyphs[i].position.x > one.glyphs[i - 1].position.x);
        CHECK(one.glyphs[i].position.x == std::round(one.glyphs[i].position.x));
    }

    // Line breaks ("\r\n" being one); empty lines skipped if asked.
    CHECK(layoutText(atlas, *font, "a\r\nb\n\nc\n", options).lines == 4);
    options.skipEmptyLines = true;
    CHECK(layoutText(atlas, *font, "a\r\nb\n\nc\n", options).lines == 3);
    CHECK(layoutText(atlas, *font, "", options).lines == 0);

    // Wrapping at spaces (which go), or within a word too long for a line.
    options.maxWidth = one.size.x + 2.0f;
    const TextLayout words = layoutText(atlas, *font, "Hello Hello Hello", options);
    CHECK(words.lines == 3);
    CHECK(words.glyphs.size() == 15);
    CHECK(words.size.x <= options.maxWidth);
    options.maxWidth = one.size.x * 0.5f;
    const TextLayout broken = layoutText(atlas, *font, "HelloHello", options);
    CHECK(broken.lines >= 3);
    CHECK(broken.glyphs.size() == 10);

    // Centered: a short line's glyphs start further in.
    options.maxWidth = 1000.0f;
    options.align = Align::Center;
    const TextLayout centered = layoutText(atlas, *font, "Hello Hello\nHi", options);
    REQUIRE(centered.lines == 2);
    const auto secondLine =
        std::find_if(centered.glyphs.begin(), centered.glyphs.end(),
                     [&](const PlacedGlyph& g) { return g.position.y > centered.lineHeight; });
    REQUIRE(secondLine != centered.glyphs.end());
    CHECK(secondLine->position.x > centered.glyphs.front().position.x + 10.0f);
}

TEST_CASE("Text: the glyph atlas reuses glyphs, reports changes, and fills up")
{
    const auto font = Font::builtin();
    REQUIRE(font);
    GlyphAtlas atlas(64);
    CHECK(atlas.takeChanged().has_value()); // all of it, new
    CHECK_FALSE(atlas.takeChanged().has_value());
    const int a = font->glyphIndex(U'A');
    CHECK(a != 0);
    const Glyph* first = atlas.glyph(*font, 16.0f, a);
    REQUIRE(first);
    CHECK(first->size.x > 0.0f);
    CHECK(atlas.takeChanged().has_value());
    CHECK(atlas.glyph(*font, 16.0f, a) == first);
    CHECK_FALSE(atlas.takeChanged().has_value());
    // A space has no bitmap.
    const Glyph* space = atlas.glyph(*font, 16.0f, font->glyphIndex(U' '));
    REQUIRE(space);
    CHECK(space->size.x == 0.0f);
    // Big glyphs soon fill a small atlas; clearing makes room again.
    bool full = false;
    for(char32_t c = U'B'; c <= U'Z' && !full; c++)
    {
        full = atlas.glyph(*font, 40.0f, font->glyphIndex(c)) == nullptr;
    }
    CHECK(full);
    atlas.clear();
    CHECK(atlas.glyph(*font, 40.0f, font->glyphIndex(U'Q')) != nullptr);
}
