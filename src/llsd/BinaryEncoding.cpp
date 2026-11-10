#include "BinaryEncoding.hpp"

#include <array>
#include <cctype>

namespace viewer::llsd::detail
{

namespace
{

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

constexpr char kBase85Alphabet[] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!#$%&()*+-;<=>?@^_`{|}~";

// Reverse lookup table: character -> digit value, or -1.
template <size_t N>
constexpr std::array<int8_t, 256> makeReverse(const char (&alphabet)[N])
{
    std::array<int8_t, 256> table{};
    table.fill(-1);
    for(size_t i = 0; i + 1 < N; i++)
    {
        table[static_cast<unsigned char>(alphabet[i])] = static_cast<int8_t>(i);
    }
    return table;
}

constexpr auto kBase64Reverse = makeReverse(kBase64Alphabet);
constexpr auto kBase85Reverse = makeReverse(kBase85Alphabet);

std::string stripWhitespace(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for(char c : text)
    {
        if(!std::isspace(static_cast<unsigned char>(c)))
        {
            out.push_back(c);
        }
    }
    return out;
}

} // namespace

std::string encodeBase64(const std::vector<uint8_t>& data)
{
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    for(; i + 2 < data.size(); i += 3)
    {
        const uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(kBase64Alphabet[(n >> 18) & 63]);
        out.push_back(kBase64Alphabet[(n >> 12) & 63]);
        out.push_back(kBase64Alphabet[(n >> 6) & 63]);
        out.push_back(kBase64Alphabet[n & 63]);
    }
    const size_t remaining = data.size() - i;
    if(remaining > 0)
    {
        uint32_t n = data[i] << 16;
        if(remaining == 2)
        {
            n |= data[i + 1] << 8;
        }
        out.push_back(kBase64Alphabet[(n >> 18) & 63]);
        out.push_back(kBase64Alphabet[(n >> 12) & 63]);
        out.push_back(remaining == 2 ? kBase64Alphabet[(n >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

std::optional<std::vector<uint8_t>> decodeBase64(std::string_view text)
{
    std::string clean = stripWhitespace(text);
    while(!clean.empty() && clean.back() == '=')
    {
        clean.pop_back();
    }
    if(clean.size() % 4 == 1)
    {
        return std::nullopt;
    }

    std::vector<uint8_t> out;
    out.reserve(clean.size() * 3 / 4);
    uint32_t buffer = 0;
    int bits = 0;
    for(char c : clean)
    {
        const int8_t v = kBase64Reverse[static_cast<unsigned char>(c)];
        if(v < 0)
        {
            return std::nullopt;
        }
        buffer = (buffer << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if(bits >= 8)
        {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

std::optional<std::vector<uint8_t>> decodeBase16(std::string_view text)
{
    const std::string clean = stripWhitespace(text);
    if(clean.size() % 2 != 0)
    {
        return std::nullopt;
    }
    auto nibble = [](char c) -> int
    {
        if(c >= '0' && c <= '9')
        {
            return c - '0';
        }
        if(c >= 'a' && c <= 'f')
        {
            return c - 'a' + 10;
        }
        if(c >= 'A' && c <= 'F')
        {
            return c - 'A' + 10;
        }
        return -1;
    };
    std::vector<uint8_t> out(clean.size() / 2);
    for(size_t i = 0; i < out.size(); i++)
    {
        const int hi = nibble(clean[i * 2]);
        const int lo = nibble(clean[i * 2 + 1]);
        if(hi < 0 || lo < 0)
        {
            return std::nullopt;
        }
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return out;
}

std::optional<std::vector<uint8_t>> decodeBase85(std::string_view text)
{
    std::string clean = stripWhitespace(text);
    // Pad the final group with the highest digit, then drop the extra bytes.
    const size_t padding = (5 - clean.size() % 5) % 5;
    clean.append(padding, '~');

    std::vector<uint8_t> out;
    out.reserve(clean.size() / 5 * 4);
    for(size_t i = 0; i < clean.size(); i += 5)
    {
        uint64_t n = 0;
        for(size_t j = 0; j < 5; j++)
        {
            const int8_t v = kBase85Reverse[static_cast<unsigned char>(clean[i + j])];
            if(v < 0)
            {
                return std::nullopt;
            }
            n = n * 85 + static_cast<uint64_t>(v);
        }
        if(n > 0xFFFFFFFFull)
        {
            return std::nullopt;
        }
        out.push_back(static_cast<uint8_t>(n >> 24));
        out.push_back(static_cast<uint8_t>(n >> 16));
        out.push_back(static_cast<uint8_t>(n >> 8));
        out.push_back(static_cast<uint8_t>(n));
    }
    out.resize(out.size() - padding);
    return out;
}

} // namespace viewer::llsd::detail
