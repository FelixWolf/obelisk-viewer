#include "SettingsView.hpp"

#include <RmlUi/Core.h>

#include <algorithm>
#include <cmath>

namespace viewer
{

SettingsView::SettingsView(Settings& settings) : settings_(settings)
{
    for(const SettingDefinition& d : settings_.definitions())
    {
        if(!d.hidden && std::ranges::find(pages_, d.page) == pages_.end())
        {
            pages_.push_back(d.page);
        }
    }
    file_ = settings_.file().string();
}

void SettingsView::bind(Rml::DataModelConstructor& model)
{
    if(auto choice = model.RegisterStruct<Choice>())
    {
        choice.RegisterMember("value", &Choice::value);
        choice.RegisterMember("label", &Choice::label);
    }
    model.RegisterArray<std::vector<Choice>>();
    if(auto row = model.RegisterStruct<Row>())
    {
        row.RegisterMember("heading", &Row::heading);
        row.RegisterMember("label", &Row::label);
        row.RegisterMember("description", &Row::description);
        row.RegisterMember("kind", &Row::kind);
        row.RegisterMember("text", &Row::text);
        row.RegisterMember("on", &Row::on);
        row.RegisterMember("chosen", &Row::chosen);
        row.RegisterMember("choices", &Row::choices);
        row.RegisterMember("unit", &Row::unit);
        row.RegisterMember("placeholder", &Row::placeholder);
        row.RegisterMember("error", &Row::error);
        row.RegisterMember("isDefault", &Row::isDefault);
        row.RegisterMember("restart", &Row::restart);
        row.RegisterMember("overridden", &Row::overridden);
    }
    model.RegisterArray<std::vector<Row>>();
    model.Bind("settingPages", &pages_);
    model.Bind("settingPage", &page_);
    model.Bind("settingPageName", &pageName_);
    model.Bind("settingRows", &rows_);
    model.Bind("settingsFile", &file_);
    model.Bind("settingsHeadset", &headset_);

    const auto event = [&model, this](const char* name, auto handle)
    {
        model.BindEventCallback(
            name,
            [this, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& arguments)
            {
                const int i = rowAt(arguments);
                if(i >= 0)
                {
                    handle(static_cast<size_t>(i), arguments);
                }
            });
    };
    // A check box clicked.
    event("settingToggle",
          [this](size_t i, const Rml::VariantList&)
          {
              const std::string& name = shown_[i].name;
              settings_.setValue(name, llsd::Value(!settings_.value(name).asBoolean()));
          });
    // A choice's key (settingChoose(row, choice)).
    event("settingChoose",
          [this](size_t i, const Rml::VariantList& arguments)
          {
              const int c = arguments.size() > 1 ? arguments[1].Get<int>(-1) : -1;
              const SettingDefinition* d = settings_.definition(shown_[i].name);
              if(d && c >= 0 && c < static_cast<int>(d->choices.size()))
              {
                  settings_.setValue(d->name, llsd::Value(d->choices[size_t(c)].value));
              }
          });
    // - or + (settingStep(row, -1 or 1)): a step from its value, to its range.
    event("settingStep",
          [this](size_t i, const Rml::VariantList& arguments)
          {
              const int direction = arguments.size() > 1 ? arguments[1].Get<int>(0) : 0;
              const SettingDefinition* d = settings_.definition(shown_[i].name);
              if(!d || direction == 0)
              {
                  return;
              }
              // From a step's multiple (so 47 goes to 50 or 45).
              const double v = settings_.value(d->name).asReal();
              const double steps = v / d->step;
              const double from =
                  direction > 0 ? std::floor(steps + 1e-9) : std::ceil(steps - 1e-9);
              settings_.setValue(d->name, llsd::Value((from + direction) * d->step));
              fill(i); // shown again, whatever was typed
              model_.DirtyVariable("settingRows");
          });
    event("settingReset",
          [this](size_t i, const Rml::VariantList&) { settings_.reset(shown_[i].name); });
    // Left the field: what's kept, as it's written.
    event("settingDone",
          [this](size_t i, const Rml::VariantList&)
          {
              typed(i);
              fill(i);
              model_.DirtyVariable("settingRows");
          });
    event("settingKeyboard",
          [this](size_t i, const Rml::VariantList&) { keyboard_ = shown_[i].name; });
    // Enter in a field: done with it.
    model.BindEventCallback(
        "settingKey",
        [this](Rml::DataModelHandle, Rml::Event& e, const Rml::VariantList& arguments)
        {
            const int key = e.GetParameter<int>("key_identifier", 0);
            if(key == Rml::Input::KI_RETURN || key == Rml::Input::KI_NUMPADENTER)
            {
                if(Rml::Element* field = e.GetTargetElement())
                {
                    field->Blur(); // settingDone
                }
                if(const int i = rowAt(arguments); i >= 0)
                {
                    typed(static_cast<size_t>(i));
                    fill(static_cast<size_t>(i));
                    model_.DirtyVariable("settingRows");
                }
            }
        });
}

int SettingsView::rowAt(const Rml::VariantList& arguments, size_t at) const
{
    const int i = arguments.size() > at ? arguments[at].Get<int>(-1) : -1;
    return i >= 0 && i < static_cast<int>(shown_.size()) ? i : -1;
}

void SettingsView::fill(size_t i)
{
    Row& row = rows_[i];
    Shown& shown = shown_[i];
    const SettingDefinition& d = *settings_.definition(shown.name);
    row.text = settings_.text(d.name);
    row.on = settings_.value(d.name).asBoolean();
    row.chosen = d.type == SettingType::Choice ? row.text : Rml::String();
    row.error.clear();
    row.isDefault = settings_.isDefault(d.name);
    row.overridden = settings_.isOverridden(d.name);
    shown.typed = row.text;
    shown.value = settings_.value(d.name);
}

void SettingsView::build()
{
    page_ = std::clamp(page_, 0, std::max(0, static_cast<int>(pages_.size()) - 1));
    pageBuilt_ = page_;
    pageName_ = pages_.empty() ? Rml::String() : pages_[static_cast<size_t>(page_)];
    rows_.clear();
    shown_.clear();
    std::string section;
    for(const SettingDefinition& d : settings_.definitions())
    {
        if(d.page != pageName_ || d.hidden)
        {
            continue;
        }
        Row row;
        if(!d.section.empty() && d.section != section)
        {
            section = d.section;
            row.heading = d.section;
        }
        row.label = d.label;
        row.description = d.description;
        row.kind = d.type == SettingType::Boolean   ? "check"
                   : d.type == SettingType::Integer ? "number"
                   : d.type == SettingType::Real    ? "number"
                   : d.type == SettingType::Choice  ? "choice"
                                                    : "text";
        for(const SettingChoice& c : d.choices)
        {
            row.choices.push_back({c.value, c.label});
        }
        row.unit = d.unit;
        row.placeholder = d.type == SettingType::Uuid ? "None" : "";
        row.restart = d.restart;
        rows_.push_back(std::move(row));
        shown_.push_back({d.name, {}, {}});
        fill(rows_.size() - 1);
    }
    version_ = settings_.version();
    model_.DirtyVariable("settingPageName");
    model_.DirtyVariable("settingRows");
}

bool SettingsView::typed(size_t i)
{
    // Typed into a field (the page set its text): set if it's a value it
    // takes, said what's wrong if not.
    Row& row = rows_[i];
    Shown& shown = shown_[i];
    if(row.text == shown.typed)
    {
        return false;
    }
    shown.typed = row.text;
    if(row.kind != "number" && row.kind != "text")
    {
        return false;
    }
    const Rml::String error = settings_.setText(shown.name, row.text);
    // The value as typed ("007" stays until the field's left).
    shown.value = settings_.value(shown.name);
    if(row.error == error)
    {
        return false;
    }
    row.error = error;
    return true;
}

void SettingsView::type(const std::string& name, const std::string& text)
{
    const std::string wrong = settings_.setText(name, text);
    for(size_t i = 0; i < shown_.size(); i++)
    {
        if(shown_[i].name == name)
        {
            fill(i);
            rows_[i].error = wrong;
            model_.DirtyVariable("settingRows");
        }
    }
}

void SettingsView::update(bool headset)
{
    if(headset_ != headset)
    {
        headset_ = headset;
        model_.DirtyVariable("settingsHeadset");
    }
    if(page_ != pageBuilt_)
    {
        // The old page's rows gone first (a frame), then the new page's:
        // the page would otherwise look up an old row's choices in the new
        // rows before taking them away.
        if(!rows_.empty())
        {
            rows_.clear();
            shown_.clear();
            model_.DirtyVariable("settingRows");
            return;
        }
        build();
        return;
    }
    bool dirty = false;
    for(size_t i = 0; i < rows_.size(); i++)
    {
        dirty |= typed(i);
    }
    // Changed some other way (a key here, the Media tab, the VR keyboard):
    // shown again.
    if(settings_.version() != version_)
    {
        version_ = settings_.version();
        for(size_t i = 0; i < rows_.size(); i++)
        {
            if(settings_.value(shown_[i].name) != shown_[i].value)
            {
                fill(i);
            }
            rows_[i].isDefault = settings_.isDefault(shown_[i].name);
            rows_[i].overridden = settings_.isOverridden(shown_[i].name);
        }
        dirty = true;
    }
    if(dirty)
    {
        model_.DirtyVariable("settingRows");
    }
}

} // namespace viewer
