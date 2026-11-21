#include "ScriptDialogPage.hpp"

#include "viewer/core/Log.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>

#include <utility>

namespace viewer
{

std::unique_ptr<ScriptDialogPage> ScriptDialogPage::create(gui::Gui& gui)
{
    std::unique_ptr<ScriptDialogPage> page(new ScriptDialogPage(gui));
    if(!page->setUp())
    {
        return nullptr;
    }
    return page;
}

void ScriptDialogPage::act(Action::Kind kind, size_t button)
{
    if(kind_.empty())
    {
        return; // nothing shown
    }
    Action a;
    a.kind = kind;
    a.serial = serial_;
    a.button = button;
    if(kind == Action::Kind::Send)
    {
        a.text = reply_;
    }
    action_ = std::move(a);
}

bool ScriptDialogPage::setUp()
{
    surface_ = gui_.createSurface("script_dialog", kSize);
    if(!surface_)
    {
        return false;
    }
    Rml::DataModelConstructor model = surface_->context()->CreateDataModel("dialog");
    if(!model)
    {
        return false;
    }
    if(auto button = model.RegisterStruct<Button>())
    {
        button.RegisterMember("label", &Button::label);
        button.RegisterMember("index", &Button::index);
    }
    model.RegisterArray<std::vector<Button>>();
    if(auto row = model.RegisterStruct<Row>())
    {
        row.RegisterMember("buttons", &Row::buttons);
    }
    model.RegisterArray<std::vector<Row>>();
    model.RegisterArray<std::vector<Rml::String>>();
    model.Bind("kind", &kind_);
    model.Bind("objectName", &objectName_);
    model.Bind("owner", &owner_);
    model.Bind("message", &message_);
    model.Bind("rows", &rows_);
    model.Bind("permissions", &permissions_);
    model.Bind("caution", &caution_);
    model.Bind("experience", &experience_);
    model.Bind("url", &url_);
    model.Bind("more", &more_);
    model.Bind("reply", &reply_);
    model.Bind("headset", &headset_);
    model.Bind("keyboard", &keyboard_);
    model.Bind("pointHint", &pointHint_);
    auto on = [&](const char* name, Action::Kind kind)
    {
        model.BindEventCallback(name, [this, kind](Rml::DataModelHandle, Rml::Event&,
                                                   const Rml::VariantList&) { act(kind); });
    };
    model.BindEventCallback("choose",
                            [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
                            {
                                const int i = a.empty() ? -1 : a[0].Get<int>(-1);
                                if(i >= 0)
                                {
                                    act(Action::Kind::Choose, static_cast<size_t>(i));
                                }
                            });
    on("send", Action::Kind::Send);
    on("allow", Action::Kind::Allow);
    on("deny", Action::Kind::Deny);
    on("open", Action::Kind::Open);
    on("ignore", Action::Kind::Ignore);
    on("type", Action::Kind::Keyboard);
    model_ = model.GetModelHandle();
    if(!surface_->load("script_dialog.rml"))
    {
        log::error("the script dialog page couldn't be read");
        return false;
    }
    return true;
}

std::optional<ScriptDialogPage::Action> ScriptDialogPage::takeAction()
{
    return std::exchange(action_, std::nullopt);
}

void ScriptDialogPage::fill(const ScriptNotice* n, size_t waiting)
{
    const uint64_t serial = n ? n->serial : 0;
    if(serial != serial_)
    {
        reply_.clear(); // a new one: no text typed into it yet
        model_.DirtyVariable("reply");
    }
    serial_ = serial;
    kind_.clear();
    rows_.clear();
    permissions_.clear();
    more_ = waiting > 1 ? static_cast<int>(waiting - 1) : 0;
    if(n)
    {
        using Kind = ScriptNotice::Kind;
        kind_ = n->kind == Kind::Dialog        ? "dialog"
                : n->kind == Kind::TextBox     ? "textbox"
                : n->kind == Kind::Permissions ? "permissions"
                                               : "url";
        objectName_ = n->objectName.empty() ? "An object" : n->objectName;
        owner_ = n->owner;
        if(n->kind == Kind::LoadUrl && names_ && !n->groupOwned)
        {
            owner_ = names_(n->ownerId);
        }
        if(n->groupOwned && owner_.empty())
        {
            owner_ = "a group";
        }
        shownOwner_ = owner_;
        message_ = n->message;
        for(const auto& row : dialogRows(n->kind == Kind::Dialog ? n->buttons.size() : 0))
        {
            Row& r = rows_.emplace_back();
            for(const size_t i : row)
            {
                r.buttons.push_back({n->buttons[i], static_cast<int>(i)});
            }
        }
        for(const std::string& p : n->permissions)
        {
            permissions_.push_back(p);
        }
        caution_ = n->caution;
        experience_ = !n->experience.isNull();
        url_ = n->url;
    }
    model_.DirtyAllVariables();
}

void ScriptDialogPage::update(const ScriptDialogs& dialogs, bool headset, bool keyboard,
                              bool pointHint)
{
    const ScriptNotice* n = dialogs.front();
    // A web page's owner's name may come after it.
    const bool ownerArrived = n && n->kind == ScriptNotice::Kind::LoadUrl && names_ &&
                              !n->groupOwned && names_(n->ownerId) != shownOwner_;
    if(shownVersion_ != dialogs.version() || ownerArrived)
    {
        fill(n, dialogs.waiting());
        shownVersion_ = dialogs.version();
    }
    if(headset != headset_ || keyboard != keyboard_ || pointHint != pointHint_)
    {
        headset_ = headset;
        keyboard_ = keyboard;
        pointHint_ = pointHint;
        model_.DirtyVariable("headset");
        model_.DirtyVariable("keyboard");
        model_.DirtyVariable("pointHint");
    }
    surface_->update();
}

} // namespace viewer
