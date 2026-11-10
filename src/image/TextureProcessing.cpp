#include "viewer/image/TextureProcessing.hpp"

#include <algorithm>

namespace viewer::image
{

std::string_view alphaModeName(AlphaMode mode)
{
    switch(mode)
    {
    case AlphaMode::Opaque:
        return "opaque";
    case AlphaMode::Mask:
        return "mask";
    case AlphaMode::Blend:
        return "blend";
    }
    return "opaque";
}

MipLevel toRgba8(const Image& image)
{
    MipLevel out;
    out.width = image.width;
    out.height = image.height;
    const size_t count = static_cast<size_t>(image.width) * image.height;
    out.pixels.resize(count * 4);

    const uint8_t* src = image.pixels.data();
    uint8_t* dst = out.pixels.data();
    switch(image.components)
    {
    case 1:
        for(size_t i = 0; i < count; i++, src += 1, dst += 4)
        {
            dst[0] = dst[1] = dst[2] = src[0];
            dst[3] = 255;
        }
        break;
    case 2:
        for(size_t i = 0; i < count; i++, src += 2, dst += 4)
        {
            dst[0] = dst[1] = dst[2] = src[0];
            dst[3] = src[1];
        }
        break;
    case 3:
        for(size_t i = 0; i < count; i++, src += 3, dst += 4)
        {
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = 255;
        }
        break;
    case 4:
        std::copy_n(src, count * 4, dst);
        break;
    default:
        out = {};
        break;
    }
    return out;
}

AlphaMode classifyAlpha(const MipLevel& rgba)
{
    bool sawPartial = false;
    bool sawNonOpaque = false;
    for(size_t i = 3; i < rgba.pixels.size(); i += 4)
    {
        const uint8_t a = rgba.pixels[i];
        if(a != 255)
        {
            sawNonOpaque = true;
            if(a != 0)
            {
                sawPartial = true;
                break;
            }
        }
    }
    if(sawPartial)
    {
        return AlphaMode::Blend;
    }
    return sawNonOpaque ? AlphaMode::Mask : AlphaMode::Opaque;
}

std::vector<MipLevel> buildMipChain(MipLevel base)
{
    std::vector<MipLevel> levels;
    levels.push_back(std::move(base));

    while(levels.back().width > 1 || levels.back().height > 1)
    {
        const MipLevel& src = levels.back();
        MipLevel dst;
        dst.width = std::max(1u, src.width / 2);
        dst.height = std::max(1u, src.height / 2);
        dst.pixels.resize(static_cast<size_t>(dst.width) * dst.height * 4);

        for(uint32_t y = 0; y < dst.height; y++)
        {
            const uint32_t y0 = std::min(y * 2, src.height - 1);
            const uint32_t y1 = std::min(y * 2 + 1, src.height - 1);
            for(uint32_t x = 0; x < dst.width; x++)
            {
                const uint32_t x0 = std::min(x * 2, src.width - 1);
                const uint32_t x1 = std::min(x * 2 + 1, src.width - 1);
                const uint8_t* p00 = &src.pixels[(static_cast<size_t>(y0) * src.width + x0) * 4];
                const uint8_t* p01 = &src.pixels[(static_cast<size_t>(y0) * src.width + x1) * 4];
                const uint8_t* p10 = &src.pixels[(static_cast<size_t>(y1) * src.width + x0) * 4];
                const uint8_t* p11 = &src.pixels[(static_cast<size_t>(y1) * src.width + x1) * 4];
                uint8_t* out = &dst.pixels[(static_cast<size_t>(y) * dst.width + x) * 4];
                for(int c = 0; c < 4; c++)
                {
                    out[c] = static_cast<uint8_t>((p00[c] + p01[c] + p10[c] + p11[c] + 2) / 4);
                }
            }
        }
        levels.push_back(std::move(dst));
    }
    return levels;
}

std::optional<TextureData> textureFromJ2k(const J2kDecoder& decoder, std::span<const uint8_t> j2k,
                                          uint32_t discardLevel)
{
    Image decoded;
    if(!decoder.decode(j2k, {.discardLevels = discardLevel, .threads = 1}, decoded))
    {
        return std::nullopt;
    }
    MipLevel base = toRgba8(decoded);
    if(base.pixels.empty() || base.width == 0 || base.height == 0)
    {
        return std::nullopt;
    }

    TextureData texture;
    texture.alphaMode = classifyAlpha(base);
    texture.discardLevel = discardLevel;
    texture.levels = buildMipChain(std::move(base));
    return texture;
}

MipLevel resizeRgba8(const MipLevel& source, uint32_t width, uint32_t height)
{
    if(source.width == width && source.height == height)
    {
        return source;
    }
    // Halve with a box filter while that's at least 2x too big, then bilinear.
    MipLevel current = source;
    while(current.width >= width * 2 && current.height >= height * 2)
    {
        current = buildMipChain(current).at(1);
    }
    MipLevel out;
    out.width = width;
    out.height = height;
    out.pixels.resize(static_cast<size_t>(width) * height * 4);
    const float scaleX = static_cast<float>(current.width) / static_cast<float>(width);
    const float scaleY = static_cast<float>(current.height) / static_cast<float>(height);
    auto texel = [&](uint32_t x, uint32_t y, uint32_t c)
    {
        x = std::min(x, current.width - 1);
        y = std::min(y, current.height - 1);
        return static_cast<float>(
            current.pixels[(static_cast<size_t>(y) * current.width + x) * 4 + c]);
    };
    for(uint32_t y = 0; y < height; y++)
    {
        const float sy = std::max(0.0f, (static_cast<float>(y) + 0.5f) * scaleY - 0.5f);
        const auto y0 = static_cast<uint32_t>(sy);
        const float fy = sy - static_cast<float>(y0);
        for(uint32_t x = 0; x < width; x++)
        {
            const float sx = std::max(0.0f, (static_cast<float>(x) + 0.5f) * scaleX - 0.5f);
            const auto x0 = static_cast<uint32_t>(sx);
            const float fx = sx - static_cast<float>(x0);
            for(uint32_t c = 0; c < 4; c++)
            {
                const float top = texel(x0, y0, c) * (1.0f - fx) + texel(x0 + 1, y0, c) * fx;
                const float bottom =
                    texel(x0, y0 + 1, c) * (1.0f - fx) + texel(x0 + 1, y0 + 1, c) * fx;
                out.pixels[(static_cast<size_t>(y) * width + x) * 4 + c] = static_cast<uint8_t>(
                    std::clamp(top * (1.0f - fy) + bottom * fy + 0.5f, 0.0f, 255.0f));
            }
        }
    }
    return out;
}

void flipRows(TextureData& texture)
{
    for(MipLevel& level : texture.levels)
    {
        const size_t row = static_cast<size_t>(level.width) * 4;
        for(uint32_t y = 0; y < level.height / 2; y++)
        {
            std::swap_ranges(level.pixels.begin() + static_cast<ptrdiff_t>(y * row),
                             level.pixels.begin() + static_cast<ptrdiff_t>((y + 1) * row),
                             level.pixels.begin() +
                                 static_cast<ptrdiff_t>((level.height - 1 - y) * row));
        }
    }
}

} // namespace viewer::image
