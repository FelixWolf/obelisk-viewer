#include "viewer/text/GlyphAtlas.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace viewer::text
{

namespace
{

// Empty pixels around each glyph, so filtering doesn't bleed neighbors in.
constexpr uint32_t kPadding = 1;

} // namespace

size_t GlyphAtlas::KeyHash::operator()(const Key& k) const
{
    size_t h = std::hash<const void*>()(k.font);
    h ^= std::hash<int>()(k.size) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<int>()(k.glyph) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
}

GlyphAtlas::GlyphAtlas(uint32_t size) : size_(size), pixels_(static_cast<size_t>(size) * size, 0)
{
    clear();
}

void GlyphAtlas::clear()
{
    glyphs_.clear();
    std::fill(pixels_.begin(), pixels_.end(), uint8_t{0});
    shelfY_ = kPadding;
    shelfHeight_ = 0;
    shelfX_ = kPadding;
    changedBegin_ = 0;
    changedEnd_ = size_;
}

const Glyph* GlyphAtlas::glyph(const Font& font, float pixelSize, int glyphIndex)
{
    const Key key{&font, static_cast<int>(std::lround(pixelSize * 64.0f)), glyphIndex};
    if(const auto it = glyphs_.find(key); it != glyphs_.end())
    {
        return &it->second;
    }
    const Font::Bitmap bitmap = font.rasterize(glyphIndex, static_cast<float>(key.size) / 64.0f);
    Glyph glyph;
    glyph.offset = glm::vec2(static_cast<float>(bitmap.left), static_cast<float>(bitmap.top));
    if(bitmap.width > 0 && bitmap.height > 0)
    {
        const auto width = static_cast<uint32_t>(bitmap.width);
        const auto height = static_cast<uint32_t>(bitmap.height);
        if(width + 2 * kPadding > size_ || height + 2 * kPadding > size_)
        {
            return nullptr;
        }
        if(shelfX_ + width + kPadding > size_)
        {
            // The next shelf.
            shelfY_ += shelfHeight_ + kPadding;
            shelfHeight_ = 0;
            shelfX_ = kPadding;
        }
        if(shelfY_ + height + kPadding > size_)
        {
            return nullptr; // full
        }
        for(uint32_t y = 0; y < height; y++)
        {
            std::memcpy(&pixels_[(shelfY_ + y) * size_ + shelfX_], &bitmap.pixels[y * width],
                        width);
        }
        const float scale = 1.0f / static_cast<float>(size_);
        glyph.size = glm::vec2(static_cast<float>(width), static_cast<float>(height));
        glyph.uv0 = glm::vec2(static_cast<float>(shelfX_), static_cast<float>(shelfY_)) * scale;
        glyph.uv1 = glyph.uv0 + glyph.size * scale;
        if(changedBegin_ >= changedEnd_)
        {
            changedBegin_ = shelfY_;
            changedEnd_ = shelfY_ + height;
        }
        else
        {
            changedBegin_ = std::min(changedBegin_, shelfY_);
            changedEnd_ = std::max(changedEnd_, shelfY_ + height);
        }
        shelfX_ += width + kPadding;
        shelfHeight_ = std::max(shelfHeight_, height);
    }
    return &glyphs_.emplace(key, glyph).first->second;
}

std::optional<GlyphAtlas::Rows> GlyphAtlas::takeChanged()
{
    if(changedBegin_ >= changedEnd_)
    {
        return std::nullopt;
    }
    const Rows rows{changedBegin_, changedEnd_ - changedBegin_};
    changedBegin_ = changedEnd_ = 0;
    return rows;
}

} // namespace viewer::text
