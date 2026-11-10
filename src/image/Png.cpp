#include "viewer/image/Png.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <span>
#include <string_view>
#include <vector>

namespace viewer::image
{

namespace
{

uint32_t crc32(std::span<const uint8_t> data, uint32_t crc = 0)
{
    static const auto table = []
    {
        std::array<uint32_t, 256> t{};
        for(uint32_t i = 0; i < 256; i++)
        {
            uint32_t c = i;
            for(int k = 0; k < 8; k++)
            {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for(const uint8_t b : data)
    {
        crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

void putBig32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void writeChunk(std::ofstream& out, std::string_view type, const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> chunk;
    putBig32(chunk, static_cast<uint32_t>(data.size()));
    chunk.insert(chunk.end(), type.begin(), type.end());
    chunk.insert(chunk.end(), data.begin(), data.end());
    putBig32(chunk, crc32(std::span(chunk).subspan(4)));
    out.write(reinterpret_cast<const char*>(chunk.data()),
              static_cast<std::streamsize>(chunk.size()));
}

} // namespace

bool writePng(const std::filesystem::path& path, const MipLevel& rgba)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if(!out)
    {
        return false;
    }
    constexpr uint8_t kSignature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    out.write(reinterpret_cast<const char*>(kSignature), sizeof(kSignature));

    std::vector<uint8_t> header;
    putBig32(header, rgba.width);
    putBig32(header, rgba.height);
    header.insert(header.end(), {8, 6, 0, 0, 0}); // 8-bit RGBA, no interlace
    writeChunk(out, "IHDR", header);

    // Raw scanlines, each prefixed with filter type 0.
    std::vector<uint8_t> raw;
    const size_t rowBytes = static_cast<size_t>(rgba.width) * 4;
    raw.reserve((rowBytes + 1) * rgba.height);
    for(uint32_t y = 0; y < rgba.height; y++)
    {
        raw.push_back(0);
        const auto row = rgba.pixels.begin() + static_cast<ptrdiff_t>(y * rowBytes);
        raw.insert(raw.end(), row, row + static_cast<ptrdiff_t>(rowBytes));
    }

    // zlib stream of stored deflate blocks.
    std::vector<uint8_t> zlib = {0x78, 0x01};
    uint32_t a = 1;
    uint32_t b = 0;
    for(const uint8_t v : raw)
    {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    for(size_t offset = 0; offset < raw.size() || offset == 0;)
    {
        const size_t size = std::min<size_t>(raw.size() - offset, 65535);
        const bool last = offset + size >= raw.size();
        zlib.push_back(last ? 1 : 0);
        zlib.push_back(static_cast<uint8_t>(size));
        zlib.push_back(static_cast<uint8_t>(size >> 8));
        zlib.push_back(static_cast<uint8_t>(~size));
        zlib.push_back(static_cast<uint8_t>(~size >> 8));
        zlib.insert(zlib.end(), raw.begin() + static_cast<ptrdiff_t>(offset),
                    raw.begin() + static_cast<ptrdiff_t>(offset + size));
        offset += size;
        if(last)
        {
            break;
        }
    }
    putBig32(zlib, (b << 16) | a);
    writeChunk(out, "IDAT", zlib);
    writeChunk(out, "IEND", {});
    return static_cast<bool>(out);
}

} // namespace viewer::image
