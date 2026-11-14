#pragma once

// A TrueType font (stb_truetype): metrics, glyph lookup, kerning, and glyph
// bitmaps. Sizes are in pixels per em, as point sizes are (10 points at 96
// dpi: 13.33 pixels), not the height of a line.

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace viewer::text
{

class Font
{
public:
    // Null if the data isn't a font stb_truetype reads.
    static std::shared_ptr<const Font> load(std::vector<uint8_t> data);
    // The built-in font (Inter Regular).
    static std::shared_ptr<const Font> builtin();
    ~Font();

    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

    struct Metrics
    {
        float ascent = 0.0f;  // above the baseline
        float descent = 0.0f; // below it (positive)
        float lineGap = 0.0f;
        float lineHeight() const { return ascent + descent + lineGap; }
    };
    Metrics metrics(float pixelSize) const;
    // The glyph for a code point (0, the missing glyph's box, if it has none).
    int glyphIndex(char32_t codepoint) const;
    float advance(int glyph, float pixelSize) const;
    float kerning(int left, int right, float pixelSize) const;

    // A glyph's coverage, one byte a pixel, with where its top left corner
    // goes from the pen on the baseline (y down).
    struct Bitmap
    {
        int width = 0;
        int height = 0;
        int left = 0;
        int top = 0;
        std::vector<uint8_t> pixels;
    };
    Bitmap rasterize(int glyph, float pixelSize) const;

private:
    Font();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace viewer::text
