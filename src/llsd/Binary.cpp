// Binary LLSD. Format as the official viewer's llsdserialize.cpp
// (LLSDBinaryParser / LLSDBinaryFormatter); our own implementation.

#include "viewer/llsd/Serialization.hpp"

#include <bit>
#include <chrono>
#include <cstring>
#include <format>

namespace viewer::llsd
{

namespace
{

constexpr int kMaxDepth = 256;

class BinaryReader
{
public:
    explicit BinaryReader(std::span<const uint8_t> data) : data_(data) {}

    size_t position() const { return pos_; }

    std::expected<Value, std::string> value(int depth)
    {
        if(depth > kMaxDepth)
        {
            return error("nested too deeply");
        }
        uint8_t tag = 0;
        if(!byte(tag))
        {
            return error("unexpected end of data");
        }
        switch(tag)
        {
        case '!':
            return Value();
        case '1':
            return Value(true);
        case '0':
            return Value(false);
        case 'i':
        {
            uint32_t v = 0;
            if(!big32(v))
            {
                return error("truncated integer");
            }
            return Value(static_cast<int32_t>(v));
        }
        case 'r':
        {
            uint64_t v = 0;
            if(!big64(v))
            {
                return error("truncated real");
            }
            return Value(std::bit_cast<double>(v));
        }
        case 'u':
        {
            Uuid id;
            if(!bytes(id.bytes.data(), 16))
            {
                return error("truncated UUID");
            }
            return Value(id);
        }
        case 's':
        {
            auto s = sizedString();
            if(!s)
            {
                return error("truncated string");
            }
            return Value(std::move(*s));
        }
        case 'l':
        {
            auto s = sizedString();
            if(!s)
            {
                return error("truncated URI");
            }
            return Value(Uri{std::move(*s)});
        }
        case '\'':
        case '"':
        {
            auto s = delimitedString(static_cast<char>(tag));
            if(!s)
            {
                return error("unterminated string");
            }
            return Value(std::move(*s));
        }
        case 'd':
        {
            // Seconds since the epoch as a little-endian double.
            double seconds = 0;
            if(!bytes(&seconds, 8))
            {
                return error("truncated date");
            }
            return Value(Date(std::chrono::microseconds(static_cast<int64_t>(seconds * 1e6))));
        }
        case 'b':
        {
            uint32_t size = 0;
            if(!big32(size) || data_.size() - pos_ < size)
            {
                return error("truncated binary");
            }
            Binary b(data_.begin() + static_cast<ptrdiff_t>(pos_),
                     data_.begin() + static_cast<ptrdiff_t>(pos_ + size));
            pos_ += size;
            return Value(std::move(b));
        }
        case '[':
        {
            uint32_t count = 0;
            if(!big32(count))
            {
                return error("truncated array");
            }
            Array array;
            array.reserve(std::min<uint32_t>(count, 4096));
            for(uint32_t i = 0; i < count; i++)
            {
                auto v = value(depth + 1);
                if(!v)
                {
                    return v;
                }
                array.push_back(std::move(*v));
            }
            uint8_t close = 0;
            if(!byte(close) || close != ']')
            {
                return error("array not closed");
            }
            return Value(std::move(array));
        }
        case '{':
        {
            uint32_t count = 0;
            if(!big32(count))
            {
                return error("truncated map");
            }
            Map map;
            for(uint32_t i = 0; i < count; i++)
            {
                uint8_t keyTag = 0;
                if(!byte(keyTag))
                {
                    return error("truncated map key");
                }
                std::optional<std::string> key;
                if(keyTag == 'k')
                {
                    key = sizedString();
                }
                else if(keyTag == '\'' || keyTag == '"')
                {
                    key = delimitedString(static_cast<char>(keyTag));
                }
                if(!key)
                {
                    return error("bad map key");
                }
                auto v = value(depth + 1);
                if(!v)
                {
                    return v;
                }
                map.set(std::move(*key), std::move(*v));
            }
            uint8_t close = 0;
            if(!byte(close) || close != '}')
            {
                return error("map not closed");
            }
            return Value(std::move(map));
        }
        default:
            return error(std::format("unknown type tag 0x{:02x}", tag));
        }
    }

private:
    std::unexpected<std::string> error(std::string_view what) const
    {
        return std::unexpected(std::format("LLSD binary: {} at byte {}", what, pos_));
    }

