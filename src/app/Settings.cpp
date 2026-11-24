#include "Settings.hpp"

#include "SavedLogins.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/llsd/Serialization.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace viewer
{

namespace
{

std::string_view trimmed(std::string_view s)
{
    while(!s.empty() && (s.front() == ' ' || s.front() == '\t'))
    {
        s.remove_prefix(1);
    }
    while(!s.empty() && (s.back() == ' ' || s.back() == '\t'))
    {
        s.remove_suffix(1);
    }
    return s;
}

bool hasMinimum(const SettingDefinition& d)
{
    return d.minimum > std::numeric_limits<double>::lowest();
}

bool hasMaximum(const SettingDefinition& d)
{
    return d.maximum < std::numeric_limits<double>::max();
}

std::string number(const SettingDefinition& d, double v)
{
    return d.type == SettingType::Integer ? std::format("{}", static_cast<int64_t>(v))
                                          : std::format("{:.{}f}", v, d.decimals);
}

// "From 0 to 100", "At least 5", ... (empty: any number).
std::string rangeText(const SettingDefinition& d)
{
    if(hasMinimum(d) && hasMaximum(d))
    {
        return std::format("from {} to {}", number(d, d.minimum), number(d, d.maximum));
    }
    if(hasMinimum(d))
    {
        return std::format("at least {}", number(d, d.minimum));
    }
    if(hasMaximum(d))
    {
        return std::format("at most {}", number(d, d.maximum));
    }
    return {};
}

std::string rangeError(const SettingDefinition& d, std::string_view what)
{
    const std::string range = rangeText(d);
    return range.empty() ? std::format("Must be {}.", what)
                         : std::format("Must be {}, {}.", what, range);
}

} // namespace

Settings::Settings(std::vector<SettingDefinition> definitions, fs::path file)
    : definitions_(std::move(definitions)), file_(std::move(file))
{
    entries_.reserve(definitions_.size());
    for(const SettingDefinition& d : definitions_)
    {
        const llsd::Value fallback = normalize(d, d.fallback);
        if(fallback.isUndefined())
        {
            log::warn("setting {}: its default isn't a value it takes", d.name);
        }
        entries_.push_back({&d, d.fallback});
    }
    load();
}

Settings::~Settings()
{
    if(unsaved_)
    {
        save();
    }
}

fs::path Settings::defaultFile()
{
    return SavedLogins::defaultListFile().parent_path() / "settings.llsd";
}

const SettingDefinition* Settings::definition(std::string_view name) const
{
    const Entry* e = entry(name);
    return e ? e->definition : nullptr;
}

Settings::Entry* Settings::entry(std::string_view name)
{
    const auto it =
        std::ranges::find_if(entries_, [&](const Entry& e) { return e.definition->name == name; });
    return it == entries_.end() ? nullptr : &*it;
}

const Settings::Entry* Settings::entry(std::string_view name) const
{
    return const_cast<Settings*>(this)->entry(name);
}

const llsd::Value& Settings::value(std::string_view name) const
{
    static const llsd::Value kNone;
    const Entry* e = entry(name);
    return e ? e->value : kNone;
}

llsd::Value Settings::normalize(const SettingDefinition& d, const llsd::Value& v)
{
    using T = llsd::Value::Type;
    switch(d.type)
    {
    case SettingType::Boolean:
        if(v.type() == T::Boolean || v.type() == T::Integer)
        {
            return llsd::Value(v.asBoolean());
        }
        return {};
    case SettingType::Integer:
    {
        if(v.type() != T::Integer && v.type() != T::Real)
        {
            return {};
        }
        const double n = std::round(v.asReal());
        if(!std::isfinite(n))
        {
            return {};
        }
        const double lowest = std::max(d.minimum, double(std::numeric_limits<int32_t>::min()));
        const double highest = std::min(d.maximum, double(std::numeric_limits<int32_t>::max()));
        return llsd::Value(static_cast<int32_t>(std::clamp(n, lowest, highest)));
    }
    case SettingType::Real:
    {
        if(v.type() != T::Integer && v.type() != T::Real)
        {
            return {};
        }
        const double n = v.asReal();
        if(!std::isfinite(n))
        {
            return {};
        }
        return llsd::Value(std::clamp(n, d.minimum, d.maximum));
    }
    case SettingType::String:
    {
        if(v.type() != T::String)
        {
            return {};
        }
        std::string s = v.asString();
        if(d.maxLength > 0 && s.size() > d.maxLength)
        {
            return {};
        }
        if(d.check && !d.check(s).empty())
        {
            return {};
        }
        return llsd::Value(std::move(s));
    }
    case SettingType::Uuid:
    {
        if(v.type() != T::Uuid && v.type() != T::String)
        {
            return {};
        }
        Uuid id;
        if(v.type() == T::Uuid)
        {
            id = v.asUuid();
        }
        else if(const std::string s = v.asString(); !s.empty())
        {
            const auto parsed = Uuid::parse(s);
            if(!parsed)
            {
                return {};
            }
            id = *parsed;
        }
        if(id.isNull() && !d.allowNull)
        {
            return {};
        }
        return llsd::Value(id);
    }
    case SettingType::Choice:
    {
        if(v.type() != T::String)
        {
            return {};
        }
        const std::string s = v.asString();
        const bool known =
            std::ranges::any_of(d.choices, [&](const SettingChoice& c) { return c.value == s; });
        return known ? llsd::Value(s) : llsd::Value();
    }
    }
    return {};
}

bool Settings::setValue(std::string_view name, llsd::Value value)
{
    Entry* e = entry(name);
    if(!e)
    {
        return false;
    }
    llsd::Value v = normalize(*e->definition, value);
    if(v.isUndefined())
    {
        return false;
    }
    if(v != e->value)
    {
        e->value = std::move(v);
        e->overridden = false; // changed here: this is kept
        changed(*e);
    }
    return true;
}

std::string Settings::text(std::string_view name) const
{
    const Entry* e = entry(name);
    if(!e)
    {
        return {};
    }
    const SettingDefinition& d = *e->definition;
    switch(d.type)
    {
    case SettingType::Boolean:
        return e->value.asBoolean() ? "true" : "false";
    case SettingType::Integer:
        return std::to_string(e->value.asInteger());
    case SettingType::Real:
        return number(d, e->value.asReal());
    case SettingType::Uuid:
        return e->value.asUuid().isNull() ? std::string() : e->value.asUuid().toString();
    case SettingType::String:
    case SettingType::Choice:
        return e->value.asString();
    }
    return {};
}

std::expected<llsd::Value, std::string> Settings::fromText(const SettingDefinition& d,
                                                           std::string_view typed)
{
    llsd::Value v;
    switch(d.type)
    {
    case SettingType::Boolean:
    {
        const std::string_view t = trimmed(typed);
        if(t != "true" && t != "false" && t != "1" && t != "0")
        {
            return std::unexpected(std::string("Must be true or false."));
        }
        v = t == "true" || t == "1";
        break;
    }
    case SettingType::Integer:
    {
        const std::string_view t = trimmed(typed);
        int64_t n = 0;
        const auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), n);
        if(t.empty() || ec != std::errc() || end != t.data() + t.size())
        {
            return std::unexpected(rangeError(d, "a whole number"));
        }
        if(static_cast<double>(n) < d.minimum || static_cast<double>(n) > d.maximum ||
           n < std::numeric_limits<int32_t>::min() || n > std::numeric_limits<int32_t>::max())
        {
            return std::unexpected(rangeError(d, "a whole number"));
        }
        v = n;
        break;
    }
    case SettingType::Real:
    {
        const std::string_view t = trimmed(typed);
        double n = 0.0;
        const auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), n);
        if(t.empty() || ec != std::errc() || end != t.data() + t.size() || !std::isfinite(n))
        {
            return std::unexpected(rangeError(d, "a number"));
        }
        if(n < d.minimum || n > d.maximum)
        {
            return std::unexpected(rangeError(d, "a number"));
        }
        v = n;
        break;
    }
    case SettingType::String:
    {
        if(d.maxLength > 0 && typed.size() > d.maxLength)
        {
            return std::unexpected(std::format("Must be at most {} characters.", d.maxLength));
        }
        if(d.check)
        {
            if(std::string wrong = d.check(typed); !wrong.empty())
            {
                return std::unexpected(std::move(wrong));
            }
        }
        v = std::string(typed);
        break;
    }
    case SettingType::Uuid:
    {
        const std::string_view t = trimmed(typed);
        if(t.empty())
        {
            if(!d.allowNull)
            {
                return std::unexpected(std::string("An id is needed."));
            }
            v = Uuid();
            break;
        }
        const auto id = Uuid::parse(t);
        if(!id)
        {
            return std::unexpected(
                std::string("Must be an id, like 01234567-89ab-cdef-0123-456789abcdef."));
        }
        if(id->isNull() && !d.allowNull)
        {
            return std::unexpected(std::string("An id is needed."));
        }
        v = *id;
        break;
    }
    case SettingType::Choice:
    {
        const std::string_view t = trimmed(typed);
        const auto it = std::ranges::find_if(d.choices, [&](const SettingChoice& c)
                                             { return c.value == t || c.label == t; });
        if(it == d.choices.end())
        {
            return std::unexpected(std::string("Must be one of its choices."));
        }
        v = it->value;
        break;
    }
    }
    llsd::Value normal = normalize(d, v);
    if(normal.isUndefined())
    {
        return std::unexpected(std::string("Not a value it takes."));
    }
    return normal;
}

