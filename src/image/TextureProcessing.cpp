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
    case AlphaMode::Transparent:
        return "transparent";
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
    // Wholly opaque or wholly transparent first.
    bool allOpaque = true;
    bool allClear = true;
    for(size_t i = 3; i < rgba.pixels.size() && (allOpaque || allClear); i += 4)
    {
        allOpaque = allOpaque && rgba.pixels[i] == 255;
        allClear = allClear && rgba.pixels[i] == 0;
    }
    if(allOpaque)
    {
        return AlphaMode::Opaque;
    }
    if(allClear)
    {
        return AlphaMode::Transparent;
    }

    // A mask, or does it need blending? As the official viewer decides
    // (LLImageGL::analyzeAlphaData): a histogram of alpha in sixteenths,
    // plus that of a 2x2 box-filtered copy (which pulls aliased edges
    // towards the middle, so alpha too fine to cut out isn't), is a mask
    // unless more than a 48th of it is mid-range, or it all sits in one half
    // without reaching that half's extreme (an intentional effect).
    uint64_t sample[16] = {};
    uint64_t total = 0;
    uint64_t length = static_cast<uint64_t>(rgba.width) * rgba.height;
    auto alpha = [&](uint32_t x, uint32_t y)
    {
        return static_cast<uint32_t>(
            rgba.pixels[(static_cast<size_t>(y) * rgba.width + x) * 4 + 3]);
    };
    if(rgba.width >= 2 && rgba.height >= 2)
    {
        for(uint32_t y = 0; y + 1 < rgba.height; y += 2)
        {
            for(uint32_t x = 0; x + 1 < rgba.width; x += 2)
            {
                const uint32_t s1 = alpha(x, y);
                const uint32_t s2 = alpha(x, y + 1);
                const uint32_t s3 = alpha(x + 1, y);
                const uint32_t s4 = alpha(x + 1, y + 1);
                sample[s1 / 16]++;
                sample[s2 / 16]++;
                sample[s3 / 16]++;
                sample[s4 / 16]++;
                const uint32_t sum = s1 + s2 + s3 + s4;
                total += 2 * sum;
                sample[sum / 64] += 4;
            }
        }
        length = static_cast<uint64_t>(rgba.width / 2) * (rgba.height / 2) * 8;
    }
    else
    {
        for(uint32_t y = 0; y < rgba.height; y++)
        {
            for(uint32_t x = 0; x < rgba.width; x++)
            {
                const uint32_t s1 = alpha(x, y);
                total += s1;
                sample[s1 / 16]++;
            }
        }
    }
    uint64_t midrange = 0;
    uint64_t lower = 0;
    uint64_t upper = 0;
    for(int i = 0; i < 16; i++)
    {
        midrange += (i >= 2 && i < 13) ? sample[i] : 0;
        lower += i < 8 ? sample[i] : 0;
        upper += i >= 8 ? sample[i] : 0;
    }
    if(midrange > length / 48 || (lower == length && total != 0) ||
       (upper == length && total != 255 * length))
    {
        return AlphaMode::Blend;
    }
    return AlphaMode::Mask;
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

std::optional<J2kInfo> readJ2kInfo(std::span<const uint8_t> j2k)
{
    auto u16 = [&](size_t at) { return static_cast<uint32_t>(j2k[at]) << 8 | j2k[at + 1]; };
    auto u32 = [&](size_t at) { return u16(at) << 16 | u16(at + 2); };
    // The codestream starts with SOC (FF4F) then SIZ (FF51); a JP2 file
    // wraps it in boxes, so look for that pair.
    size_t start = 0;
    while(start + 4 <= j2k.size() && !(j2k[start] == 0xFF && j2k[start + 1] == 0x4F &&
                                       j2k[start + 2] == 0xFF && j2k[start + 3] == 0x51))
    {
        start++;
    }
    const size_t siz = start + 2;
    if(siz + 22 > j2k.size())
    {
        return std::nullopt;
    }
    J2kInfo info;
    info.width = u32(siz + 6) - u32(siz + 14);   // Xsiz - XOsiz
    info.height = u32(siz + 10) - u32(siz + 18); // Ysiz - YOsiz
    // Main header markers after SIZ, up to the first tile (SOT, FF90): COD
    // has the decomposition levels.
    for(size_t at = siz + 2 + u16(siz + 2); at + 4 <= j2k.size();)
    {
        if(j2k[at] != 0xFF)
        {
            break;
        }
        const uint8_t marker = j2k[at + 1];
        if(marker == 0x90)
        {
            break;
        }
        const uint32_t length = u16(at + 2);
        if(marker == 0x52 && at + 10 <= j2k.size()) // COD
        {
            info.levels = j2k[at + 9];
            break;
        }
        at += 2 + length;
    }
    if(info.width == 0 || info.height == 0)
    {
        return std::nullopt;
    }
    return info;
}

uint32_t discardLevelFor(const J2kInfo& info, uint32_t wanted)
{
    if(wanted == 0)
    {
        return 0;
    }
    const uint32_t side = std::max(info.width, info.height);
    uint32_t discard = 0;
    while(discard < info.levels && (side >> (discard + 1)) >= wanted)
    {
        discard++;
    }
    return discard;
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
    texture.fullWidth = base.width << discardLevel;
    texture.fullHeight = base.height << discardLevel;
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
