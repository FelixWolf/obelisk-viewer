#include "ChatView.hpp"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

#include <ctime>
#include <format>

namespace viewer
{

namespace
{

// "14:05", local time.
std::string clockTime(std::chrono::system_clock::time_point when)
{
    const std::time_t t = std::chrono::system_clock::to_time_t(when);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    return std::format("{:02}:{:02}", local.tm_hour, local.tm_min);
}

} // namespace

void ChatView::bind(Rml::DataModelConstructor& model)
{
    if(auto c = model.RegisterStruct<Conversation>())
    {
        c.RegisterMember("id", &Conversation::id);
        c.RegisterMember("name", &Conversation::name);
        c.RegisterMember("unread", &Conversation::unread);
    }
    model.RegisterArray<std::vector<Conversation>>();
    if(auto l = model.RegisterStruct<Line>())
    {
        l.RegisterMember("name", &Line::name);
        l.RegisterMember("text", &Line::text);
        l.RegisterMember("kind", &Line::kind);
        l.RegisterMember("when", &Line::when);
    }
    model.RegisterArray<std::vector<Line>>();
    model.Bind("chatConversations", &conversations_);
    model.Bind("chatOpen", &open_);
    model.Bind("chatLines", &lines_);
    model.Bind("chatShowObjects", &showObjects_);
    model.Bind("chatDraft", &draft_);
    model.Bind("chatHeadset", &headset_);
    auto send = [this]
    {
        if(parseChatInput(draft_))
        {
            say_ = draft_;
            toBottom_ = true; // what we say shows at the bottom
        }
        draft_.clear();
        model_.DirtyVariable("chatDraft");
    };
    model.BindEventCallback(
        "chatSend", [send](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { send(); });
    // Enter in the field sends too.
    model.BindEventCallback("chatKey",
                            [send](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList&)
                            {
                                const int key = event.GetParameter<int>("key_identifier", 0);
                                if(key == Rml::Input::KI_RETURN ||
                                   key == Rml::Input::KI_NUMPADENTER)
                                {
                                    send();
                                }
                            });
    model.BindEventCallback("chatType", [this](Rml::DataModelHandle, Rml::Event&,
                                               const Rml::VariantList&) { keyboard_ = true; });
}

void ChatView::fill(const LocalChat& chat)
{
    lines_.clear();
    for(const ChatLine& c : chat.log())
    {
        const bool toUs = c.chatType == ChatType::Owner;
        if(!showObjects_ && c.sourceType == ChatSource::Object && !toUs)
        {
            continue;
        }
        Line line;
        line.name = c.emote ? std::string() : c.fromName;
        line.text = c.text;
        line.kind = toUs                                 ? "owner"
                    : c.sourceType == ChatSource::Object ? "object"
                    : c.sourceType == ChatSource::Agent  ? "avatar"
                                                         : "system";
        line.when = clockTime(c.when);
        lines_.push_back(std::move(line));
    }
    model_.DirtyVariable("chatLines");
}

void ChatView::update(const LocalChat& chat, bool shown, bool headset,
                      Rml::ElementDocument* document)
{
    if(headset != headset_)
    {
        headset_ = headset;
        model_.DirtyVariable("chatHeadset");
    }
    // What wasn't seen: lines since the tab last showed them.
    if(shown && open_ == "nearby")
    {
        seen_ = chat.added();
    }
    const int unread = static_cast<int>(
        std::min<uint64_t>(chat.added() - std::min(seen_, chat.added()), chat.log().size()));
    if(conversations_[0].unread != unread)
    {
        conversations_[0].unread = unread;
        model_.DirtyVariable("chatConversations");
    }
    if(!shown)
    {
        return;
    }
    Rml::Element* scroll = document ? document->GetElementById("chat-lines") : nullptr;
    if(shown_ != chat.added() || shownObjects_ != showObjects_)
    {
        // Following the newest lines unless scrolled back to read.
        if(scroll &&
           scroll->GetScrollTop() + scroll->GetClientHeight() >= scroll->GetScrollHeight() - 8.0f)
        {
            toBottom_ = true;
        }
        if(shown_ == ~uint64_t{0} || shownObjects_ != showObjects_)
        {
            toBottom_ = true; // first shown, or filtered anew
        }
        shown_ = chat.added();
        shownObjects_ = showObjects_;
        fill(chat);
    }
}

bool ChatView::scrollAfterLayout(Rml::ElementDocument* document)
{
    Rml::Element* scroll = document ? document->GetElementById("chat-lines") : nullptr;
    if(!toBottom_ || !scroll)
    {
        return false;
    }
    toBottom_ = false;
    scroll->SetScrollTop(scroll->GetScrollHeight() - scroll->GetClientHeight());
    return true;
}

} // namespace viewer
