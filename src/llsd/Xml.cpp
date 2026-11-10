// LLSD+XML, following the same rules as pymetaverse's llsd.py:
//  - empty elements mean the type's default (<integer /> is 0, <date /> is the epoch)
//  - <boolean> accepts 1/0/true/false (any case) and empty
//  - <binary> accepts encoding="base64" (default), "base85" and "base16"
//  - comments are ignored

#include "BinaryEncoding.hpp"
#include "viewer/llsd/Serialization.hpp"

#include <tinyxml2.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>

namespace viewer::llsd
{

namespace
{

using tinyxml2::XMLElement;

std::string_view trim(std::string_view s)
{
    while(!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
    {
        s.remove_prefix(1);
    }
    while(!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
    {
        s.remove_suffix(1);
    }
    return s;
}

std::string lowercase(std::string_view s)
{
    std::string out(s);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string_view textOf(const XMLElement& element)
{
    const char* text = element.GetText();
    return text ? std::string_view(text) : std::string_view();
}

std::unexpected<std::string> fail(const XMLElement& element, std::string_view message)
{
    return std::unexpected(
        std::format("LLSD+XML line {}: <{}> {}", element.GetLineNum(), element.Name(), message));
}

std::expected<Value, std::string> parseElement(const XMLElement& element, int depth)
{
    constexpr int kMaxDepth = 256;
    if(depth > kMaxDepth)
    {
        return fail(element, "nested too deeply");
    }

    const std::string_view tag = element.Name();
    const std::string_view text = textOf(element);

    if(tag == "undef")
    {
        return Value();
    }
    if(tag == "boolean")
    {
        const std::string v = lowercase(trim(text));
        if(v == "1" || v == "true")
        {
            return Value(true);
        }
        if(v.empty() || v == "0" || v == "false")
        {
            return Value(false);
        }
        return fail(element, std::format("has unexpected value '{}'", v));
    }
    if(tag == "integer")
    {
        const std::string_view v = trim(text);
        if(v.empty())
        {
            return Value(0);
        }
        int64_t parsed = 0;
        const auto result = std::from_chars(v.data(), v.data() + v.size(), parsed);
        if(result.ec != std::errc() || result.ptr != v.data() + v.size())
        {
            return fail(element, std::format("has invalid value '{}'", v));
        }
        // Values in the uint32 range keep their bit pattern: SL sends U32
        // fields (circuit codes, flags) as LLSD integers.
        if(parsed > std::numeric_limits<int32_t>::max() &&
           parsed <= std::numeric_limits<uint32_t>::max())
        {
            return Value(static_cast<int32_t>(static_cast<uint32_t>(parsed)));
        }
        parsed = std::clamp<int64_t>(parsed, std::numeric_limits<int32_t>::min(),
                                     std::numeric_limits<int32_t>::max());
        return Value(static_cast<int32_t>(parsed));
    }
    if(tag == "real")
    {
        std::string_view v = trim(text);
        if(v.empty())
        {
            return Value(0.0);
        }
        if(v.front() == '+')
        {
            v.remove_prefix(1);
        }
        double parsed = 0;
        const auto result = std::from_chars(v.data(), v.data() + v.size(), parsed);
        if(result.ec != std::errc() || result.ptr != v.data() + v.size())
        {
            return fail(element, std::format("has invalid value '{}'", v));
        }
        return Value(parsed);
    }
    if(tag == "uuid")
    {
        const std::string_view v = trim(text);
        if(v.empty())
        {
            return Value(Uuid{});
        }
        const auto id = Uuid::parse(v);
        if(!id)
        {
            return fail(element, std::format("has invalid value '{}'", v));
        }
        return Value(*id);
    }
    if(tag == "string")
    {
        return Value(std::string(text));
    }
    if(tag == "binary")
    {
        const char* attribute = element.Attribute("encoding");
        const std::string encoding = attribute ? lowercase(attribute) : "base64";
        std::optional<Binary> decoded;
        if(encoding == "base64")
        {
            decoded = detail::decodeBase64(text);
        }
        else if(encoding == "base85")
        {
            decoded = detail::decodeBase85(text);
        }
        else if(encoding == "base16")
        {
            decoded = detail::decodeBase16(text);
        }
        else
        {
            return fail(element, std::format("has unknown encoding '{}'", encoding));
        }
        if(!decoded)
        {
            return fail(element, std::format("has invalid {} data", encoding));
        }
        return Value(std::move(*decoded));
    }
    if(tag == "date")
    {
        if(trim(text).empty())
        {
            return Value(Date{});
        }
        const auto date = parseDate(text);
        if(!date)
        {
            return fail(element, std::format("has invalid timestamp '{}'", text));
        }
        return Value(*date);
    }
    if(tag == "uri")
    {
        return Value(Uri{std::string(text)});
    }
    if(tag == "map")
    {
        Map map;
        for(const XMLElement* key = element.FirstChildElement(); key;)
        {
            if(std::string_view(key->Name()) != "key")
            {
                return fail(*key, "found in map where <key> was expected");
            }
            const XMLElement* valueElement = key->NextSiblingElement();
            if(!valueElement)
            {
                return fail(*key, "has no value");
            }
            auto value = parseElement(*valueElement, depth + 1);
            if(!value)
            {
                return value;
            }
            map.set(std::string(textOf(*key)), std::move(*value));
            key = valueElement->NextSiblingElement();
        }
        return Value(std::move(map));
    }
    if(tag == "array")
    {
        Array array;
        for(const XMLElement* child = element.FirstChildElement(); child;
            child = child->NextSiblingElement())
        {
            auto value = parseElement(*child, depth + 1);
            if(!value)
            {
                return value;
            }
            array.push_back(std::move(*value));
        }
        return Value(std::move(array));
    }
    return fail(element, "is not an LLSD element");
}

class XmlWriter
{
public:
    explicit XmlWriter(const XmlWriteOptions& options)
        : options_(options), printer_(nullptr, !options.pretty)
    {
    }

    std::string finish(const Value& value)
    {
        printer_.PushHeader(false, true);
        printer_.OpenElement("llsd", !options_.pretty);
        write(value);
        printer_.CloseElement(!options_.pretty);
        return std::string(printer_.CStr(), static_cast<size_t>(printer_.CStrSize() - 1));
    }

private:
    void leaf(const char* tag, std::string_view text, bool isDefault)
    {
        // Elements with text stay on one line even in pretty mode.
        printer_.OpenElement(tag, true);
        if(!(options_.compact && isDefault))
        {
            printer_.PushText(std::string(text).c_str());
        }
        printer_.CloseElement(true);
    }

    void write(const Value& value)
    {
        switch(value.type())
        {
        case Value::Type::Undefined:
            printer_.OpenElement("undef", true);
            printer_.CloseElement(true);
            break;
        case Value::Type::Boolean:
        {
            const bool b = *value.get<bool>();
            leaf("boolean", b ? "true" : "false", !b);
            break;
        }
        case Value::Type::Integer:
        {
            const int32_t i = *value.get<int32_t>();
            leaf("integer", value.asString(), i == 0);
            break;
        }
        case Value::Type::Real:
        {
            const double d = *value.get<double>();
            leaf("real", value.asString(), d == 0.0 && !std::signbit(d));
            break;
        }
        case Value::Type::String:
        {
            const auto& s = *value.get<std::string>();
            leaf("string", s, s.empty());
            break;
        }
        case Value::Type::Uuid:
        {
            const auto& id = *value.get<Uuid>();
            leaf("uuid", id.toString(), id.isNull());
            break;
        }
        case Value::Type::Date:
            leaf("date", formatDate(*value.get<Date>()), false);
            break;
        case Value::Type::Uri:
        {
            const auto& u = value.get<Uri>()->value;
            leaf("uri", u, u.empty());
            break;
        }
        case Value::Type::Binary:
        {
            const auto& b = *value.get<Binary>();
            leaf("binary", detail::encodeBase64(b), b.empty());
            break;
        }
        case Value::Type::Map:
        {
            const bool compactContainer = !options_.pretty || value.get<Map>()->empty();
            printer_.OpenElement("map", compactContainer);
            for(const auto& [key, entry] : *value.get<Map>())
            {
                printer_.OpenElement("key", true);
                printer_.PushText(key.c_str());
                printer_.CloseElement(true);
                write(entry);
            }
            printer_.CloseElement(compactContainer);
            break;
        }
        case Value::Type::Array:
        {
            const bool compactContainer = !options_.pretty || value.get<Array>()->empty();
            printer_.OpenElement("array", compactContainer);
            for(const auto& entry : *value.get<Array>())
            {
                write(entry);
            }
            printer_.CloseElement(compactContainer);
            break;
        }
        }
    }

    XmlWriteOptions options_;
    tinyxml2::XMLPrinter printer_;
};

} // namespace

std::string toXml(const Value& value, const XmlWriteOptions& options)
{
    return XmlWriter(options).finish(value);
}

std::expected<Value, std::string> fromXml(std::string_view xml)
{
    // Pedantic whitespace keeps whitespace-only <string> values intact.
    tinyxml2::XMLDocument document(true, tinyxml2::PEDANTIC_WHITESPACE);
    if(document.Parse(xml.data(), xml.size()) != tinyxml2::XML_SUCCESS)
    {
        return std::unexpected(std::format("LLSD+XML: {}", document.ErrorStr()));
    }
    const XMLElement* root = document.RootElement();
    if(!root || std::string_view(root->Name()) != "llsd")
    {
        return std::unexpected(std::string("LLSD+XML: root element is not <llsd>"));
    }
    const XMLElement* first = root->FirstChildElement();
    if(!first)
    {
        return Value();
    }
    return parseElement(*first, 0);
}

std::optional<Format> detectFormat(std::string_view data)
{
    // Skip a UTF-8 byte order mark and leading whitespace.
    if(data.starts_with("\xEF\xBB\xBF"))
    {
        data.remove_prefix(3);
    }
    data = trim(data);

    const std::string head = lowercase(data.substr(0, 32));
    if(head.starts_with("<?llsd/notation?>"))
    {
        return Format::Notation;
    }
    if(head.starts_with("<?llsd/binary?>"))
    {
        return Format::Binary;
    }
    if(head.starts_with("<?xml") || head.starts_with("<llsd"))
    {
        return Format::Xml;
    }
    return std::nullopt;
}

std::expected<Value, std::string> parse(std::string_view data)
{
    const auto format = detectFormat(data);
    if(!format)
    {
        return std::unexpected(std::string("LLSD: unrecognized serialization format"));
    }
    switch(*format)
    {
    case Format::Xml:
        return fromXml(data);
    case Format::Notation:
        return std::unexpected(std::string("LLSD: notation format is not supported yet"));
    case Format::Binary:
        return std::unexpected(std::string("LLSD: binary format is not supported yet"));
    }
    return std::unexpected(std::string("LLSD: unrecognized serialization format"));
}

} // namespace viewer::llsd