std::string Settings::setText(std::string_view name, std::string_view typed)
{
    const Entry* e = entry(name);
    if(!e)
    {
        return "There's no such setting.";
    }
    auto v = fromText(*e->definition, typed);
    if(!v)
    {
        return v.error();
    }
    setValue(name, std::move(*v));
    return {};
}

std::string Settings::override(std::string_view name, std::string_view typed)
{
    Entry* e = entry(name);
    if(!e)
    {
        return std::format("there's no setting {}", name);
    }
    auto v = fromText(*e->definition, typed);
    if(!v)
    {
        return v.error();
    }
    if(!e->overridden)
    {
        e->kept = e->value;
        e->overridden = true;
    }
    e->value = std::move(*v);
    return {};
}

bool Settings::isOverridden(std::string_view name) const
{
    const Entry* e = entry(name);
    return e && e->overridden;
}

bool Settings::isDefault(std::string_view name) const
{
    const Entry* e = entry(name);
    return !e || e->value == e->definition->fallback;
}

void Settings::reset(std::string_view name)
{
    if(Entry* e = entry(name); e && e->value != e->definition->fallback)
    {
        e->value = e->definition->fallback;
        e->overridden = false;
        changed(*e);
    }
}

void Settings::changed(const Entry& e)
{
    version_++;
    if(std::ranges::find(changed_, e.definition->name) == changed_.end())
    {
        changed_.push_back(e.definition->name);
    }
    unsaved_ = true;
    changedAt_ = -1.0; // seen by the next saveIfChanged
}

