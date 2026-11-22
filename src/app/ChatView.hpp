#pragma once

// The main menu's Chat tab: conversations on the left (Nearby, local chat,
// then instant messages and group chats, newest first), the chosen one's
// lines on the right, newest at the bottom, and a line to type into. Chat
// never pops up anywhere else (objects can chat a lot): it waits here, and
// the list counts what wasn't seen. Nearby can leave out objects' chat
// (what's said to us alone, llOwnerSay, always shows, marked). In a
// headset, a button opens the VR keyboard instead of the field.

#include "InstantMessages.hpp"
#include "LocalChat.hpp"

#include <RmlUi/Core/DataModelHandle.h>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Rml
{
class ElementDocument;
}

namespace viewer
{

class ChatView
{
public:
    // The Nearby conversation's id (others' are their sessions' ids).
    static constexpr const char* kNearby = "nearby";

    // Registers the tab's values and events in the menu's model (before its
    // document is loaded); then the model's handle.
    void bind(Rml::DataModelConstructor& model);
    void setHandle(Rml::DataModelHandle handle) { model_ = handle; }
    // Shows this conversation (kNearby, or a session's id).
    void open(const std::string& id)
    {
        open_ = id;
        model_.DirtyVariable("chatOpen");
    }

    // Once a frame: the conversations (their lines shown only while
    // `shown`: the tab is), and how typing goes (`headset`: on the VR
    // keyboard). `self`: our agent id (our own lines look ours).
    void update(const LocalChat& chat, const InstantMessages& ims, const Uuid& self, bool shown,
                bool headset, Rml::ElementDocument* document);
    // After the page's layout: keeps the newest line in view if it should;
    // true if it scrolled (lay out again).
    bool scrollAfterLayout(Rml::ElementDocument* document);

    // What was sent, and to which conversation (kNearby, or a session's
    // id), once.
    struct Said
    {
        std::string conversation;
        std::string text;
    };
    std::optional<Said> takeSay() { return std::exchange(say_, std::nullopt); }
    // The VR keyboard was asked for, to type into this conversation, once.
    std::optional<std::string> takeKeyboard() { return std::exchange(keyboard_, std::nullopt); }

    struct Conversation
    {
        Rml::String id;
        Rml::String name;
        int unread = 0;
        bool operator==(const Conversation&) const = default;
    };
    struct Line
    {
        Rml::String name; // empty for emotes (the name is in the text)
        Rml::String text;
        // avatar, self (ours), object, owner (said to us alone), system
        // (automatic answers and the like)
        Rml::String kind;
        Rml::String when; // "14:05" (with the date if it's not today)
    };

private:
    void fillNearby(const LocalChat& chat, const Uuid& self);
    void fillConversation(const ImConversation& c);

    Rml::DataModelHandle model_;
    std::vector<Conversation> conversations_{{kNearby, "Nearby", 0}};
    Rml::String open_ = kNearby;
    Rml::String title_ = "Nearby";
    Rml::String typing_; // "Ann is typing…"
    std::vector<Line> lines_;
    bool showObjects_ = true;
    Rml::String draft_;
    bool headset_ = false;
    std::optional<Said> say_;
    std::optional<std::string> keyboard_;
    // What's shown: which conversation, its lines' count, the filter.
    std::string shownOpen_;
    uint64_t shownAdded_ = ~uint64_t{0};
    bool shownObjects_ = true;
    uint64_t shownImVersion_ = ~uint64_t{0};
    // Lines seen in each conversation (by id), while the tab showed it.
    std::unordered_map<std::string, uint64_t> seen_;
    // Keep the newest line in view after the next layout (unless scrolled
    // back to read).
    bool toBottom_ = false;
};

} // namespace viewer
