#pragma once

// The main menu's Settings tab: the settings' pages on the left (their
// definitions' pages, in order; hidden settings aren't shown), the chosen page's settings on the
// right, scrolling, under their sections' headings. Each as its type has it: a check box
// (booleans), a number field with - and + (whole numbers and reals), a text field (strings and
// ids), keys side by side (choices), or one over the other (listed).
//
// What's typed is checked as it's typed: a value the setting takes is set
// at once (and saved a moment later); one it doesn't is said under the
// field, and the setting keeps its value. Leaving the field shows the
// value as it's kept. In a headset the fields aren't typed into: a key
// asks for the VR keyboard (takeKeyboard()), and what's typed on it comes
// back through type().

#include "Settings.hpp"

#include "viewer/llsd/Value.hpp"

#include <RmlUi/Core/DataModelHandle.h>

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace viewer
{

class SettingsView
{
public:
    explicit SettingsView(Settings& settings);

    // Registers the tab's values and events in the menu's model (before its
    // document is loaded); then the model's handle.
    void bind(Rml::DataModelConstructor& model);
    void setHandle(Rml::DataModelHandle handle) { model_ = handle; }

    // What a listed setting's chosen from now (shown again if it changed).
    void setList(const std::string& name, const std::vector<SettingChoice>& choices);
    // The page shown.
    const std::string& pageName() const { return pageName_; }

    // Once a frame while the tab shows.
    void update(bool headset);

    // A setting (its name) to type on the VR keyboard, once.
    std::optional<std::string> takeKeyboard() { return std::exchange(keyboard_, std::nullopt); }
    // What was typed for it there.
    void type(const std::string& name, const std::string& text);

    struct Choice
    {
        Rml::String value;
        Rml::String label;
        bool operator==(const Choice&) const = default;
    };
    struct Row
    {
        Rml::String heading; // its section's, if it starts one
        Rml::String label;
        Rml::String description;
        Rml::String kind; // "check", "number", "text", "choice", "list"
        Rml::String text; // the field's (the page sets it)
        bool on = false;  // a check box's
        Rml::String chosen;
        std::vector<Choice> choices;
        Rml::String unit;
        Rml::String placeholder; // shown for an empty field
        Rml::String error;       // what's wrong with what's typed
        bool isDefault = true;
        bool restart = false;    // read when the viewer starts
        bool overridden = false; // set for this run (--setting), not saved
        bool operator==(const Row&) const = default;
    };

private:
    // What a row shows of its setting, from it.
    void fill(size_t i);
    void build();
    // What was typed into row i's field, if it changed, set or said wrong
    // (true: the row changed).
    bool typed(size_t i);
    // The row's index, from an event's arguments (-1: none).
    int rowAt(const Rml::VariantList& arguments, size_t at = 0) const;

    Settings& settings_;
    Rml::DataModelHandle model_;
    std::vector<Rml::String> pages_;
    int page_ = 0; // the page sets it
    int pageBuilt_ = -1;
    Rml::String pageName_;
    Rml::String file_;
    bool headset_ = false;
    std::vector<Row> rows_;
    // Each row's setting, the text it was last seen with and the value it
    // last showed.
    struct Shown
    {
        std::string name;
        Rml::String typed;
        llsd::Value value;
    };
    std::vector<Shown> shown_;
    std::map<std::string, std::vector<Choice>> lists_;
    uint64_t version_ = 0;
    std::optional<std::string> keyboard_;
};

} // namespace viewer
