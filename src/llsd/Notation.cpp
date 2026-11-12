// LLSD notation (llsdserialize.cpp's LLSDNotationParser) and JSON, both
// read into Values.

#include "BinaryEncoding.hpp"
#include "viewer/llsd/Serialization.hpp"

#include <charconv>
#include <cmath>
#include <format>

namespace viewer::llsd
{

namespace
{

constexpr int kMaxDepth = 64;

class Reader
{
public:
    explicit Reader(std::string_view text) : text_(text) {}

    bool done() const { return at_ >= text_.size(); }
    char peek() const { return done() ? '\0' : text_[at_]; }
    char get() { return done() ? '\0' : text_[at_++]; }
    size_t position() const { return at_; }
    void skipSpace()
    {
        while(!done() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r'))
        {
            at_++;
        }
    }
    // Space and commas (notation separates with either).
    void skipSeparators()
    {
        while(!done() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r' ||
                          peek() == ','))
        {
            at_++;
        }
    }
    bool consume(std::string_view word)
    {
        if(text_.substr(at_).starts_with(word))
        {
            at_ += word.size();
            return true;
        }
        return false;
    }
    std::string_view take(size_t n)
    {
        if(text_.size() - at_ < n)
        {
            at_ = text_.size();
            return {};
        }
        const auto out = text_.substr(at_, n);
        at_ += n;
        return out;
    }
    // Characters while `keep` holds.
    template <typename F>
    std::string_view takeWhile(F keep)
    {
        const size_t start = at_;
        while(!done() && keep(peek()))
        {
            at_++;
        }
        return text_.substr(start, at_ - start);
    }

private:
    std::string_view text_;
    size_t at_ = 0;
};

std::unexpected<std::string> error(const Reader& r, std::string_view what)
{
    return std::unexpected(std::format("LLSD notation: {} at {}", what, r.position()));
}

int hexDigit(char c)
{
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}

// A quoted string with backslash escapes (\n, \xHH, ...); the opening quote
// is next.
std::expected<std::string, std::string> quoted(Reader& r)
{
    const char quote = r.get();
    std::string out;
    while(true)
    {
        if(r.done())
        {
            return error(r, "unterminated string");
        }
        char c = r.get();
        if(c == quote)
        {
            return out;
        }
        if(c != '\\')
        {
            out.push_back(c);
            continue;
        }
        c = r.get();
        switch(c)
        {
        case 'a':
            out.push_back('\a');
            break;
        case 'b':
            out.push_back('\b');
            break;
        case 'f':
            out.push_back('\f');
            break;
        case 'n':
            out.push_back('\n');
            break;
        case 'r':
            out.push_back('\r');
            break;
        case 't':
            out.push_back('\t');
            break;
        case 'v':
            out.push_back('\v');
            break;
        case 'x':
        {
            const int high = hexDigit(r.get());
            const int low = hexDigit(r.get());
            if(high < 0 || low < 0)
            {
                return error(r, "bad \\x escape");
            }
            out.push_back(static_cast<char>(high * 16 + low));
            break;
        }
        default:
            out.push_back(c);
            break;
        }
    }
}

// "(length)" followed by a quote, `length` raw bytes and the same quote.
std::expected<std::string, std::string> sized(Reader& r)
{
    if(r.get() != '(')
    {
        return error(r, "expected (length)");
    }
    const auto digits = r.takeWhile([](char c) { return c >= '0' && c <= '9'; });
    size_t length = 0;
    std::from_chars(digits.data(), digits.data() + digits.size(), length);
    if(r.get() != ')')
    {
        return error(r, "expected )");
    }
    const char quote = r.get();
    if(quote != '"' && quote != '\'')
    {
        return error(r, "expected a quote");
    }
    const auto bytes = r.take(length);
    if(bytes.size() != length || r.get() != quote)
    {
        return error(r, "sized string runs past the end");
    }
    return std::string(bytes);
}

// A string in any of notation's forms: "...", '...' or s(n)"...".
std::expected<std::string, std::string> anyString(Reader& r)
{
    if(r.peek() == 's')
    {
        r.get();
        return sized(r);
    }
    return quoted(r);
}

std::expected<Value, std::string> notationValue(Reader& r, int depth);

std::expected<Value, std::string> notationNumber(Reader& r, bool real)
{
    const auto text = r.takeWhile(
        [](char c)
        {
            return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' ||
                   c == 'E' || c == 'n' || c == 'a' || c == 'i' || c == 'f';
        });
    if(real)
    {
        if(text == "nan")
        {
            return Value(std::nan(""));
        }
        double v = 0.0;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
        if(ec != std::errc())
        {
            return error(r, "bad real");
        }
        return Value(v);
    }
    int64_t v = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    if(ec != std::errc())
    {
        return error(r, "bad integer");
    }
    return Value(v);
}

std::expected<Value, std::string> notationValue(Reader& r, int depth)
{
    if(depth > kMaxDepth)
    {
        return error(r, "nested too deeply");
    }
    r.skipSpace();
    const char c = r.peek();
    switch(c)
    {
    case '{':
    {
        r.get();
        Map map;
        while(true)
        {
            r.skipSeparators();
            if(r.peek() == '}')
            {
                r.get();
                return Value(std::move(map));
            }
            if(r.done())
            {
                return error(r, "unterminated map");
            }
            auto key = anyString(r);
            if(!key)
            {
                return std::unexpected(key.error());
            }
            r.skipSpace();
            if(r.get() != ':')
            {
                return error(r, "expected :");
            }
            auto value = notationValue(r, depth + 1);
            if(!value)
            {
                return value;
            }
            map[*key] = std::move(*value);
        }
    }
    case '[':
    {
        r.get();
        Array array;
        while(true)
        {
            r.skipSeparators();
            if(r.peek() == ']')
            {
                r.get();
                return Value(std::move(array));
            }
            if(r.done())
            {
                return error(r, "unterminated array");
            }
            auto value = notationValue(r, depth + 1);
            if(!value)
            {
                return value;
            }
            array.push_back(std::move(*value));
        }
    }
    case '!':
        r.get();
        return Value();
    case '0':
        r.get();
        return Value(false);
    case '1':
        r.get();
        return Value(true);
    case 'f':
    case 'F':
        if(r.consume("false") || r.consume("FALSE"))
        {
            return Value(false);
        }
        r.get();
        return Value(false);
    case 't':
    case 'T':
        if(r.consume("true") || r.consume("TRUE"))
        {
            return Value(true);
        }
        r.get();
        return Value(true);
    case 'i':
        r.get();
        return notationNumber(r, false);
    case 'r':
        r.get();
        return notationNumber(r, true);
    case 'u':
    {
        r.get();
        const auto text = r.take(36);
        const auto id = Uuid::parse(text);
        if(!id)
        {
            return error(r, "bad UUID");
        }
        return Value(*id);
    }
    case '"':
    case '\'':
    case 's':
    {
        auto s = anyString(r);
        if(!s)
        {
            return std::unexpected(s.error());
        }
        return Value(std::move(*s));
    }
    case 'l':
    {
        r.get();
        auto s = quoted(r);
        if(!s)
        {
            return std::unexpected(s.error());
        }
        return Value(Uri{std::move(*s)});
    }
    case 'd':
    {
        r.get();
        auto s = quoted(r);
        if(!s)
        {
            return std::unexpected(s.error());
        }
        return Value(parseDate(*s).value_or(Date{}));
    }
    case 'b':
    {
        r.get();
        if(r.peek() == '(')
        {
            auto raw = sized(r);
            if(!raw)
            {
                return std::unexpected(raw.error());
            }
            return Value(Binary(raw->begin(), raw->end()));
        }
        const bool base16 = r.consume("16");
        if(!base16 && !r.consume("64"))
        {
            return error(r, "unknown binary encoding");
        }
        auto text = quoted(r);
        if(!text)
        {
            return std::unexpected(text.error());
        }
        auto bytes = base16 ? detail::decodeBase16(*text) : detail::decodeBase64(*text);
        if(!bytes)
        {
            return error(r, "bad binary");
        }
        return Value(std::move(*bytes));
    }
    default:
        return error(r, std::format("unexpected '{}'", c));
    }
}

// --- JSON ----------------------------------------------------------------------

std::unexpected<std::string> jsonError(const Reader& r, std::string_view what)
{
    return std::unexpected(std::format("JSON: {} at {}", what, r.position()));
}

void appendUtf8(std::string& out, uint32_t cp)
{
    if(cp < 0x80)
    {
        out.push_back(static_cast<char>(cp));
    }
    else if(cp < 0x800)
    {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else if(cp < 0x10000)
    {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else
    {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::expected<std::string, std::string> jsonString(Reader& r)
{
    if(r.get() != '"')
    {
        return jsonError(r, "expected a string");
    }
    std::string out;
    auto hex4 = [&]() -> std::optional<uint32_t>
    {
        uint32_t v = 0;
        for(int i = 0; i < 4; i++)
        {
            const int d = hexDigit(r.get());
            if(d < 0)
            {
                return std::nullopt;
            }
            v = v << 4 | static_cast<uint32_t>(d);
        }
        return v;
    };
    while(true)
    {
        if(r.done())
        {
            return jsonError(r, "unterminated string");
        }
        const char c = r.get();
        if(c == '"')
        {
            return out;
        }
        if(c != '\\')
        {
            out.push_back(c);
            continue;
        }
        const char e = r.get();
        switch(e)
        {
        case 'b':
            out.push_back('\b');
            break;
        case 'f':
            out.push_back('\f');
            break;
        case 'n':
            out.push_back('\n');
            break;
        case 'r':
            out.push_back('\r');
            break;
        case 't':
            out.push_back('\t');
            break;
        case 'u':
        {
            auto cp = hex4();
            if(!cp)
            {
                return jsonError(r, "bad \\u escape");
            }
            if(*cp >= 0xD800 && *cp < 0xDC00 && r.consume("\\u"))
            {
                const auto low = hex4();
                if(low && *low >= 0xDC00 && *low < 0xE000)
                {
                    *cp = 0x10000 + ((*cp - 0xD800) << 10) + (*low - 0xDC00);
                }
            }
            appendUtf8(out, *cp);
            break;
        }
        default:
            out.push_back(e); // \" \\ \/
            break;
        }
    }
}

std::expected<Value, std::string> jsonValue(Reader& r, int depth)
{
    if(depth > kMaxDepth)
    {
        return jsonError(r, "nested too deeply");
    }
    r.skipSpace();
    const char c = r.peek();
    if(c == '{')
    {
        r.get();
        Map map;
        r.skipSpace();
        if(r.peek() == '}')
        {
            r.get();
            return Value(std::move(map));
        }
        while(true)
        {
            r.skipSpace();
            auto key = jsonString(r);
            if(!key)
            {
                return std::unexpected(key.error());
            }
            r.skipSpace();
            if(r.get() != ':')
            {
                return jsonError(r, "expected :");
            }
            auto value = jsonValue(r, depth + 1);
            if(!value)
            {
                return value;
            }
            map[*key] = std::move(*value);
            r.skipSpace();
            const char next = r.get();
            if(next == '}')
            {
                return Value(std::move(map));
            }
            if(next != ',')
            {
                return jsonError(r, "expected , or }");
            }
        }
    }
    if(c == '[')
    {
        r.get();
        Array array;
        r.skipSpace();
        if(r.peek() == ']')
        {
            r.get();
            return Value(std::move(array));
        }
        while(true)
        {
            auto value = jsonValue(r, depth + 1);
            if(!value)
            {
                return value;
            }
            array.push_back(std::move(*value));
            r.skipSpace();
            const char next = r.get();
            if(next == ']')
            {
                return Value(std::move(array));
            }
            if(next != ',')
            {
                return jsonError(r, "expected , or ]");
            }
        }
    }
    if(c == '"')
    {
        auto s = jsonString(r);
        if(!s)
        {
            return std::unexpected(s.error());
        }
        return Value(std::move(*s));
    }
    if(r.consume("true"))
    {
        return Value(true);
    }
    if(r.consume("false"))
    {
        return Value(false);
    }
    if(r.consume("null"))
    {
        return Value();
    }
    const auto text = r.takeWhile(
        [](char ch)
        {
            return (ch >= '0' && ch <= '9') || ch == '-' || ch == '+' || ch == '.' || ch == 'e' ||
                   ch == 'E';
        });
    if(text.empty())
    {
        return jsonError(r, std::format("unexpected '{}'", c));
    }
    // Whole numbers that fit are integers; everything else is real.
    if(text.find_first_of(".eE") == std::string_view::npos)
    {
        int64_t v = 0;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
        if(ec == std::errc() && end == text.data() + text.size() && v >= INT32_MIN &&
           v <= INT32_MAX)
        {
            return Value(v);
        }
    }
    double v = 0.0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    if(ec != std::errc())
    {
        return jsonError(r, "bad number");
    }
    return Value(v);
}

} // namespace

std::expected<Value, std::string> fromNotation(std::string_view text)
{
    Reader r(text);
    r.skipSpace();
    r.consume("<?llsd/notation?>");
    return notationValue(r, 0);
}

std::expected<Value, std::string> fromJson(std::string_view text)
{
    Reader r(text);
    auto value = jsonValue(r, 0);
    if(!value)
    {
        return value;
    }
    r.skipSpace();
    if(!r.done())
    {
        return jsonError(r, "trailing characters");
    }
    return value;
}

} // namespace viewer::llsd
