#pragma once

// Lays out UTF-8 text in lines: breaks at line breaks, wraps at word
// boundaries (or within a word too long for a line), kerns, and aligns
// each line. Glyph positions are whole pixels, so text drawn at whole
// pixels stays sharp.

#include "viewer/text/Font.hpp"
#include "viewer/text/GlyphAtlas.hpp"

#include <glm/glm.hpp>

#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace viewer::text
{

// The code points in UTF-8 text; malformed bytes become U+FFFD.
std::u32string decodeUtf8(std::string_view utf8);

enum class Align
{
    Left,
    Center,
    Right,
};

struct LayoutOptions
{
    float pixelSize = 13.333f; // Font: pixels per em (10 points)
    float maxWidth = std::numeric_limits<float>::infinity();
    Align align = Align::Left;
    bool skipEmptyLines = false; // as floating text does
    float lineGap = 0.0f;        // extra pixels between lines
};

struct PlacedGlyph
{
    // Its quad (top left, pixels, y down, from the layout's top left) and
    // where it is in the atlas.
    glm::vec2 position{0.0f};
    glm::vec2 size{0.0f};
    glm::vec2 uv0{0.0f};
    glm::vec2 uv1{0.0f};
};

struct TextLayout
{
    std::vector<PlacedGlyph> glyphs; // visible ones (not blanks)
    size_t lines = 0;
    float lineHeight = 0.0f; // baseline to baseline: the font's, plus the gap
    float ascent = 0.0f;     // the first baseline, from the top
    glm::vec2 size{0.0f};    // the widest line, by every line
    // False if the atlas filled up: clear it and lay out again.
    bool complete = true;
};

TextLayout layoutText(GlyphAtlas& atlas, const Font& font, std::string_view utf8,
                      const LayoutOptions& options = {});

} // namespace viewer::text
