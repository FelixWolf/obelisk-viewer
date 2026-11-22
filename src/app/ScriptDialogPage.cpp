#include "ScriptDialogPage.hpp"

#include "InventoryBrowser.hpp"

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
    a.offer = offer_;
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
    model.Bind("subtitle", &subtitle_);
    model.Bind("details", &details_);
    model.Bind("acceptLabel", &acceptLabel_);
    model.Bind("declineLabel", &declineLabel_);
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
    on("accept", Action::Kind::Accept);
    on("decline", Action::Kind::Decline);
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
    if(serial != serial_ || offer_)
    {
        reply_.clear(); // a new one: no text typed into it yet
        model_.DirtyVariable("reply");
    }
    serial_ = serial;
    offer_ = false;
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

namespace
{

// "a notecard", by asset type.
std::string kindOf(int assetType)
{
    const std::string_view name = inventoryKind(assetType, -1).name;
    return name.empty() ? std::string("item") : std::string(name);
}

} // namespace

void ScriptDialogPage::fill(const Offer& o, size_t waiting)
{
    serial_ = o.serial;
    offer_ = true;
    kind_ = "offer";
    rows_.clear();
    permissions_.clear();
    details_.clear();
    more_ = waiting > 1 ? static_cast<int>(waiting - 1) : 0;
    objectName_ = o.fromName.empty() ? std::string("Someone") : o.fromName;
    owner_.clear();
    message_ = o.message;
    subtitle_.clear();
    acceptLabel_ = "OK";
    declineLabel_.clear();
    url_.clear();
    const std::string item =
        o.itemName.empty() ? kindOf(o.assetType) : o.itemName + " (" + kindOf(o.assetType) + ")";
    using Kind = Offer::Kind;
    switch(o.kind)
    {
    case Kind::Teleport:
        subtitle_ = "offers you a teleport";
        if(!o.rating.empty())
        {
            details_.push_back("The region is rated " + o.rating + ".");
        }
        acceptLabel_ = "Teleport";
        declineLabel_ = "Decline";
        break;
    case Kind::TeleportRequest:
        subtitle_ = "asks you to teleport them to you";
        acceptLabel_ = "Offer a teleport";
        declineLabel_ = "Decline";
        break;
    case Kind::Friendship:
        subtitle_ = "offers you friendship";
        acceptLabel_ = "Accept";
        declineLabel_ = "Decline";
        break;
    case Kind::GroupInvite:
        subtitle_ = "invites you to a group";
        details_.push_back(o.fee > 0 ? "Joining costs L$" + std::to_string(o.fee) + "."
                                     : std::string("Joining is free."));
        acceptLabel_ = o.fee > 0 ? "Join and pay L$" + std::to_string(o.fee) : "Join";
        declineLabel_ = "Decline";
        break;
    case Kind::Inventory:
        subtitle_ = "gave you an item";
        message_ = item;
        details_.push_back("It's in your inventory.");
        acceptLabel_ = "Keep";
        declineLabel_ = "Discard";
        break;
    case Kind::ObjectInventory:
    {
        const std::string owner = nameOf(o.from);
        subtitle_ = owner.empty() ? std::string("An object") : "Owned by " + owner;
        message_ = "It offers you " + item + ".";
        acceptLabel_ = "Keep";
        declineLabel_ = "Decline";
        break;
    }
    case Kind::GroupNotice:
        objectName_ = o.subject.empty() ? std::string("Group notice") : o.subject;
        subtitle_ = o.fromName.empty() ? std::string("A group notice")
                                       : "A group notice from " + o.fromName;
        if(o.assetType >= 0)
        {
            details_.push_back("Attached: " + item);
            acceptLabel_ = "Keep the attachment";
            declineLabel_ = "Close";
        }
        break;
    case Kind::Url:
        url_ = o.url;
        details_.push_back(o.url);
        if(headset_)
        {
            details_.push_back("It opens on your desktop.");
        }
        acceptLabel_ = "Open in browser";
        declineLabel_ = "Not now";
        break;
    case Kind::Message:
        if(o.fromName.empty() && !o.fromObject)
        {
            objectName_ = "Second Life";
        }
        break;
    }
    shownOwner_ = o.kind == Kind::ObjectInventory ? nameOf(o.from) : std::string();
    caution_ = false;
    experience_ = false;
    model_.DirtyAllVariables();
}

void ScriptDialogPage::update(const ScriptDialogs& dialogs, const Offers& offers, bool headset,
                              bool keyboard, bool pointHint)
{
    if(headset != headset_ || keyboard != keyboard_ || pointHint != pointHint_)
    {
        headset_ = headset;
        keyboard_ = keyboard;
        pointHint_ = pointHint;
        model_.DirtyVariable("headset");
        model_.DirtyVariable("keyboard");
        model_.DirtyVariable("pointHint");
        shownVersions_.reset(); // (an offered page's hint)
    }
    // Scripts' first, then offers.
    const ScriptNotice* n = dialogs.front();
    const Offer* o = n ? nullptr : offers.front();
    const size_t waiting = dialogs.waiting() + offers.waiting();
    // An owner's name may come after the notice.
    const bool ownerArrived =
        (n && n->kind == ScriptNotice::Kind::LoadUrl && names_ && !n->groupOwned &&
         names_(n->ownerId) != shownOwner_) ||
        (o && o->kind == Offer::Kind::ObjectInventory && nameOf(o->from) != shownOwner_);
    const std::pair versions{dialogs.version(), offers.version()};
    if(shownVersions_ != versions || ownerArrived)
    {
        if(o)
        {
            fill(*o, waiting);
        }
        else
        {
            fill(n, waiting);
        }
        shownVersions_ = versions;
    }
    surface_->update();
}

} // namespace viewer
