#include "viewer/core/Uuid.hpp"

#include <algorithm>
#include <random>

namespace viewer
{
namespace
{

int hexValue(char c)
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
}

constexpr bool isHyphenPosition(size_t i)
{
    return i == 8 || i == 13 || i == 18 || i == 23;
}

} // namespace

std::optional<Uuid> Uuid::parse(std::string_view text)
{
    if(text.size() != 36)
    {
        return std::nullopt;
    }
    Uuid id;
    size_t byte = 0;
    for(size_t i = 0; i < text.size();)
    {
        if(isHyphenPosition(i))
        {
            if(text[i] != '-')
            {
                return std::nullopt;
            }
            i++;
            continue;
        }
        const int hi = hexValue(text[i]);
        const int lo = hexValue(text[i + 1]);
        if(hi < 0 || lo < 0)
        {
            return std::nullopt;
        }
        id.bytes[byte++] = static_cast<uint8_t>((hi << 4) | lo);
        i += 2;
    }
    return id;
}

std::string Uuid::toString() const
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for(size_t i = 0; i < bytes.size(); i++)
    {
        if(i == 4 || i == 6 || i == 8 || i == 10)
        {
            out.push_back('-');
        }
        out.push_back(kHex[bytes[i] >> 4]);
        out.push_back(kHex[bytes[i] & 0xF]);
    }
    return out;
}

bool Uuid::isNull() const
{
    return std::ranges::all_of(bytes, [](uint8_t b) { return b == 0; });
}

Uuid Uuid::random()
{
    thread_local std::mt19937_64 engine{std::random_device{}()};
    Uuid id;
    for(size_t i = 0; i < 16; i += 8)
    {
        const uint64_t bits = engine();
        for(size_t b = 0; b < 8; b++)
        {
            id.bytes[i + b] = static_cast<uint8_t>(bits >> (8 * b));
        }
    }
    id.bytes[6] = static_cast<uint8_t>((id.bytes[6] & 0x0F) | 0x40); // version 4
    id.bytes[8] = static_cast<uint8_t>((id.bytes[8] & 0x3F) | 0x80); // RFC 4122 variant
    return id;
}

} // namespace viewer
