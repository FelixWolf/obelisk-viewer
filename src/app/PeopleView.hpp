#pragma once

// The main menu's People tab, as the official viewer's People floater
// (llpanelpeople.cpp) has its Friends and Nearby lists: friends online
// first, then by name; nearby people nearest first. Picking someone shows
// them on the right with what can be done: a message (on the Chat tab),
// offering a teleport or asking for one, asking for friendship or ending it
// (asked twice), and for friends, what they may see and do of ours (see us
// online, find us on the map, edit our objects) and what they let us.

#include "AvatarNames.hpp"
#include "People.hpp"

#include "viewer/voice/VoiceClient.hpp"

#include <RmlUi/Core/DataModelHandle.h>

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace viewer
{

class PeopleView
{
public:
    // Registers the tab's values and events in the menu's model (before its
    // document is loaded); then the model's handle.
    void bind(Rml::DataModelConstructor& model);
    void setHandle(Rml::DataModelHandle handle) { model_ = handle; }

    // Names (null until known; asking queues them).
    using Names = std::function<const AvatarName*(const Uuid&)>;
    // Their voice (nothing: not in it).
    using Voice = std::function<std::optional<voice::VoiceClient::Status>(const Uuid&)>;
    // Once a frame while the tab shows: `here` is where we are on the grid
    // (nothing: not in the world).
    void update(const Friends& friends, const Nearby& nearby, const Names& names,
                std::optional<glm::dvec3> here, const Voice& voice);

    // What the page asked for, once.
    struct Action
    {
        enum class Kind : uint8_t
        {
            Message,         // open a conversation with them
            OfferTeleport,   // to where we are
            RequestTeleport, // to where they are
            OfferFriendship,
            EndFriendship,
            ResetSkeleton,   // as we see them
            ResetAnimations, // and their animations
            SetRights,       // what we let a friend do: `rights`
        };
        Kind kind = Kind::Message;
        Uuid who;
        std::string name; // theirs, as shown
        int32_t rights = 0;
    };
    std::optional<Action> takeAction() { return std::exchange(action_, std::nullopt); }

    struct Row
    {
        Rml::String id;
        Rml::String name;
        Rml::String detail; // username, distance
        bool online = false;
        bool isFriend = false;
        Rml::String voice; // "in", "speaking", "muted"; empty: not in voice
        bool operator==(const Row&) const = default;
    };
    struct Card
    {
        Rml::String name;
        Rml::String username;
        Rml::String status; // "Online", "Offline", "12 m away"
        bool isFriend = false;
        bool online = false; // a friend online, or anyone nearby
        bool nearby = false;
        // What we let them, and what they let us.
        bool givesOnline = false;
        bool givesMap = false;
        bool givesModify = false;
        Rml::String theyLetUs; // "They let you see when they're online, ..."
        bool operator==(const Card&) const = default;
    };

private:
    void act(Action::Kind kind, int32_t rights = 0);

    Rml::DataModelHandle model_;
    Rml::String list_ = "friends"; // or "nearby"
    std::vector<Row> rows_;
    Rml::String note_;
    Rml::String chosen_; // the id picked, empty: none
    Card card_;
    bool confirmEnd_ = false;
    std::optional<Action> action_;
    Rml::String shownChosen_;  // the one the confirmation was about
    Uuid chosenId_;            // the one picked, if they're still there
    int32_t chosenRights_ = 0; // what we let them
};

} // namespace viewer
