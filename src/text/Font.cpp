#include "viewer/text/Font.hpp"

#include "EmbeddedFonts.hpp"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>

#include <cmath>
#include <iterator>

namespace viewer::text
{

struct Font::Impl
{
    std::vector<uint8_t> data; // stb_truetype reads it in place
    stbtt_fontinfo info{};
    int ascent = 0;
    int descent = 0;
    int lineGap = 0;
};

Font::Font() : impl_(std::make_unique<Impl>()) {}

Font::~Font() = default;

std::shared_ptr<const Font> Font::load(std::vector<uint8_t> data)
{
    if(data.empty())
    {
        return nullptr;
    }
    std::shared_ptr<Font> font(new Font());
    Impl& f = *font->impl_;
    f.data = std::move(data);
    const int offset = stbtt_GetFontOffsetForIndex(f.data.data(), 0);
    if(offset < 0 || !stbtt_InitFont(&f.info, f.data.data(), offset))
    {
        return nullptr;
    }
    stbtt_GetFontVMetrics(&f.info, &f.ascent, &f.descent, &f.lineGap);
    return font;
}

std::shared_ptr<const Font> Font::builtin()
{
    return load(std::vector<uint8_t>(std::begin(embedded::kInterRegularTtf),
                                     std::end(embedded::kInterRegularTtf)));
}

Font::Metrics Font::metrics(float pixelSize) const
{
    const float scale = stbtt_ScaleForMappingEmToPixels(&impl_->info, pixelSize);
    return {static_cast<float>(impl_->ascent) * scale, -static_cast<float>(impl_->descent) * scale,
            static_cast<float>(impl_->lineGap) * scale};
}

int Font::glyphIndex(char32_t codepoint) const
{
    return stbtt_FindGlyphIndex(&impl_->info, static_cast<int>(codepoint));
}

float Font::advance(int glyph, float pixelSize) const
{
    int advance = 0;
    int bearing = 0;
    stbtt_GetGlyphHMetrics(&impl_->info, glyph, &advance, &bearing);
    return static_cast<float>(advance) * stbtt_ScaleForMappingEmToPixels(&impl_->info, pixelSize);
}

float Font::kerning(int left, int right, float pixelSize) const
{
    const int kern = stbtt_GetGlyphKernAdvance(&impl_->info, left, right);
    return static_cast<float>(kern) * stbtt_ScaleForMappingEmToPixels(&impl_->info, pixelSize);
}

Font::Bitmap Font::rasterize(int glyph, float pixelSize) const
{
    const float scale = stbtt_ScaleForMappingEmToPixels(&impl_->info, pixelSize);
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    stbtt_GetGlyphBitmapBox(&impl_->info, glyph, scale, scale, &x0, &y0, &x1, &y1);
    Bitmap bitmap;
    bitmap.width = std::max(x1 - x0, 0);
    bitmap.height = std::max(y1 - y0, 0);
    bitmap.left = x0;
    bitmap.top = y0;
    if(bitmap.width == 0 || bitmap.height == 0)
    {
        return bitmap;
    }
    bitmap.pixels.resize(static_cast<size_t>(bitmap.width) * static_cast<size_t>(bitmap.height));
    stbtt_MakeGlyphBitmap(&impl_->info, bitmap.pixels.data(), bitmap.width, bitmap.height,
                          bitmap.width, scale, scale, glyph);
    return bitmap;
}

} // namespace viewer::text
