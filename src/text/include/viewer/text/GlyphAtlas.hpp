#pragma once

// Glyph bitmaps packed into one coverage image (one byte a pixel), for
// drawing text as textured quads. Glyphs are rasterized the first time
// they're asked for, at the size asked for, into rows ("shelves") of the
// image. When it's full nothing more fits until it's cleared, which drops
// every glyph; whoever lays out text then starts over (TextLayout).

#include "viewer/text/Font.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace viewer::text
{

struct Glyph
{
    // The bitmap's size and where its top left goes from the pen on the
    // baseline (pixels, y down); zero sized for blanks.
    glm::vec2 size{0.0f};
    glm::vec2 offset{0.0f};
    // Where it is in the atlas (0..1).
    glm::vec2 uv0{0.0f};
    glm::vec2 uv1{0.0f};
};

class GlyphAtlas
{
public:
    explicit GlyphAtlas(uint32_t size = 1024);

    // A glyph of `font` at `pixelSize` (Font), rasterized into the atlas if
    // it isn't there yet; null if it doesn't fit (clear() and try again).
    const Glyph* glyph(const Font& font, float pixelSize, int glyphIndex);
    // A filled, antialiased circle of `radius` pixels, an odd number of
    // pixels across (its offset goes from its center to its top left; the
    // middle texel is fully covered): quarters of it make rounded corners,
    // its middle fills. Null if it doesn't fit.
    const Glyph* disc(float radius);
    // Drops every glyph.
    void clear();

    uint32_t size() const { return size_; }
    // The image, size() x size(), row by row.
    const std::vector<uint8_t>& pixels() const { return pixels_; }
    // The rows changed since the last call (y, height), if any.
    struct Rows
    {
        uint32_t y = 0;
        uint32_t height = 0;
    };
    std::optional<Rows> takeChanged();

private:
    struct Key
    {
        const Font* font = nullptr;
        int size = 0; // pixel size in 64ths
        int glyph = 0;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash
    {
        size_t operator()(const Key& k) const;
    };
    // Packs a bitmap into the atlas under `key`; null if it doesn't fit.
    const Glyph* place(const Key& key, uint32_t width, uint32_t height, const uint8_t* bitmap,
                       glm::vec2 offset);
    uint32_t size_ = 0;
    std::vector<uint8_t> pixels_;
    std::unordered_map<Key, Glyph, KeyHash> glyphs_;
    // The shelf being filled: its top, height so far, and the next free x.
    uint32_t shelfY_ = 0;
    uint32_t shelfHeight_ = 0;
    uint32_t shelfX_ = 0;
    // Changed rows: [changedBegin_, changedEnd_).
    uint32_t changedBegin_ = 0;
    uint32_t changedEnd_ = 0;
};

} // namespace viewer::text
