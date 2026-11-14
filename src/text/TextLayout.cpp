#include "viewer/text/TextLayout.hpp"

#include <algorithm>
#include <cmath>

namespace viewer::text
{

namespace
{

constexpr char32_t kReplacement = 0xFFFD;

bool isBreak(char32_t c)
{
    return c == U'\n' || c == U'\r';
}

bool isSpace(char32_t c)
{
    return c == U' ' || c == U'\t' || c == 0x3000;
}

// One glyph of a line, before alignment.
struct Item
{
    int glyph = 0;
    float x = 0.0f; // pen position, from the line's start
    float advance = 0.0f;
    bool space = false;
};

} // namespace

std::u32string decodeUtf8(std::string_view utf8)
{
    std::u32string out;
    out.reserve(utf8.size());
    size_t i = 0;
    while(i < utf8.size())
    {
        const auto b = static_cast<uint8_t>(utf8[i]);
        int extra = 0;
        char32_t c = 0;
        char32_t least = 0;
        if(b < 0x80)
        {
            out.push_back(b);
            i++;
            continue;
        }
        if((b & 0xE0) == 0xC0)
        {
            extra = 1;
            c = b & 0x1F;
            least = 0x80;
        }
        else if((b & 0xF0) == 0xE0)
        {
            extra = 2;
            c = b & 0x0F;
            least = 0x800;
        }
        else if((b & 0xF8) == 0xF0)
        {
            extra = 3;
            c = b & 0x07;
            least = 0x10000;
        }
        else
        {
            out.push_back(kReplacement);
            i++;
            continue;
        }
        size_t j = i + 1;
        bool ok = true;
        for(int k = 0; k < extra; k++, j++)
        {
            if(j >= utf8.size() || (static_cast<uint8_t>(utf8[j]) & 0xC0) != 0x80)
            {
                ok = false;
                break;
            }
            c = (c << 6) | (static_cast<uint8_t>(utf8[j]) & 0x3F);
        }
        if(!ok || c < least || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
        {
            out.push_back(kReplacement);
            i = ok ? j : std::max(j, i + 1);
            continue;
        }
        out.push_back(c);
        i = j;
    }
    return out;
}

TextLayout layoutText(GlyphAtlas& atlas, const Font& font, std::string_view utf8,
                      const LayoutOptions& options)
{
    TextLayout layout;
    const float size = options.pixelSize;
    const Font::Metrics metrics = font.metrics(size);
    layout.lineHeight = std::round(metrics.lineHeight());
    layout.ascent = std::round(metrics.ascent);
    const std::u32string text = decodeUtf8(utf8);
    if(text.empty())
    {
        return layout;
    }

    // Lines of glyphs, wrapped. A break at the very end starts no line.
    std::vector<std::vector<Item>> lines;
    size_t start = 0;
    while(start < text.size())
    {
        size_t end = start;
        while(end < text.size() && !isBreak(text[end]))
        {
            end++;
        }
        if(end > start || !options.skipEmptyLines)
        {
            std::vector<Item> line;
            int previous = -1;
            float pen = 0.0f;
            for(size_t i = start; i < end; i++)
            {
                const char32_t c = text[i];
                if(c < 0x20 && c != U'\t')
                {
                    continue; // other control characters
                }
                Item item;
                item.space = isSpace(c);
                item.glyph = font.glyphIndex(c == U'\t' ? U' ' : c);
                if(previous >= 0)
                {
                    pen += font.kerning(previous, item.glyph, size);
                }
                item.advance = font.advance(item.glyph, size);
                if(pen + item.advance > options.maxWidth && !line.empty() && !item.space)
                {
                    // Wrap: after the line's last space if it has one (that
                    // space and any before it go), else here.
                    size_t cut = line.size();
                    for(size_t k = line.size(); k > 0; k--)
                    {
                        if(line[k - 1].space)
                        {
                            cut = k;
                            break;
                        }
                    }
                    std::vector<Item> rest(line.begin() + static_cast<ptrdiff_t>(cut), line.end());
                    line.resize(cut);
                    while(!line.empty() && line.back().space)
                    {
                        line.pop_back();
                    }
                    if(line.empty())
                    {
                        line = std::move(rest);
                        rest.clear();
                    }
                    lines.push_back(std::move(line));
                    line = std::move(rest);
                    const float shift = line.empty() ? 0.0f : line.front().x;
                    for(Item& it : line)
                    {
                        it.x -= shift;
                    }
                    pen = line.empty() ? 0.0f : line.back().x + line.back().advance;
                    if(!line.empty())
                    {
                        pen += font.kerning(line.back().glyph, item.glyph, size);
                    }
                }
                if(item.space && line.empty() && !lines.empty() && i > start)
                {
                    previous = -1; // spaces a wrap left at a line's start
                    continue;
                }
                item.x = pen;
                pen += item.advance;
                line.push_back(item);
                previous = item.glyph;
            }
            lines.push_back(std::move(line));
        }
        if(end >= text.size())
        {
            break;
        }
        // "\r\n" is one break.
        start =
            end + ((text[end] == U'\r' && end + 1 < text.size() && text[end + 1] == U'\n') ? 2 : 1);
    }
    if(options.skipEmptyLines)
    {
        std::erase_if(lines, [](const std::vector<Item>& l) { return l.empty(); });
    }

    // Widths (without trailing spaces), then glyphs placed and aligned.
    std::vector<float> widths;
    widths.reserve(lines.size());
    float widest = 0.0f;
    for(const auto& line : lines)
    {
        float width = 0.0f;
        for(const Item& item : line)
        {
            if(!item.space)
            {
                width = item.x + item.advance;
            }
        }
        width = std::ceil(width);
        widths.push_back(width);
        widest = std::max(widest, width);
    }
    layout.lines = lines.size();
    layout.size = glm::vec2(widest, layout.lineHeight * static_cast<float>(lines.size()));
    for(size_t l = 0; l < lines.size(); l++)
    {
        float x0 = 0.0f;
        if(options.align == Align::Center)
        {
            x0 = std::floor((widest - widths[l]) * 0.5f);
        }
        else if(options.align == Align::Right)
        {
            x0 = widest - widths[l];
        }
        const float baseline = layout.ascent + layout.lineHeight * static_cast<float>(l);
        for(const Item& item : lines[l])
        {
            const Glyph* glyph = atlas.glyph(font, size, item.glyph);
            if(!glyph)
            {
                layout.complete = false;
                continue;
            }
            if(glyph->size.x <= 0.0f)
            {
                continue;
            }
            PlacedGlyph placed;
            placed.position =
                glm::vec2(std::round(x0 + item.x) + glyph->offset.x, baseline + glyph->offset.y);
            placed.size = glyph->size;
            placed.uv0 = glyph->uv0;
            placed.uv1 = glyph->uv1;
            layout.glyphs.push_back(placed);
        }
    }
    return layout;
}

} // namespace viewer::text
