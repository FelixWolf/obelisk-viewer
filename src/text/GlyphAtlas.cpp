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
    return place(key, static_cast<uint32_t>(bitmap.width), static_cast<uint32_t>(bitmap.height),
                 bitmap.pixels.data(),
                 glm::vec2(static_cast<float>(bitmap.left), static_cast<float>(bitmap.top)));
}

const Glyph* GlyphAtlas::disc(float radius)
{
    const Key key{nullptr, static_cast<int>(std::lround(radius * 64.0f)), -1};
    if(const auto it = glyphs_.find(key); it != glyphs_.end())
    {
        return &it->second;
    }
    // An odd number of pixels across, so one is right in the middle.
    const float r = static_cast<float>(key.size) / 64.0f;
    const auto half = static_cast<int>(std::ceil(r));
    const auto side = static_cast<uint32_t>(2 * half + 1);
    std::vector<uint8_t> bitmap(static_cast<size_t>(side) * side);
    constexpr int kSamples = 4; // a side, per pixel
    for(uint32_t y = 0; y < side; y++)
    {
        for(uint32_t x = 0; x < side; x++)
        {
            int inside = 0;
            for(int sy = 0; sy < kSamples; sy++)
            {
                for(int sx = 0; sx < kSamples; sx++)
                {
                    const float px = static_cast<float>(static_cast<int>(x) - half) - 0.5f +
                                     (static_cast<float>(sx) + 0.5f) / kSamples;
                    const float py = static_cast<float>(static_cast<int>(y) - half) - 0.5f +
                                     (static_cast<float>(sy) + 0.5f) / kSamples;
                    inside += px * px + py * py <= r * r;
                }
            }
            bitmap[y * side + x] = static_cast<uint8_t>(inside * 255 / (kSamples * kSamples));
        }
    }
    return place(key, side, side, bitmap.data(), glm::vec2(-static_cast<float>(half) - 0.5f));
}

const Glyph* GlyphAtlas::place(const Key& key, uint32_t width, uint32_t height,
                               const uint8_t* bitmap, glm::vec2 offset)
{
    Glyph glyph;
    glyph.offset = offset;
    if(width > 0 && height > 0)
    {
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
            std::memcpy(&pixels_[(shelfY_ + y) * size_ + shelfX_], &bitmap[y * width], width);
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
