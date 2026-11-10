#pragma once

// LLSD ("Linden Lab Structured Data") value: the dynamically typed tree used by
// login, capabilities, the event queue and many assets.
// Spec: https://wiki.secondlife.com/wiki/LLSD

#include "viewer/core/Uuid.hpp"

#include <chrono>
#include <concepts>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace viewer::llsd
{

class Value;

// UTC, microsecond precision.
using Date = std::chrono::sys_time<std::chrono::microseconds>;
using Binary = std::vector<uint8_t>;
using Array = std::vector<Value>;

struct Uri
{
    std::string value;
    bool operator==(const Uri&) const = default;
};

// String-keyed map that keeps insertion order (useful when debugging and when
// round-tripping). Lookup is linear; LLSD maps are small.
class Map
{
public:
    using Entry = std::pair<std::string, Value>;
    using iterator = std::vector<Entry>::iterator;
    using const_iterator = std::vector<Entry>::const_iterator;

    Map() = default;
    Map(std::initializer_list<Entry> entries);

    Value* find(std::string_view key);
    const Value* find(std::string_view key) const;
    bool contains(std::string_view key) const { return find(key) != nullptr; }

    // Returns the value for `key`, inserting an undefined value if missing.
    Value& operator[](std::string_view key);
    void set(std::string key, Value value);
    bool erase(std::string_view key);

    size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }
    iterator begin() { return entries_.begin(); }
    iterator end() { return entries_.end(); }
    const_iterator begin() const { return entries_.begin(); }
    const_iterator end() const { return entries_.end(); }

    // Order-insensitive, like LLSD map semantics.
    bool operator==(const Map& other) const;

private:
    std::vector<Entry> entries_;
};

class Value
{
public:
    // Scoped enumerators can't clash with the Date/Binary/Array types, but
    // GCC's -Wshadow reports them anyway.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#endif
    enum class Type : uint8_t
    {
        Undefined,
        Boolean,
        Integer,
        Real,
        String,
        Uuid,
        Date,
        Uri,
        Binary,
        Map,
        Array,
    };
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

    Value() = default;
    Value(bool v) : data_(v) {}
    // LLSD integers are 32-bit signed; wider values are truncated (U32 flag
    // fields keep their bit pattern).
    template <std::integral T>
        requires(!std::same_as<T, bool>)
    Value(T v) : data_(static_cast<int32_t>(v))
    {
    }
    template <std::floating_point T>
    Value(T v) : data_(static_cast<double>(v))
    {
    }
    Value(std::string v) : data_(std::move(v)) {}
    Value(std::string_view v) : data_(std::string(v)) {}
    Value(const char* v) : data_(std::string(v)) {}
    Value(viewer::Uuid v) : data_(v) {}
    Value(Date v) : data_(v) {}
    Value(Uri v) : data_(std::move(v)) {}
    Value(Binary v) : data_(std::move(v)) {}
    Value(Map v) : data_(std::move(v)) {}
    Value(Array v) : data_(std::move(v)) {}
    Value(std::nullptr_t) = delete; // would otherwise silently become a bool

    Type type() const { return static_cast<Type>(data_.index()); }
    bool isUndefined() const { return type() == Type::Undefined; }
    bool isMap() const { return type() == Type::Map; }
    bool isArray() const { return type() == Type::Array; }

    // Conversions follow LLSD rules: any type can be read as any other,
    // falling back to that type's default (false, 0, "", null UUID, epoch...).
    bool asBoolean() const;
    int32_t asInteger() const;
    double asReal() const;
    std::string asString() const;
    viewer::Uuid asUuid() const;
    Date asDate() const;
    Uri asUri() const;
    Binary asBinary() const;

    // Typed pointer access without conversion; nullptr if the type differs.
    template <typename T>
    const T* get() const
    {
        return std::get_if<T>(&data_);
    }
    template <typename T>
    T* get()
    {
        return std::get_if<T>(&data_);
    }

    // --- Map access ---
    // Const lookup: an undefined value if this isn't a map or the key is missing.
    const Value& operator[](std::string_view key) const;
    const Value& operator[](const char* key) const { return (*this)[std::string_view(key)]; }
    // Mutable lookup: turns this value into a map if it isn't one, then
    // returns the entry (inserted as undefined if missing).
    Value& operator[](std::string_view key);
    Value& operator[](const char* key) { return (*this)[std::string_view(key)]; }
    bool has(std::string_view key) const;

    // --- Array access ---
    // Const: an undefined value if not an array or out of range.
    // (Templates so that v[0] is an exact match rather than ambiguous with
    // the const char* key overload.)
    template <std::integral T>
        requires(!std::same_as<T, bool>)
    const Value& operator[](T index) const
    {
        return at(static_cast<size_t>(index));
    }
    // Mutable: turns this value into an array if needed and grows it to fit.
    template <std::integral T>
        requires(!std::same_as<T, bool>)
    Value& operator[](T index)
    {
        return at(static_cast<size_t>(index));
    }
    const Value& at(size_t index) const;
    Value& at(size_t index);
    void append(Value value);

    // Number of entries in a map or array; 0 for everything else.
    size_t size() const;

    // The map/array itself. Converts an undefined value to an empty container;
    // a value of any other type is replaced.
    Map& map();
    Array& array();

    bool operator==(const Value& other) const = default;

private:
    // Order must match Type.
    std::variant<std::monostate, bool, int32_t, double, std::string, viewer::Uuid, Date, Uri,
                 Binary, Map, Array>
        data_;
};

// "2006-02-01T14:29:53.430000Z"
std::string formatDate(Date date);
// Accepts "YYYY-MM-DDTHH:MM:SS[.fraction][Z]".
std::optional<Date> parseDate(std::string_view text);

} // namespace viewer::llsd
