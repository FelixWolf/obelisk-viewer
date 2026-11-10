#include "viewer/protocol/MessageTypes.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace viewer::protocol
{

void MessageWriter::messageId(MessageId id, Frequency frequency)
{
    switch(frequency)
    {
    case Frequency::High:
        out_.push_back(static_cast<uint8_t>(id));
        break;
    case Frequency::Medium:
        out_.push_back(0xFF);
        out_.push_back(static_cast<uint8_t>(id));
        break;
    case Frequency::Low:
    case Frequency::Fixed:
        for(int shift = 24; shift >= 0; shift -= 8)
        {
            out_.push_back(static_cast<uint8_t>(id >> shift));
        }
        break;
    }
}

void MessageWriter::f32(float v)
{
    little(std::bit_cast<uint32_t>(v));
}

void MessageWriter::f64(double v)
{
    little(std::bit_cast<uint64_t>(v));
}

void MessageWriter::vector3(const Vector3& v)
{
    f32(v.x);
    f32(v.y);
    f32(v.z);
}

void MessageWriter::vector3d(const Vector3d& v)
{
    f64(v.x);
    f64(v.y);
    f64(v.z);
}

void MessageWriter::vector4(const Vector4& v)
{
    f32(v.x);
    f32(v.y);
    f32(v.z);
    f32(v.w);
}

void MessageWriter::quaternion(const Quaternion& q)
{
    // Normalize and flip to w >= 0 so the receiver can rebuild w.
    const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    const float scale = length > 0 ? (q.w < 0 ? -1.0f : 1.0f) / length : 0.0f;
    f32(q.x * scale);
    f32(q.y * scale);
    f32(q.z * scale);
}

void MessageWriter::ipAddress(uint32_t v)
{
    for(int shift = 24; shift >= 0; shift -= 8)
    {
        out_.push_back(static_cast<uint8_t>(v >> shift));
    }
}

void MessageWriter::ipPort(uint16_t v)
{
    out_.push_back(static_cast<uint8_t>(v >> 8));
    out_.push_back(static_cast<uint8_t>(v));
}

void MessageWriter::variable1(const std::vector<uint8_t>& v)
{
    const size_t size = std::min<size_t>(v.size(), 0xFF);
    u8(static_cast<uint8_t>(size));
    out_.insert(out_.end(), v.begin(), v.begin() + static_cast<ptrdiff_t>(size));
}

void MessageWriter::variable2(const std::vector<uint8_t>& v)
{
    const size_t size = std::min<size_t>(v.size(), 0xFFFF);
    u16(static_cast<uint16_t>(size));
    out_.insert(out_.end(), v.begin(), v.begin() + static_cast<ptrdiff_t>(size));
}

bool MessageReader::read(uint8_t* out, size_t size)
{
    if(failed_ || remaining() < size)
    {
        failed_ = true;
        std::memset(out, 0, size);
        return false;
    }
    std::memcpy(out, data_.data() + position_, size);
    position_ += size;
    return true;
}

uint8_t MessageReader::u8()
{
    uint8_t v = 0;
    read(&v, 1);
    return v;
}

float MessageReader::f32()
{
    return std::bit_cast<float>(little<uint32_t>());
}

double MessageReader::f64()
{
    return std::bit_cast<double>(little<uint64_t>());
}

Uuid MessageReader::uuid()
{
    Uuid id;
    read(id.bytes.data(), id.bytes.size());
    return id;
}

Vector3 MessageReader::vector3()
{
    Vector3 v;
    v.x = f32();
    v.y = f32();
    v.z = f32();
    return v;
}

Vector3d MessageReader::vector3d()
{
    Vector3d v;
    v.x = f64();
    v.y = f64();
    v.z = f64();
    return v;
}

Vector4 MessageReader::vector4()
{
    Vector4 v;
    v.x = f32();
    v.y = f32();
    v.z = f32();
    v.w = f32();
    return v;
}

Quaternion MessageReader::quaternion()
{
    Quaternion q;
    q.x = f32();
    q.y = f32();
    q.z = f32();
    const float w2 = 1.0f - (q.x * q.x + q.y * q.y + q.z * q.z);
    q.w = w2 > 0 ? std::sqrt(w2) : 0.0f;
    return q;
}

uint32_t MessageReader::ipAddress()
{
    uint8_t b[4] = {};
    read(b, 4);
    return static_cast<uint32_t>(b[0]) << 24 | static_cast<uint32_t>(b[1]) << 16 |
           static_cast<uint32_t>(b[2]) << 8 | b[3];
}

uint16_t MessageReader::ipPort()
{
    uint8_t b[2] = {};
    read(b, 2);
    return static_cast<uint16_t>(b[0] << 8 | b[1]);
}

std::vector<uint8_t> MessageReader::variable(size_t size)
{
    std::vector<uint8_t> out(size);
    if(!read(out.data(), size))
    {
        out.clear();
    }
    return out;
}

std::optional<std::pair<MessageId, size_t>> readMessageId(std::span<const uint8_t> body)
{
    if(body.empty())
    {
        return std::nullopt;
    }
    if(body[0] != 0xFF)
    {
        return std::pair<MessageId, size_t>{body[0], 1};
    }
    if(body.size() < 2)
    {
        return std::nullopt;
    }
    if(body[1] != 0xFF)
    {
        return std::pair<MessageId, size_t>{0xFF00u | body[1], 2};
    }
    if(body.size() < 4)
    {
        return std::nullopt;
    }
    const MessageId id = 0xFFFF0000u | static_cast<uint32_t>(body[2]) << 8 | body[3];
    return std::pair<MessageId, size_t>{id, 4};
}

std::string toString(const std::vector<uint8_t>& field)
{
    const auto end = std::ranges::find(field, uint8_t{0});
    return std::string(field.begin(), end);
}

std::vector<uint8_t> fromString(std::string_view text)
{
    std::vector<uint8_t> out(text.begin(), text.end());
    out.push_back(0);
    return out;
}

} // namespace viewer::protocol
