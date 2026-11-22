#include "ChatView.hpp"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

#include <algorithm>
#include <ctime>
#include <format>

namespace viewer
{

namespace
{

std::tm localTime(std::chrono::system_clock::time_point when)
{
    const std::time_t t = std::chrono::system_clock::to_time_t(when);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    return local;
}

// "14:05" local time, or "Nov 18 14:05" if it's not today.
std::string clockTime(std::chrono::system_clock::time_point when)
{
    static constexpr const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const std::tm local = localTime(when);
    const std::tm today = localTime(std::chrono::system_clock::now());
    if(local.tm_year == today.tm_year && local.tm_yday == today.tm_yday)
    {
        return std::format("{:02}:{:02}", local.tm_hour, local.tm_min);
    }
    return std::format("{} {} {:02}:{:02}", kMonths[local.tm_mon % 12], local.tm_mday,
                       local.tm_hour, local.tm_min);
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
    model.Bind("chatTitle", &title_);
    model.Bind("chatTyping", &typing_);
    model.Bind("chatLines", &lines_);
    model.Bind("chatShowObjects", &showObjects_);
    model.Bind("chatDraft", &draft_);
    model.Bind("chatHeadset", &headset_);
    auto send = [this]
    {
        if(parseChatInput(draft_))
        {
            say_ = Said{open_, draft_};
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
                                               const Rml::VariantList&) { keyboard_ = open_; });
}

void ChatView::fillNearby(const LocalChat& chat, const Uuid& self)
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
                    : c.sourceType == ChatSource::Agent  ? (c.source == self ? "self" : "avatar")
                                                         : "system";
        line.when = clockTime(c.when);
        lines_.push_back(std::move(line));
    }
}

void ChatView::fillConversation(const ImConversation& c)
{
    lines_.clear();
    for(const ImMessage& m : c.lines)
    {
        Line line;
        const bool emote = m.text.starts_with("/me ") || m.text.starts_with("/me'");
        line.name = emote ? std::string() : m.fromName;
        line.text = emote ? m.fromName + m.text.substr(3) : m.text;
        if(m.saved)
        {
            line.text = "(saved) " + line.text;
        }
        line.kind = m.response ? "system" : m.ours ? "self" : "avatar";
        line.when = clockTime(m.when);
        lines_.push_back(std::move(line));
    }
}

void ChatView::update(const LocalChat& chat, const InstantMessages& ims, const Uuid& self,
                      bool shown, bool headset, Rml::ElementDocument* document)
{
    if(headset != headset_)
    {
        headset_ = headset;
        model_.DirtyVariable("chatHeadset");
    }
    // The one shown, if it went (logged out): Nearby.
    const std::string open = open_;
    Uuid openSession;
    const ImConversation* conversation = nullptr;
    if(open != kNearby)
    {
        if(const auto id = Uuid::parse(open))
        {
            openSession = *id;
            conversation = ims.find(openSession);
        }
        if(!conversation)
        {
            open_ = kNearby;
            model_.DirtyVariable("chatOpen");
        }
    }
    const std::string openId = open_;
    // What's shown is seen.
    if(shown)
    {
        seen_[openId] = conversation ? conversation->added : chat.added();
    }

    // The list: Nearby, then the others newest first, each with what wasn't
    // seen.
    auto unseen = [&](const std::string& id, uint64_t added, size_t kept)
    {
        const auto it = seen_.find(id);
        const uint64_t seen = it == seen_.end() ? 0 : std::min(it->second, added);
        return static_cast<size_t>(std::min<uint64_t>(added - seen, kept));
    };
    std::vector<Conversation> list;
    list.push_back(
        {kNearby, "Nearby", static_cast<int>(unseen(kNearby, chat.added(), chat.log().size()))});
    for(const ImConversation& c : ims.conversations())
    {
        // Ours don't count.
        const std::string id = c.session.toString();
        const size_t newer = unseen(id, c.added, c.lines.size());
        const int unread = static_cast<int>(
            std::count_if(c.lines.end() - static_cast<std::ptrdiff_t>(newer), c.lines.end(),
                          [](const ImMessage& m) { return !m.ours; }));
        list.push_back({id, c.name.empty() ? std::string("Someone") : c.name, unread});
    }
    if(list != conversations_)
    {
        conversations_ = std::move(list);
        model_.DirtyVariable("chatConversations");
    }
    if(!shown)
    {
        return;
    }

    // The chosen one's title, whether they're typing, and its lines.
    const std::string title =
        conversation ? (conversation->name.empty() ? std::string("Someone") : conversation->name)
                     : std::string("Nearby");
    if(title_ != title)
    {
        title_ = title;
        model_.DirtyVariable("chatTitle");
    }
    const std::string typing = conversation && conversation->typing
                                   ? conversation->name + " is typing\xE2\x80\xA6"
                                   : std::string();
    if(typing_ != typing)
    {
        typing_ = typing;
        model_.DirtyVariable("chatTyping");
    }
    const uint64_t added = conversation ? conversation->added : chat.added();
    const bool switched = shownOpen_ != openId;
    if(switched || shownAdded_ != added || shownObjects_ != showObjects_ ||
       (conversation && shownImVersion_ != ims.version()))
    {
        // Following the newest lines unless scrolled back to read.
        Rml::Element* scroll = document ? document->GetElementById("chat-lines") : nullptr;
        if(switched || shownObjects_ != showObjects_ ||
           (scroll &&
            scroll->GetScrollTop() + scroll->GetClientHeight() >= scroll->GetScrollHeight() - 8.0f))
        {
            toBottom_ = true;
        }
        shownOpen_ = openId;
        shownAdded_ = added;
        shownObjects_ = showObjects_;
        shownImVersion_ = ims.version();
        if(conversation)
        {
            fillConversation(*conversation);
        }
        else
        {
            fillNearby(chat, self);
        }
        model_.DirtyVariable("chatLines");
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
