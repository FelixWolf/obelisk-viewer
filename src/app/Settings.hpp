#pragma once

// The viewer's settings: each defined in C++ (ViewerSettings.cpp) with its
// type, default, range and how the Settings tab shows it, and kept in
// settings.llsd (LLSD notation, pretty printed) beside the saved accounts'
// list. Only values that differ from their defaults are written, so a new
// default reaches everyone who didn't choose otherwise; names the viewer
// doesn't know (from a newer one) are kept as they were.
//
// Values are read and set through typed keys (SettingKey<bool> and so on);
// a value set is made one the setting takes (clamped to its range, one of
// its choices), and the app asks what changed (takeChanged()) to apply it.

#include "viewer/core/Uuid.hpp"
#include "viewer/llsd/Value.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace viewer
{

enum class SettingType : uint8_t
{
    Boolean, // a check box
    Integer, // a number field (whole), with - and +
    Real,    // a number field, with - and +
    String,  // a text field
    Uuid,    // a text field taking an id (empty: the null id, if allowed)
    Choice,  // one of a few strings, as keys side by side
};

// A setting's name, and the type it's read and set as: bool, int32_t,
// double, std::string (strings and choices) or Uuid.
template <typename T>
struct SettingKey
{
    std::string_view name;
};

struct SettingChoice
{
    std::string value; // as stored
    std::string label; // as shown
};

struct SettingDefinition
{
    std::string name{}; // its key in the file: "graphics.shadows"
    SettingType type = SettingType::Boolean;
    llsd::Value fallback{}; // its default
    // Where the Settings tab shows it: the page, a heading on it (empty:
    // under the one before), its label and what it does.
    std::string page{};
    std::string section{};
    std::string label{};
    std::string description{};
    // Numbers: the range, the step of - and +, the unit shown after the
    // field ("%", "MiB") and the decimals shown (reals).
    double minimum = std::numeric_limits<double>::lowest();
    double maximum = std::numeric_limits<double>::max();
    double step = 1.0;
    std::string unit{};
    int decimals = 1;
    // Choices: those it may be.
    std::vector<SettingChoice> choices{};
    // Strings: the longest it may be (0: any). Ids: whether it may be none.
    size_t maxLength = 0;
    bool allowNull = true;
    // A further check of a string: what's wrong with it (empty: nothing).
    std::function<std::string(std::string_view)> check{};
    // Only read when the viewer starts.
    bool restart = false;
    // Not on the Settings tab: set in the file, or for a run with
    // --setting NAME VALUE (internal and testing settings).
    bool hidden = false;
};

// A definition for `key`, its type from the key's (and Choice if it has
// choices) and `fallback` its default.
template <typename T>
SettingDefinition defineSetting(SettingKey<T> key, T fallback, SettingDefinition definition);

class Settings
{
public:
    // Reads `file` (if it's there) into the definitions' defaults.
    Settings(std::vector<SettingDefinition> definitions, std::filesystem::path file);
    // Writes what's not been written yet.
    ~Settings();

    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

    // settings.llsd, beside the saved accounts' list (SavedLogins).
    static std::filesystem::path defaultFile();
    const std::filesystem::path& file() const { return file_; }

    const std::vector<SettingDefinition>& definitions() const { return definitions_; }
    // Null if there's no such setting.
    const SettingDefinition* definition(std::string_view name) const;

    template <typename T>
    T get(SettingKey<T> key) const;
    // False (and nothing changed) if there's no such setting, or it isn't
    // of this type.
    template <typename T>
    bool set(SettingKey<T> key, T value);

    // By name: its value (undefined if there's no such setting).
    const llsd::Value& value(std::string_view name) const;
    // Made a value the setting takes (converted, clamped); false if it can't
    // be (or there's no such setting).
    bool setValue(std::string_view name, llsd::Value value);
    // What a field shows of it: "2048", "0.5", the string, the id ("" for
    // none), "true"/"false".
    std::string text(std::string_view name) const;
    // From what was typed into its field: what's wrong with it (nothing
    // changed), or empty if it's set.
    std::string setText(std::string_view name, std::string_view text);
    bool isDefault(std::string_view name) const;
    // For this run only (the command line's --setting): as setText, but
    // not saved, and not a change (nothing's running yet). Changing it
    // afterwards (setValue, reset) keeps the change instead.
    std::string override(std::string_view name, std::string_view text);
    bool isOverridden(std::string_view name) const;
    void reset(std::string_view name);

    // Counts changes (to see if any happened); and the names of those that
    // changed since last asked, in the order they did.
    uint64_t version() const { return version_; }
    std::vector<std::string> takeChanged() { return std::exchange(changed_, {}); }

    // Writes the file if something changed at least `delay` seconds before
    // `now` (seconds, any clock), so a run of changes is one write.
    void saveIfChanged(double now, double delay = 1.0);
    // Writes it now (if anything changed since it was); false if it couldn't.
    bool save();

private:
    struct Entry
    {
        const SettingDefinition* definition;
        llsd::Value value;
        // Overridden for this run (override()): `kept` is what's saved.
        bool overridden = false;
        llsd::Value kept{};
    };
    static std::expected<llsd::Value, std::string> fromText(const SettingDefinition& definition,
                                                            std::string_view text);
    Entry* entry(std::string_view name);
    const Entry* entry(std::string_view name) const;
    // `value` made one the setting takes; undefined if it can't be.
    static llsd::Value normalize(const SettingDefinition& definition, const llsd::Value& value);
    void load();
    void changed(const Entry& e);

    std::vector<SettingDefinition> definitions_;
    std::vector<Entry> entries_;
    llsd::Map unknown_; // read, not defined (kept)
    std::filesystem::path file_;
    uint64_t version_ = 0;
    std::vector<std::string> changed_;
    bool unsaved_ = false;
    double changedAt_ = -1.0; // when the unsaved change was seen (saveIfChanged's clock)
};

namespace detail
{

template <typename T>
constexpr SettingType settingType()
{
    if constexpr(std::is_same_v<T, bool>)
    {
        return SettingType::Boolean;
    }
    else if constexpr(std::is_same_v<T, int32_t>)
    {
        return SettingType::Integer;
    }
    else if constexpr(std::is_same_v<T, double>)
    {
        return SettingType::Real;
    }
    else if constexpr(std::is_same_v<T, std::string>)
    {
        return SettingType::String;
    }
    else
    {
        static_assert(std::is_same_v<T, Uuid>,
                      "settings are bool, int32_t, double, string or Uuid");
        return SettingType::Uuid;
    }
}

} // namespace detail

template <typename T>
SettingDefinition defineSetting(SettingKey<T> key, T fallback, SettingDefinition definition)
{
    definition.name = std::string(key.name);
    definition.type = detail::settingType<T>();
    if(definition.type == SettingType::String && !definition.choices.empty())
    {
        definition.type = SettingType::Choice;
    }
    definition.fallback = llsd::Value(std::move(fallback));
    return definition;
}

template <typename T>
T Settings::get(SettingKey<T> key) const
{
    const llsd::Value& v = value(key.name);
    if constexpr(std::is_same_v<T, bool>)
    {
        return v.asBoolean();
    }
    else if constexpr(std::is_same_v<T, int32_t>)
    {
        return v.asInteger();
    }
    else if constexpr(std::is_same_v<T, double>)
    {
        return v.asReal();
    }
    else if constexpr(std::is_same_v<T, std::string>)
    {
        return v.asString();
    }
    else
    {
        return v.asUuid();
    }
}

template <typename T>
bool Settings::set(SettingKey<T> key, T value)
{
    const SettingDefinition* d = definition(key.name);
    const SettingType type = detail::settingType<T>();
    if(!d || (d->type != type && !(d->type == SettingType::Choice && type == SettingType::String)))
    {
        return false;
    }
    return setValue(key.name, llsd::Value(std::move(value)));
}

} // namespace viewer