void Settings::load()
{
    std::ifstream in(file_, std::ios::binary);
    if(!in)
    {
        return; // none yet: the defaults
    }
    std::stringstream text;
    text << in.rdbuf();
    // Notation, header or not (as written, or by hand); XML or binary too.
    const std::string data = text.str();
    const auto parsed = llsd::detectFormat(data) ? llsd::parse(data) : llsd::fromNotation(data);
    if(!parsed || !parsed->isMap())
    {
        log::warn("settings: {} couldn't be read ({}); using the defaults", file_.string(),
                  parsed ? std::string("not a map") : parsed.error());
        return;
    }
    for(const auto& [name, stored] : *parsed->get<llsd::Map>())
    {
        Entry* e = entry(name);
        if(!e)
        {
            unknown_.set(name, stored);
            continue;
        }
        llsd::Value v = normalize(*e->definition, stored);
        if(v.isUndefined())
        {
            log::warn("settings: {} isn't a value {} takes; using its default",
                      llsd::toNotation(stored), name);
            continue;
        }
        e->value = std::move(v);
    }
}

void Settings::saveIfChanged(double now, double delay)
{
    if(!unsaved_)
    {
        return;
    }
    if(changedAt_ < 0.0)
    {
        changedAt_ = now;
    }
    if(now - changedAt_ >= delay)
    {
        save();
    }
}

bool Settings::save()
{
    llsd::Map out;
    for(const Entry& e : entries_)
    {
        // An override isn't saved: the value it was over is.
        const llsd::Value& v = e.overridden ? e.kept : e.value;
        if(v != e.definition->fallback)
        {
            out.set(e.definition->name, v);
        }
    }
    for(const auto& [name, value] : unknown_)
    {
        out.set(name, value);
    }
    const std::string text =
        llsd::toNotation(llsd::Value(std::move(out)), {.pretty = true, .header = true});

    // Into a new file, then over the old one, so a crash can't leave half.
    std::error_code error;
    fs::create_directories(file_.parent_path(), error);
    fs::path temporary = file_;
    temporary += ".new";
    {
        std::ofstream o(temporary, std::ios::binary | std::ios::trunc);
        o << text;
        if(!o)
        {
            log::warn("settings: couldn't write {}", temporary.string());
            return false;
        }
    }
    fs::rename(temporary, file_, error);
    if(error)
    {
        log::warn("settings: couldn't replace {}: {}", file_.string(), error.message());
        return false;
    }
    unsaved_ = false;
    return true;
}

} // namespace viewer
