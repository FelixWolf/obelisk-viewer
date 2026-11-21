#pragma once

// The main menu's Chat tab: conversations on the left (Nearby, local chat,
// for now; instant messages and group chats later), the chosen one's
// lines on the right, newest at the bottom, and a line to type into. Chat
// never pops up anywhere else (objects can chat a lot): it waits here, and
// the list counts what wasn't seen. Nearby can leave out objects' chat
// (what's said to us alone, llOwnerSay, always shows, marked). In a
// headset, a button opens the VR keyboard instead of the field.

#include "LocalChat.hpp"

#include <RmlUi/Core/DataModelHandle.h>

#include <cstdint>
#include <optional>
#include <string>
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
    // Registers the tab's values and events in the menu's model (before its
    // document is loaded); then the model's handle.
    void bind(Rml::DataModelConstructor& model);
    void setHandle(Rml::DataModelHandle handle) { model_ = handle; }

    // Once a frame: the lines (shown only while `shown`: the tab is), and
    // how typing goes (`headset`: on the VR keyboard).
    void update(const LocalChat& chat, bool shown, bool headset, Rml::ElementDocument* document);
    // After the page's layout: keeps the newest line in view if it should;
    // true if it scrolled (lay out again).
    bool scrollAfterLayout(Rml::ElementDocument* document);

    // What was sent to Nearby (as typed: parseChatInput), once.
    std::optional<std::string> takeSay() { return std::exchange(say_, std::nullopt); }
    // The VR keyboard was asked for, to type into Nearby, once.
    bool takeKeyboard() { return std::exchange(keyboard_, false); }

    struct Conversation
    {
        Rml::String id;
        Rml::String name;
        int unread = 0;
    };
    struct Line
    {
        Rml::String name; // empty for emotes (the name is in the text)
        Rml::String text;
        Rml::String kind; // avatar, object, owner (said to us alone), system
        Rml::String when; // "14:05"
    };

private:
    void fill(const LocalChat& chat);

    Rml::DataModelHandle model_;
    std::vector<Conversation> conversations_{{"nearby", "Nearby", 0}};
    Rml::String open_ = "nearby";
    std::vector<Line> lines_;
    bool showObjects_ = true;
    bool shownObjects_ = true;
    Rml::String draft_;
    bool headset_ = false;
    std::optional<std::string> say_;
    bool keyboard_ = false;
    // The log's newest line shown, and the newest seen (while the tab is).
    uint64_t shown_ = ~uint64_t{0};
    uint64_t seen_ = 0;
    // Keep the newest line in view after the next layout (unless scrolled
    // back to read).
    bool toBottom_ = false;
};

} // namespace viewer
