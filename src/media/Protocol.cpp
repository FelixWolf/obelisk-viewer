#include "viewer/media/Protocol.hpp"

#include <bit>
#include <cstring>
#include <type_traits>

namespace viewer::media
{

namespace
{

class Out
{
public:
    void u8(uint8_t v) { bytes.push_back(v); }
    void boolean(bool v) { u8(v ? 1 : 0); }
    void u32(uint32_t v)
    {
        for(int i = 0; i < 4; i++)
        {
            bytes.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }
    }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void f32(float v) { u32(std::bit_cast<uint32_t>(v)); }
    void string(const std::string& s)
    {
        u32(static_cast<uint32_t>(s.size()));
        bytes.insert(bytes.end(), s.begin(), s.end());
    }
    std::vector<uint8_t> bytes;
};

class In
{
public:
    explicit In(std::span<const uint8_t> bytes) : bytes_(bytes) {}
    uint8_t u8()
    {
        if(at_ + 1 > bytes_.size())
        {
            ok_ = false;
            return 0;
        }
        return bytes_[at_++];
    }
    bool boolean() { return u8() != 0; }
    uint32_t u32()
    {
        if(at_ + 4 > bytes_.size())
        {
            ok_ = false;
            return 0;
        }
        uint32_t v = 0;
        for(int i = 0; i < 4; i++)
        {
            v |= uint32_t{bytes_[at_++]} << (8 * i);
        }
        return v;
    }
    int32_t i32() { return static_cast<int32_t>(u32()); }
    float f32() { return std::bit_cast<float>(u32()); }
    std::string string()
    {
        const uint32_t n = u32();
        if(!ok_ || n > bytes_.size() - at_)
        {
            ok_ = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(bytes_.data() + at_), n);
        at_ += n;
        return s;
    }
    // All of it read, and nothing short.
    bool done() const { return ok_ && at_ == bytes_.size(); }
    size_t left() const { return bytes_.size() - at_; }
    void fail() { ok_ = false; }

private:
    std::span<const uint8_t> bytes_;
    size_t at_ = 0;
    bool ok_ = true;
};

// Each message's fields, both ways: `io` is an Out (writing) or an In.
template <typename IO, typename T>
void fields(IO& io, T& m)
{
    constexpr bool writing = std::is_same_v<IO, Out>;
    auto u32 = [&](uint32_t& v)
    {
        if constexpr(writing)
        {
            io.u32(v);
        }
        else
        {
            v = io.u32();
        }
    };
    auto i32 = [&](int32_t& v)
    {
        if constexpr(writing)
        {
            io.i32(v);
        }
        else
        {
            v = io.i32();
        }
    };
    auto boolean = [&](bool& v)
    {
        if constexpr(writing)
        {
            io.boolean(v);
        }
        else
        {
            v = io.boolean();
        }
    };
    auto string = [&](std::string& v)
    {
        if constexpr(writing)
        {
            io.string(v);
        }
        else
        {
            v = io.string();
        }
    };
    auto byte = [&](auto& v)
    {
        if constexpr(writing)
        {
            io.u8(static_cast<uint8_t>(v));
        }
        else
        {
            v = static_cast<std::remove_reference_t<decltype(v)>>(io.u8());
        }
    };

    if constexpr(std::is_same_v<T, Init>)
    {
        u32(m.version);
        string(m.root);
        boolean(m.sandbox);
        boolean(m.allowData);
    }
    else if constexpr(std::is_same_v<T, Open>)
    {
        u32(m.tab);
        string(m.profile);
        string(m.url);
        u32(m.width);
        u32(m.height);
        string(m.memory);
        u32(m.frameRate);
    }
    else if constexpr(std::is_same_v<T, Navigate> || std::is_same_v<T, Address>)
    {
        u32(m.tab);
        string(m.url);
    }
    else if constexpr(std::is_same_v<T, Resize>)
    {
        u32(m.tab);
        u32(m.width);
        u32(m.height);
        string(m.memory);
    }
    else if constexpr(std::is_same_v<T, Close> || std::is_same_v<T, AudioStop> ||
                      std::is_same_v<T, Closed> || std::is_same_v<T, Crashed> ||
                      std::is_same_v<T, Resized>)
    {
        u32(m.tab);
    }
    else if constexpr(std::is_same_v<T, Mouse>)
    {
        u32(m.tab);
        byte(m.kind);
        i32(m.x);
        i32(m.y);
        byte(m.button);
        byte(m.clicks);
    }
    else if constexpr(std::is_same_v<T, Wheel>)
    {
        u32(m.tab);
        i32(m.x);
        i32(m.y);
        i32(m.dx);
        i32(m.dy);
    }
    else if constexpr(std::is_same_v<T, Visibility>)
    {
        u32(m.tab);
        boolean(m.hidden);
        u32(m.frameRate);
    }
    else if constexpr(std::is_same_v<T, Release>)
    {
        u32(m.tab);
        u32(m.slot);
    }
    else if constexpr(std::is_same_v<T, Quit>)
    {
    }
    else if constexpr(std::is_same_v<T, Ready>)
    {
        u32(m.version);
        string(m.engine);
    }
    else if constexpr(std::is_same_v<T, Failed>)
    {
        string(m.reason);
    }
    else if constexpr(std::is_same_v<T, Frame>)
    {
        u32(m.tab);
        u32(m.slot);
        u32(m.width);
        u32(m.height);
    }
    else if constexpr(std::is_same_v<T, Title>)
    {
        u32(m.tab);
        string(m.title);
    }
    else if constexpr(std::is_same_v<T, Loading>)
    {
        u32(m.tab);
        boolean(m.loading);
    }
    else if constexpr(std::is_same_v<T, AudioStart>)
    {
        u32(m.tab);
        u32(m.sampleRate);
        u32(m.channels);
    }
    else if constexpr(std::is_same_v<T, Audio>)
    {
        u32(m.tab);
        if constexpr(writing)
        {
            io.u32(static_cast<uint32_t>(m.samples.size()));
            for(float s : m.samples)
            {
                io.f32(s);
            }
        }
        else
        {
            const uint32_t n = io.u32();
            if(n <= io.left() / 4)
            {
                m.samples.resize(n);
                for(float& s : m.samples)
                {
                    s = io.f32();
                }
            }
            else
            {
                io.fail();
            }
        }
    }
    else
    {
        static_assert(sizeof(T) == 0, "a message without its fields");
    }
}

template <size_t I = 0>
std::optional<Message> decodeAs(size_t type, In& in)
{
    if constexpr(I < std::variant_size_v<Message>)
    {
        if(type == I)
        {
            std::variant_alternative_t<I, Message> m;
            fields(in, m);
            if(!in.done())
            {
                return std::nullopt;
            }
            return Message(std::move(m));
        }
        return decodeAs<I + 1>(type, in);
    }
    else
    {
        return std::nullopt;
    }
}

} // namespace

std::vector<uint8_t> encode(const Message& message)
{
    Out out;
    out.u32(0); // the length, below
    out.u8(static_cast<uint8_t>(message.index()));
    std::visit(
        [&](const auto& m)
        {
            auto copy = m; // fields() takes it as either way's
            fields(out, copy);
        },
        message);
    const uint32_t length = static_cast<uint32_t>(out.bytes.size() - 4);
    for(int i = 0; i < 4; i++)
    {
        out.bytes[static_cast<size_t>(i)] = static_cast<uint8_t>(length >> (8 * i));
    }
    return std::move(out.bytes);
}

std::optional<Message> decode(std::span<const uint8_t> body)
{
    if(body.empty())
    {
        return std::nullopt;
    }
    In in(body.subspan(1));
    return decodeAs(body[0], in);
}

void Parser::feed(std::span<const uint8_t> bytes)
{
    if(at_ > 0 && at_ * 2 > buffer_.size())
    {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<ptrdiff_t>(at_));
        at_ = 0;
    }
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

std::optional<Message> Parser::next()
{
    if(broken_ || buffer_.size() - at_ < 4)
    {
        return std::nullopt;
    }
    uint32_t length = 0;
    for(int i = 0; i < 4; i++)
    {
        length |= uint32_t{buffer_[at_ + static_cast<size_t>(i)]} << (8 * i);
    }
    if(length == 0 || length > kMaxMessage)
    {
        broken_ = true;
        return std::nullopt;
    }
    if(buffer_.size() - at_ - 4 < length)
    {
        return std::nullopt;
    }
    auto m = decode(std::span(buffer_).subspan(at_ + 4, length));
    at_ += 4 + length;
    if(!m)
    {
        broken_ = true;
    }
    return m;
}

} // namespace viewer::media
