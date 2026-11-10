#pragma once

// UDP packet framing (https://wiki.secondlife.com/wiki/Packet_Layout):
//
//   flags (1) | sequence (4, big-endian) | extra length (1) | extra (n)
//   | message (zero-coded if flagged) | acks (4 each, big-endian) | ack count (1)
//
// The message is the message number plus its body; see MessageTypes.hpp.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace viewer::protocol
{

namespace PacketFlags
{
inline constexpr uint8_t Zerocoded = 0x80;
inline constexpr uint8_t Reliable = 0x40;
inline constexpr uint8_t Resent = 0x20;
inline constexpr uint8_t Ack = 0x10;
} // namespace PacketFlags

// Largest packet we send; acks are only appended while they fit.
inline constexpr size_t kMaxPacketSize = 1200;
// Largest message we accept after zero-decoding.
inline constexpr size_t kMaxDecodedSize = 64 * 1024;

struct Packet
{
    uint8_t flags = 0;
    uint32_t sequence = 0;
    std::vector<uint8_t> message{}; // zero-decoded
    std::vector<uint32_t> acks{};
};

// Returns nothing for malformed packets.
std::optional<Packet> parsePacket(std::span<const uint8_t> data);

// Builds a packet. If Zerocoded is set but zero-coding wouldn't make the
// message smaller, it is sent unencoded (and the flag cleared). Acks are
// taken from the front of `acks` while they fit in kMaxPacketSize (at most
// 255); the number used is returned through `acksUsed`.
std::vector<uint8_t> buildPacket(uint8_t flags, uint32_t sequence, std::span<const uint8_t> message,
                                 std::span<const uint32_t> acks = {}, size_t* acksUsed = nullptr);

std::vector<uint8_t> zeroEncode(std::span<const uint8_t> data);
std::optional<std::vector<uint8_t>> zeroDecode(std::span<const uint8_t> data,
                                               size_t maxSize = kMaxDecodedSize);

} // namespace viewer::protocol
