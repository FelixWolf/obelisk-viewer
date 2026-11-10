#pragma once

// Building blocks for the generated message structs (Messages.hpp): field
// types, the bounds-checked reader/writer, and message metadata.
//
// Wire format (see message_template.msg): numbers are little-endian, except
// the message number, IPADDR and IPPORT, which are big-endian.

#include "viewer/core/Uuid.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace viewer::protocol
{

struct Vector3
{
    float x = 0;
    float y = 0;
    float z = 0;
    bool operator==(const Vector3&) const = default;
};

struct Vector3d
{
    double x = 0;
    double y = 0;
    double z = 0;
    bool operator==(const Vector3d&) const = default;
};

struct Vector4
{
    float x = 0;
    float y = 0;
    float z = 0;
    float w = 0;
    bool operator==(const Vector4&) const = default;
};

// Sent as x, y, z only; w is rebuilt on receipt (unit quaternion, w >= 0).
struct Quaternion
{
    float x = 0;
    float y = 0;
    float z = 0;
    float w = 1;
    bool operator==(const Quaternion&) const = default;
};

enum class Frequency : uint8_t
{
    High,   // 1-byte message number
    Medium, // 0xFF + 1 byte
    Low,    // 0xFF 0xFF + 2 bytes
    Fixed,  // 4 bytes, 0xFFFFFFxx
};

enum class Deprecation : uint8_t
{
    None,
    UdpDeprecated,  // still works over UDP; the simulator prefers the event queue
    UdpBlacklisted, // must not be sent or accepted over UDP
    Deprecated,
};

// Message numbers are stored fully qualified: High n, Medium 0xFF00 | n,
// Low 0xFFFF0000 | n, Fixed as written in the template.
using MessageId = uint32_t;

struct MessageInfo
{
    MessageId id;
    std::string_view name;
    Frequency frequency;
    bool trusted;   // only simulators may send it (we never accept it from elsewhere)
    bool zerocoded; // send with the zero-coding flag
    Deprecation deprecation;
};

// Appends little-endian fields to a byte vector.
class MessageWriter
{
public:
    explicit MessageWriter(std::vector<uint8_t>& out) : out_(out) {}

    void messageId(MessageId id, Frequency frequency);

    void u8(uint8_t v) { out_.push_back(v); }
    void u16(uint16_t v) { little(v); }
    void u32(uint32_t v) { little(v); }
    void u64(uint64_t v) { little(v); }
    void s8(int8_t v) { little(static_cast<uint8_t>(v)); }
    void s16(int16_t v) { little(static_cast<uint16_t>(v)); }
    void s32(int32_t v) { little(static_cast<uint32_t>(v)); }
    void s64(int64_t v) { little(static_cast<uint64_t>(v)); }
    void f32(float v);
    void f64(double v);
    void boolean(bool v) { out_.push_back(v ? 1 : 0); }
    void uuid(const Uuid& v) { out_.insert(out_.end(), v.bytes.begin(), v.bytes.end()); }
    void vector3(const Vector3& v);
    void vector3d(const Vector3d& v);
    void vector4(const Vector4& v);
    void quaternion(const Quaternion& v);
    void ipAddress(uint32_t v); // a.b.c.d is (a << 24) | ... ; sent as a, b, c, d
    void ipPort(uint16_t v);    // big-endian
    void fixed(std::span<const uint8_t> v) { out_.insert(out_.end(), v.begin(), v.end()); }
    // Length-prefixed (1 or 2 bytes). Longer data is truncated.
    void variable1(const std::vector<uint8_t>& v);
    void variable2(const std::vector<uint8_t>& v);

private:
    template <typename T>
    void little(T v)
    {
        for(size_t i = 0; i < sizeof(T); i++)
        {
            out_.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }
    }

    std::vector<uint8_t>& out_;
};

// Reads fields from a byte span. Reading past the end sets failed() and
// returns zeros from then on, so decoders don't need to check every field.
class MessageReader
{
public:
    explicit MessageReader(std::span<const uint8_t> data) : data_(data) {}

    bool failed() const { return failed_; }
    size_t remaining() const { return data_.size() - position_; }
    size_t position() const { return position_; }

    uint8_t u8();
    uint16_t u16() { return little<uint16_t>(); }
    uint32_t u32() { return little<uint32_t>(); }
    uint64_t u64() { return little<uint64_t>(); }
    int8_t s8() { return static_cast<int8_t>(u8()); }
    int16_t s16() { return static_cast<int16_t>(little<uint16_t>()); }
    int32_t s32() { return static_cast<int32_t>(little<uint32_t>()); }
    int64_t s64() { return static_cast<int64_t>(little<uint64_t>()); }
    float f32();
    double f64();
    bool boolean() { return u8() != 0; }
    Uuid uuid();
    Vector3 vector3();
    Vector3d vector3d();
    Vector4 vector4();
    Quaternion quaternion();
    uint32_t ipAddress();
    uint16_t ipPort();
    template <size_t N>
    std::array<uint8_t, N> fixed()
    {
        std::array<uint8_t, N> out{};
        read(out.data(), N);
        return out;
    }
    std::vector<uint8_t> variable1() { return variable(u8()); }
    std::vector<uint8_t> variable2() { return variable(u16()); }

private:
    bool read(uint8_t* out, size_t size);
    std::vector<uint8_t> variable(size_t size);

    template <typename T>
    T little()
    {
        uint8_t bytes[sizeof(T)] = {};
        read(bytes, sizeof(T));
        T v = 0;
        for(size_t i = 0; i < sizeof(T); i++)
        {
            v = static_cast<T>(v | static_cast<T>(static_cast<T>(bytes[i]) << (8 * i)));
        }
        return v;
    }

    std::span<const uint8_t> data_;
    size_t position_ = 0;
    bool failed_ = false;
};

// Reads the message number at the start of a (zero-decoded) message.
// Returns the id and how many bytes it took.
std::optional<std::pair<MessageId, size_t>> readMessageId(std::span<const uint8_t> body);

// Variable fields that carry text are null-terminated on the wire.
std::string toString(const std::vector<uint8_t>& field);
std::vector<uint8_t> fromString(std::string_view text);

// Every generated message struct M satisfies this.
template <typename M>
concept Message = requires(const M& constMessage, M& message, MessageWriter& w, MessageReader& r) {
    { M::kInfo } -> std::convertible_to<MessageInfo>;
    constMessage.encode(w);
    { message.decode(r) } -> std::same_as<bool>;
};

// Message number + body, not zero-coded (Packet does that).
template <Message M>
std::vector<uint8_t> encodeMessage(const M& message)
{
    std::vector<uint8_t> out;
    MessageWriter w(out);
    w.messageId(M::kInfo.id, M::kInfo.frequency);
    message.encode(w);
    return out;
}

// Decodes a message whose number has already been checked to be M's.
// Returns nothing if the data is truncated.
template <Message M>
std::optional<M> decodeMessage(std::span<const uint8_t> body)
{
    const auto id = readMessageId(body);
    if(!id || id->first != M::kInfo.id)
    {
        return std::nullopt;
    }
    MessageReader r(body.subspan(id->second));
    M message;
    if(!message.decode(r))
    {
        return std::nullopt;
    }
    return message;
}

} // namespace viewer::protocol
