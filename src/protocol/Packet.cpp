#include "viewer/protocol/Packet.hpp"

#include <algorithm>

namespace viewer::protocol
{

namespace
{

constexpr size_t kHeaderSize = 6;

void putBig32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

uint32_t getBig32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 |
           static_cast<uint32_t>(p[2]) << 8 | p[3];
}

} // namespace

std::vector<uint8_t> zeroEncode(std::span<const uint8_t> data)
{
    // A run of n zeros becomes 0x00 n (n <= 255).
    std::vector<uint8_t> out;
    out.reserve(data.size());
    size_t i = 0;
    while(i < data.size())
    {
        if(data[i] != 0)
        {
            out.push_back(data[i]);
            i++;
            continue;
        }
        size_t run = 0;
        while(i < data.size() && data[i] == 0 && run < 255)
        {
            run++;
            i++;
        }
        out.push_back(0);
        out.push_back(static_cast<uint8_t>(run));
    }
    return out;
}

std::optional<std::vector<uint8_t>> zeroDecode(std::span<const uint8_t> data, size_t maxSize)
{
    std::vector<uint8_t> out;
    out.reserve(std::min(data.size() * 2, maxSize));
    for(size_t i = 0; i < data.size(); i++)
    {
        if(data[i] != 0)
        {
            out.push_back(data[i]);
        }
        else
        {
            if(i + 1 >= data.size())
            {
                return std::nullopt; // zero without a count
            }
            i++;
            out.insert(out.end(), data[i], 0);
        }
        if(out.size() > maxSize)
        {
            return std::nullopt;
        }
    }
    return out;
}

std::optional<Packet> parsePacket(std::span<const uint8_t> data)
{
    if(data.size() < kHeaderSize)
    {
        return std::nullopt;
    }
    Packet packet;
    packet.flags = data[0];
    packet.sequence = getBig32(&data[1]);
    const size_t extra = data[5];
    size_t messageStart = kHeaderSize + extra;
    size_t messageEnd = data.size();
    if(messageStart > messageEnd)
    {
        return std::nullopt;
    }

    if(packet.flags & PacketFlags::Ack)
    {
        if(messageEnd <= messageStart)
        {
            return std::nullopt;
        }
        const size_t count = data[messageEnd - 1];
        const size_t ackBytes = count * 4 + 1;
        if(messageEnd - messageStart < ackBytes)
        {
            return std::nullopt;
        }
        messageEnd -= ackBytes;
        packet.acks.reserve(count);
        for(size_t i = 0; i < count; i++)
        {
            packet.acks.push_back(getBig32(&data[messageEnd + i * 4]));
        }
    }

    const auto message = data.subspan(messageStart, messageEnd - messageStart);
    if(packet.flags & PacketFlags::Zerocoded)
    {
        auto decoded = zeroDecode(message);
        if(!decoded)
        {
            return std::nullopt;
        }
        packet.message = std::move(*decoded);
    }
    else
    {
        packet.message.assign(message.begin(), message.end());
    }
    return packet;
}

std::vector<uint8_t> buildPacket(uint8_t flags, uint32_t sequence, std::span<const uint8_t> message,
                                 std::span<const uint32_t> acks, size_t* acksUsed)
{
    flags = static_cast<uint8_t>(flags & ~PacketFlags::Ack);
    std::vector<uint8_t> encoded;
    std::span<const uint8_t> body = message;
    if(flags & PacketFlags::Zerocoded)
    {
        encoded = zeroEncode(message);
        if(encoded.size() < message.size())
        {
            body = encoded;
        }
        else
        {
            flags = static_cast<uint8_t>(flags & ~PacketFlags::Zerocoded);
        }
    }

    std::vector<uint8_t> out;
    out.reserve(kHeaderSize + body.size() + 1 + std::min<size_t>(acks.size(), 255) * 4);
    out.push_back(flags);
    putBig32(out, sequence);
    out.push_back(0); // no extra header
    out.insert(out.end(), body.begin(), body.end());

    size_t used = 0;
    while(used < acks.size() && used < 255 && out.size() + 4 + 1 <= kMaxPacketSize)
    {
        putBig32(out, acks[used]);
        used++;
    }
    if(used > 0)
    {
        out.push_back(static_cast<uint8_t>(used));
        out[0] = static_cast<uint8_t>(out[0] | PacketFlags::Ack);
    }
    if(acksUsed)
    {
        *acksUsed = used;
    }
    return out;
}

} // namespace viewer::protocol
