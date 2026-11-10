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

} // namespace viewer::image