    bool bytes(void* out, size_t size)
    {
        if(data_.size() - pos_ < size)
        {
            return false;
        }
        std::memcpy(out, data_.data() + pos_, size);
        pos_ += size;
        return true;
    }
    bool byte(uint8_t& out) { return bytes(&out, 1); }
    bool big32(uint32_t& out)
    {
        uint8_t b[4];
        if(!bytes(b, 4))
        {
            return false;
        }
        out = static_cast<uint32_t>(b[0]) << 24 | static_cast<uint32_t>(b[1]) << 16 |
              static_cast<uint32_t>(b[2]) << 8 | b[3];
        return true;
    }
    bool big64(uint64_t& out)
    {
        uint32_t high = 0;
        uint32_t low = 0;
        if(!big32(high) || !big32(low))
        {
            return false;
        }
        out = static_cast<uint64_t>(high) << 32 | low;
        return true;
    }
    std::optional<std::string> sizedString()
    {
        uint32_t size = 0;
        if(!big32(size) || data_.size() - pos_ < size)
        {
            return std::nullopt;
        }
        std::string s(reinterpret_cast<const char*>(data_.data() + pos_), size);
        pos_ += size;
        return s;
    }
    // A quoted string with backslash escapes (rarely used in binary LLSD).
    std::optional<std::string> delimitedString(char delimiter)
    {
        std::string s;
        while(pos_ < data_.size())
        {
            const char c = static_cast<char>(data_[pos_++]);
            if(c == delimiter)
            {
                return s;
            }
            if(c == '\\' && pos_ < data_.size())
            {
                const char e = static_cast<char>(data_[pos_++]);
                switch(e)
                {
                case 'n':
                    s.push_back('\n');
                    break;
                case 't':
                    s.push_back('\t');
                    break;
                case 'r':
                    s.push_back('\r');
                    break;
                default:
                    s.push_back(e);
                    break;
                }
                continue;
            }
            s.push_back(c);
        }
        return std::nullopt;
    }

    std::span<const uint8_t> data_;
    size_t pos_ = 0;
};

void putBig32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void putSized(std::vector<uint8_t>& out, std::string_view s)
{
    putBig32(out, static_cast<uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

void write(std::vector<uint8_t>& out, const Value& v)
{
    switch(v.type())
    {
    case Value::Type::Undefined:
        out.push_back('!');
        break;
    case Value::Type::Boolean:
        out.push_back(v.asBoolean() ? '1' : '0');
        break;
    case Value::Type::Integer:
        out.push_back('i');
        putBig32(out, static_cast<uint32_t>(v.asInteger()));
        break;
    case Value::Type::Real:
    {
        out.push_back('r');
        const auto bits = std::bit_cast<uint64_t>(v.asReal());
        putBig32(out, static_cast<uint32_t>(bits >> 32));
        putBig32(out, static_cast<uint32_t>(bits));
        break;
    }
    case Value::Type::String:
        out.push_back('s');
        putSized(out, *v.get<std::string>());
        break;
    case Value::Type::Uuid:
        out.push_back('u');
        out.insert(out.end(), v.get<Uuid>()->bytes.begin(), v.get<Uuid>()->bytes.end());
        break;
    case Value::Type::Date:
    {
        out.push_back('d');
        const double seconds = static_cast<double>(v.get<Date>()->time_since_epoch().count()) / 1e6;
        uint8_t b[8];
        std::memcpy(b, &seconds, 8);
        out.insert(out.end(), b, b + 8);
        break;
    }
    case Value::Type::Uri:
        out.push_back('l');
        putSized(out, v.get<Uri>()->value);
        break;
    case Value::Type::Binary:
    {
        const Binary& b = *v.get<Binary>();
        out.push_back('b');
        putBig32(out, static_cast<uint32_t>(b.size()));
        out.insert(out.end(), b.begin(), b.end());
        break;
    }
    case Value::Type::Map:
    {
        const Map& m = *v.get<Map>();
        out.push_back('{');
        putBig32(out, static_cast<uint32_t>(m.size()));
        for(const auto& [key, value] : m)
        {
            out.push_back('k');
            putSized(out, key);
            write(out, value);
        }
        out.push_back('}');
        break;
    }
    case Value::Type::Array:
    {
        const Array& a = *v.get<Array>();
        out.push_back('[');
        putBig32(out, static_cast<uint32_t>(a.size()));
        for(const Value& value : a)
        {
            write(out, value);
        }
        out.push_back(']');
        break;
    }
    }
}

} // namespace

std::expected<Value, std::string> fromBinary(std::span<const uint8_t> data, size_t* consumed)
{
    BinaryReader reader(data);
    auto value = reader.value(0);
    if(value && consumed)
    {
        *consumed = reader.position();
    }
    return value;
}

std::vector<uint8_t> toBinary(const Value& value)
{
    std::vector<uint8_t> out;
    write(out, value);
    return out;
}

} // namespace viewer::llsd
