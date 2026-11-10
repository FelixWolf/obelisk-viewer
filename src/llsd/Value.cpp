#include "viewer/llsd/Value.hpp"

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

const Value& undefinedValue()
{
    static const Value kUndefined;
    return kUndefined;
}

std::string formatReal(double v)
{
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), v);
    return std::string(buffer, result.ptr);
}

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

template <typename T>
std::optional<T> parseNumber(std::string_view text)
{
    text = trim(text);
    if(!text.empty() && text.front() == '+')
    {
        text.remove_prefix(1);
    }
    T value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if(result.ec != std::errc() || result.ptr != text.data() + text.size())
    {
        return std::nullopt;
    }
    return value;
}

int32_t clampToInt32(double v)
{
    if(std::isnan(v))
    {
        return 0;
    }
    v = std::trunc(v);
    if(v <= static_cast<double>(std::numeric_limits<int32_t>::min()))
    {
        return std::numeric_limits<int32_t>::min();
    }
    if(v >= static_cast<double>(std::numeric_limits<int32_t>::max()))
    {
        return std::numeric_limits<int32_t>::max();
    }
    return static_cast<int32_t>(v);
}

} // namespace

// --- Map ---------------------------------------------------------------------

Map::Map(std::initializer_list<Entry> entries)
{
    for(const auto& entry : entries)
    {
        set(entry.first, entry.second);
    }
}

Value* Map::find(std::string_view key)
{
    for(auto& entry : entries_)
    {
        if(entry.first == key)
        {
            return &entry.second;
        }
    }
    return nullptr;
}

const Value* Map::find(std::string_view key) const
{
    return const_cast<Map*>(this)->find(key);
}

Value& Map::operator[](std::string_view key)
{
    if(Value* existing = find(key))
    {
        return *existing;
    }
    entries_.emplace_back(std::string(key), Value());
    return entries_.back().second;
}

void Map::set(std::string key, Value value)
{
    if(Value* existing = find(key))
    {
        *existing = std::move(value);
        return;
    }
    entries_.emplace_back(std::move(key), std::move(value));
}

bool Map::erase(std::string_view key)
{
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [&](const Entry& e) { return e.first == key; });
    if(it == entries_.end())
    {
        return false;
    }
    entries_.erase(it);
    return true;
}

bool Map::operator==(const Map& other) const
{
    if(entries_.size() != other.entries_.size())
    {
        return false;
    }
    for(const auto& [key, value] : entries_)
    {
        const Value* theirs = other.find(key);
        if(!theirs || !(*theirs == value))
        {
            return false;
        }
    }
    return true;
}

// --- Conversions -------------------------------------------------------------------

bool Value::asBoolean() const
{
    switch(type())
    {
    case Type::Boolean:
        return std::get<bool>(data_);
    case Type::Integer:
        return std::get<int32_t>(data_) != 0;
    case Type::Real:
    {
        const double v = std::get<double>(data_);
        return v != 0.0 && !std::isnan(v);
    }
    case Type::String:
        return !std::get<std::string>(data_).empty();
    default:
        return false;
    }
}

int32_t Value::asInteger() const
{
    switch(type())
    {
    case Type::Boolean:
        return std::get<bool>(data_) ? 1 : 0;
    case Type::Integer:
        return std::get<int32_t>(data_);
    case Type::Real:
        return clampToInt32(std::get<double>(data_));
    case Type::String:
    {
        const auto& s = std::get<std::string>(data_);
        if(auto i = parseNumber<int64_t>(s))
        {
            return clampToInt32(static_cast<double>(*i));
        }
        if(auto d = parseNumber<double>(s))
        {
            return clampToInt32(*d);
        }
        return 0;
    }
    default:
        return 0;
    }
}

double Value::asReal() const
{
    switch(type())
    {
    case Type::Boolean:
        return std::get<bool>(data_) ? 1.0 : 0.0;
    case Type::Integer:
        return std::get<int32_t>(data_);
    case Type::Real:
        return std::get<double>(data_);
    case Type::String:
        return parseNumber<double>(std::get<std::string>(data_)).value_or(0.0);
    default:
        return 0.0;
    }
}

std::string Value::asString() const
{
    switch(type())
    {
    case Type::Boolean:
        return std::get<bool>(data_) ? "true" : "";
    case Type::Integer:
        return std::to_string(std::get<int32_t>(data_));
    case Type::Real:
        return formatReal(std::get<double>(data_));
    case Type::String:
        return std::get<std::string>(data_);
    case Type::Uuid:
        return std::get<viewer::Uuid>(data_).toString();
    case Type::Date:
        return formatDate(std::get<Date>(data_));
    case Type::Uri:
        return std::get<Uri>(data_).value;
    default:
        return {};
    }
}

