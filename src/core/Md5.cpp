#include "viewer/core/Md5.hpp"

#include <bit>
#include <cmath>
#include <cstring>

namespace viewer
{

namespace
{

constexpr std::array<uint32_t, 64> kShifts = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, //
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, //
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, //
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

// K[i] = floor(|sin(i + 1)| * 2^32), per RFC 1321. Doubles compute these
// exactly; building the table once avoids transcribing 64 constants.
const std::array<uint32_t, 64>& sineTable()
{
    static const std::array<uint32_t, 64> table = []
    {
        std::array<uint32_t, 64> t{};
        for(size_t i = 0; i < t.size(); i++)
        {
            t[i] = static_cast<uint32_t>(
                std::floor(std::fabs(std::sin(static_cast<double>(i + 1))) * 4294967296.0));
        }
        return t;
    }();
    return table;
}

void processBlock(std::array<uint32_t, 4>& state, const uint8_t* block)
{
    const auto& k = sineTable();
    uint32_t m[16];
    for(size_t i = 0; i < 16; i++)
    {
        m[i] = static_cast<uint32_t>(block[i * 4]) | static_cast<uint32_t>(block[i * 4 + 1]) << 8 |
               static_cast<uint32_t>(block[i * 4 + 2]) << 16 |
               static_cast<uint32_t>(block[i * 4 + 3]) << 24;
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    for(uint32_t i = 0; i < 64; i++)
    {
        uint32_t f = 0;
        uint32_t g = 0;
        if(i < 16)
        {
            f = (b & c) | (~b & d);
            g = i;
        }
        else if(i < 32)
        {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        }
        else if(i < 48)
        {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        }
        else
        {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        f = f + a + k[i] + m[g];
        a = d;
        d = c;
        c = b;
        b = b + std::rotl(f, static_cast<int>(kShifts[i]));
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

} // namespace

Md5Digest md5(std::span<const uint8_t> data)
{
    std::array<uint32_t, 4> state = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};

    const size_t fullBlocks = data.size() / 64;
    for(size_t i = 0; i < fullBlocks; i++)
    {
        processBlock(state, data.data() + i * 64);
    }

    // Padding: 0x80, zeros, then the bit length as a little-endian uint64.
    uint8_t tail[128] = {};
    const size_t remaining = data.size() % 64;
    if(remaining > 0)
    {
        std::memcpy(tail, data.data() + fullBlocks * 64, remaining);
    }
    tail[remaining] = 0x80;
    const size_t tailSize = remaining < 56 ? 64 : 128;
    const uint64_t bitLength = static_cast<uint64_t>(data.size()) * 8;
    for(size_t i = 0; i < 8; i++)
    {
        tail[tailSize - 8 + i] = static_cast<uint8_t>(bitLength >> (8 * i));
    }
    for(size_t offset = 0; offset < tailSize; offset += 64)
    {
        processBlock(state, tail + offset);
    }

    Md5Digest digest{};
    for(size_t i = 0; i < 16; i++)
    {
        digest[i] = static_cast<uint8_t>(state[i / 4] >> (8 * (i % 4)));
    }
    return digest;
}

Md5Digest md5(std::string_view text)
{
    return md5(std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

std::string toHex(std::span<const uint8_t> bytes)
{
    constexpr const char* kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for(const uint8_t b : bytes)
    {
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 0x0f]);
    }
    return out;
}

} // namespace viewer
