#include "PeopleView.hpp"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>

namespace viewer
{

namespace
{

std::string lowered(std::string s)
{
    std::ranges::transform(s, s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// "8 m", "1.2 km".
std::string distanceText(double metres)
{
    if(metres < 1000.0)
    {
        return std::format("{:.0f} m", metres);
    }
    return std::format("{:.1f} km", metres / 1000.0);
}

// "They let you see when they're online and find them on the map."
std::string letsUs(int32_t rights)
{
    std::vector<std::string> what;
    if(rights & FriendRights::Online)
    {
        what.emplace_back("see when they're online");
    }
    if(rights & FriendRights::MapLocation)
    {
        what.emplace_back("find them on the map");
    }
    if(rights & FriendRights::ModifyObjects)
    {
        what.emplace_back("edit their objects");
    }
    if(what.empty())
    {
        return "They don't let you see when they're online.";
    }
    std::string text = "They let you " + what[0];
    for(size_t i = 1; i < what.size(); i++)
    {
        text += (i + 1 == what.size() ? " and " : ", ") + what[i];
    }
    return text + ".";
}

} // namespace

void PeopleView::bind(Rml::DataModelConstructor& model)
{
    if(auto r = model.RegisterStruct<Row>())
    {
        r.RegisterMember("id", &Row::id);
        r.RegisterMember("name", &Row::name);
        r.RegisterMember("detail", &Row::detail);
        r.RegisterMember("online", &Row::online);
        r.RegisterMember("isFriend", &Row::isFriend);
    }
    model.RegisterArray<std::vector<Row>>();
    if(auto c = model.RegisterStruct<Card>())
    {
        c.RegisterMember("name", &Card::name);
        c.RegisterMember("username", &Card::username);
        c.RegisterMember("status", &Card::status);
        c.RegisterMember("isFriend", &Card::isFriend);
        c.RegisterMember("online", &Card::online);
        c.RegisterMember("givesOnline", &Card::givesOnline);
        c.RegisterMember("givesMap", &Card::givesMap);
        c.RegisterMember("givesModify", &Card::givesModify);
        c.RegisterMember("theyLetUs", &Card::theyLetUs);
    }
    model.Bind("peopleList", &list_);
    model.Bind("peopleRows", &rows_);
    model.Bind("peopleNote", &note_);
    model.Bind("peopleChosen", &chosen_);
    model.Bind("peopleCard", &card_);
    model.Bind("peopleConfirmEnd", &confirmEnd_);
    const auto bindAction = [&](const char* name, Action::Kind kind)
    {
        model.BindEventCallback(name, [this, kind](Rml::DataModelHandle, Rml::Event&,
                                                   const Rml::VariantList&) { act(kind); });
    };
    bindAction("peopleMessage", Action::Kind::Message);
    bindAction("peopleOfferTeleport", Action::Kind::OfferTeleport);
    bindAction("peopleRequestTeleport", Action::Kind::RequestTeleport);
    bindAction("peopleAddFriend", Action::Kind::OfferFriendship);
    // Ending a friendship is asked twice.
    model.BindEventCallback("peopleEnd",
                            [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                if(!confirmEnd_)
                                {
                                    confirmEnd_ = true;
                                    model_.DirtyVariable("peopleConfirmEnd");
                                    return;
                                }
                                act(Action::Kind::EndFriendship);
                            });
    model.BindEventCallback("peopleKeep",
                            [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                confirmEnd_ = false;
                                model_.DirtyVariable("peopleConfirmEnd");
                            });
    // A right of ours to them toggled: peopleRight(1), (2) or (4).
    model.BindEventCallback(
        "peopleRight",
        [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& arguments)
        {
            const int32_t bit = arguments.empty() ? 0 : arguments.front().Get<int>(0);
            if(bit == FriendRights::Online || bit == FriendRights::MapLocation ||
               bit == FriendRights::ModifyObjects)
            {
                act(Action::Kind::SetRights, chosenRights_ ^ bit);
            }
        });
}

void PeopleView::act(Action::Kind kind, int32_t rights)
{
    if(chosenId_.isNull())
    {
        return;
    }
    action_ = Action{kind, chosenId_, card_.name, rights};
    if(confirmEnd_)
    {
        confirmEnd_ = false;
        model_.DirtyVariable("peopleConfirmEnd");
    }
}

void PeopleView::update(const Friends& friends, const Nearby& nearby, const Names& names,
                        std::optional<glm::dvec3> here)
{
    const auto nameOf = [&](const Uuid& id) -> std::pair<std::string, std::string>
    {
        const AvatarName* n = names(id);
        if(!n)
        {
            return {"Loading…", std::string()};
        }
        return {n->displayName, n->username};
    };

    // The rows, and who's in them (ids by row).
    std::vector<Row> rows;
    std::string note;
    struct Sorted
    {
        Row row;
        std::string key;
        double distance = 0.0;
    };
    std::vector<Sorted> sorted;
    if(list_ == "nearby")
    {
        for(const Nearby::Person& p : nearby.all())
        {
            const auto [name, username] = nameOf(p.id);
            Sorted s;
            s.row.id = p.id.toString();
            s.row.name = name;
            s.row.online = true;
            s.row.isFriend = friends.find(p.id) != nullptr;
            if(here)
            {
                glm::dvec3 where = globalPosition(p.regionHandle, p.position);
                if(!p.heightKnown)
                {
                    where.z = here->z; // only across, then
                }
                s.distance = glm::length(where - *here);
                s.row.detail = distanceText(s.distance);
            }
            s.key = lowered(name);
            sorted.push_back(std::move(s));
        }
        std::ranges::sort(sorted, [](const Sorted& a, const Sorted& b)
                          { return std::tie(a.distance, a.key) < std::tie(b.distance, b.key); });
        note = sorted.empty() ? (here ? "Nobody else is around." : "Not in the world.") : "";
    }
    else
    {
        for(const Friend& f : friends.all())
        {
            const auto [name, username] = nameOf(f.id);
            Sorted s;
            s.row.id = f.id.toString();
            s.row.name = name;
            s.row.detail = f.online ? "Online" : "";
            s.row.online = f.online;
            s.row.isFriend = true;
            s.key = lowered(name);
            sorted.push_back(std::move(s));
        }
        std::ranges::sort(
            sorted, [](const Sorted& a, const Sorted& b)
            { return std::tie(b.row.online, a.key) < std::tie(a.row.online, b.key); });
        note = sorted.empty() ? "No friends yet. Pick someone nearby to ask." : "";
    }
    rows.reserve(sorted.size());
    for(Sorted& s : sorted)
    {
        rows.push_back(std::move(s.row));
    }
    if(rows != rows_)
    {
        rows_ = std::move(rows);
        model_.DirtyVariable("peopleRows");
    }
    if(note != note_)
    {
        note_ = note;
        model_.DirtyVariable("peopleNote");
    }

    // The one picked: a friend, or someone nearby.
    const auto id = Uuid::parse(chosen_);
    const Friend* f = id ? friends.find(*id) : nullptr;
    std::optional<Nearby::Person> near;
    if(id)
    {
        for(const Nearby::Person& p : nearby.all())
        {
            if(p.id == *id)
            {
                near = p;
                break;
            }
        }
    }
    if(id && !f && !near && list_ != "nearby")
    {
        // Gone from both (a friendship ended): nobody picked.
        chosen_.clear();
        model_.DirtyVariable("peopleChosen");
    }
    Card card;
    chosenId_ = Uuid();
    chosenRights_ = 0;
    if(id && (f || near))
    {
        chosenId_ = *id;
        const auto [name, username] = nameOf(*id);
        card.name = name;
        card.username = username;
        card.isFriend = f != nullptr;
        card.online = near.has_value() || (f && f->online);
        if(near && here)
        {
            glm::dvec3 where = globalPosition(near->regionHandle, near->position);
            if(!near->heightKnown)
            {
                where.z = here->z;
            }
            card.status = distanceText(glm::length(where - *here)) + " away";
        }
        else
        {
            card.status = card.online ? "Online" : "Offline";
        }
        if(f)
        {
            chosenRights_ = f->rightsGiven;
            card.givesOnline = (f->rightsGiven & FriendRights::Online) != 0;
            card.givesMap = (f->rightsGiven & FriendRights::MapLocation) != 0;
            card.givesModify = (f->rightsGiven & FriendRights::ModifyObjects) != 0;
            card.theyLetUs = letsUs(f->rightsHas);
        }
    }
    if(card != card_)
    {
        card_ = std::move(card);
        model_.DirtyVariable("peopleCard");
    }
    if(shownChosen_ != chosen_)
    {
        shownChosen_ = chosen_;
        if(confirmEnd_)
        {
            confirmEnd_ = false; // asked about someone else
            model_.DirtyVariable("peopleConfirmEnd");
        }
    }
}

} // namespace viewer