viewer::Uuid Value::asUuid() const
{
    if(const auto* id = get<viewer::Uuid>())
    {
        return *id;
    }
    if(const auto* s = get<std::string>())
    {
        return viewer::Uuid::parse(*s).value_or(viewer::Uuid{});
    }
    return {};
}

Date Value::asDate() const
{
    if(const auto* d = get<Date>())
    {
        return *d;
    }
    if(const auto* s = get<std::string>())
    {
        return parseDate(*s).value_or(Date{});
    }
    if(type() == Type::Integer || type() == Type::Real)
    {
        // Seconds since the epoch.
        return Date(std::chrono::microseconds(static_cast<int64_t>(asReal() * 1'000'000.0)));
    }
    return {};
}

Uri Value::asUri() const
{
    if(const auto* u = get<Uri>())
    {
        return *u;
    }
    if(const auto* s = get<std::string>())
    {
        return Uri{*s};
    }
    return {};
}

Binary Value::asBinary() const
{
    if(const auto* b = get<Binary>())
    {
        return *b;
    }
    return {};
}

// --- Containers ------------------------------------------------------------------

const Value& Value::operator[](std::string_view key) const
{
    if(const auto* m = get<Map>())
    {
        if(const Value* v = m->find(key))
        {
            return *v;
        }
    }
    return undefinedValue();
}

Value& Value::operator[](std::string_view key)
{
    return map()[key];
}

bool Value::has(std::string_view key) const
{
    const auto* m = get<Map>();
    return m && m->contains(key);
}

const Value& Value::at(size_t index) const
{
    if(const auto* a = get<Array>())
    {
        if(index < a->size())
        {
            return (*a)[index];
        }
    }
    return undefinedValue();
}

Value& Value::at(size_t index)
{
    Array& a = array();
    if(index >= a.size())
    {
        a.resize(index + 1);
    }
    return a[index];
}

void Value::append(Value value)
{
    array().push_back(std::move(value));
}

size_t Value::size() const
{
    if(const auto* m = get<Map>())
    {
        return m->size();
    }
    if(const auto* a = get<Array>())
    {
        return a->size();
    }
    return 0;
}

Map& Value::map()
{
    if(!isMap())
    {
        data_ = Map();
    }
    return std::get<Map>(data_);
}

Array& Value::array()
{
    if(!isArray())
    {
        data_ = Array();
    }
    return std::get<Array>(data_);
}

// --- Dates -----------------------------------------------------------------------

std::string formatDate(Date date)
{
    return std::format("{:%Y-%m-%dT%H:%M:%S}Z", date);
}

std::optional<Date> parseDate(std::string_view text)
{
    text = trim(text);
    if(!text.empty() && (text.back() == 'Z' || text.back() == 'z'))
    {
        text.remove_suffix(1);
    }
    // YYYY-MM-DDTHH:MM:SS
    if(text.size() < 19 || text[4] != '-' || text[7] != '-' ||
       (text[10] != 'T' && text[10] != 't') || text[13] != ':' || text[16] != ':')
    {
        return std::nullopt;
    }
    const auto field = [&](size_t at, size_t length)
    { return parseNumber<int>(text.substr(at, length)); };
    const auto year = field(0, 4);
    const auto month = field(5, 2);
    const auto day = field(8, 2);
    const auto hour = field(11, 2);
    const auto minute = field(14, 2);
    const auto second = field(17, 2);
    if(!year || !month || !day || !hour || !minute || !second)
    {
        return std::nullopt;
    }

    int64_t micros = 0;
    if(text.size() > 19)
    {
        if(text[19] != '.')
        {
            return std::nullopt;
        }
        // Fractional seconds: keep up to 6 digits, right-padded.
        const std::string_view fraction = text.substr(20);
        if(fraction.empty())
        {
            return std::nullopt;
        }
        int64_t scale = 100'000;
        for(char c : fraction)
        {
            if(c < '0' || c > '9')
            {
                return std::nullopt;
            }
            micros += (c - '0') * scale;
            scale /= 10;
        }
    }

    using namespace std::chrono;
    const year_month_day ymd{std::chrono::year(*year),
                             std::chrono::month(static_cast<unsigned>(*month)),
                             std::chrono::day(static_cast<unsigned>(*day))};
    if(!ymd.ok() || *hour > 23 || *minute > 59 || *second > 60)
    {
        return std::nullopt;
    }
    return time_point_cast<microseconds>(sys_days(ymd)) + hours(*hour) + minutes(*minute) +
           seconds(*second) + microseconds(micros);
}

} // namespace viewer::llsd
