#include "LocalChat.hpp"

#include "viewer/protocol/Messages.hpp"

namespace viewer
{

namespace
{

// A bubble's lines: at most this many, and this many characters
// (MAX_BUBBLE_CHAT_UTTERANCES, DB_CHAT_MSG_STR_LEN).
constexpr size_t kBubbleLines = 12;
constexpr size_t kBubbleCharacters = 1023;
constexpr size_t kLogLines = 500;
constexpr uint8_t kFullyAudible = 1;

std::string trimmed(std::string text)
{
    const size_t begin = text.find_first_not_of(" \t\r\n");
    if(begin == std::string::npos)
    {
        return {};
    }
    const size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// Tabs (from scripts) become four spaces.
std::string expandTabs(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for(const char c : text)
    {
        if(c == '\t')
        {
            out += "    ";
        }
        else
        {
            out += c;
        }
    }
    return out;
}

} // namespace

void LocalChat::handle(const protocol::CircuitEvent& event, AvatarNames& names,
                       Clock::time_point now)
{
    const auto m = event.decode<protocol::msg::ChatFromSimulator>();
    if(!m)
    {
        return;
    }
    const auto& c = m->ChatData;
    handle(c.SourceType, c.ChatType, c.Audible, c.SourceID, c.OwnerID,
           protocol::toString(c.FromName), protocol::toString(c.Message), &names, now);
}

void LocalChat::handle(uint8_t sourceType, uint8_t chatType, uint8_t audible, const Uuid& source,
                       const Uuid& owner, std::string fromName, std::string message,
                       AvatarNames* names, Clock::time_point now)
{
    if(audible != kFullyAudible)
    {
        return;
    }
    const bool avatar = sourceType == ChatSource::Agent;
    if(avatar && (chatType == ChatType::StartTyping || chatType == ChatType::StopTyping))
    {
        Bubble& b = bubbles_[source];
        b.typing = chatType == ChatType::StartTyping;
        b.typingSince = now;
        return;
    }
    if(chatType == ChatType::StartTyping || chatType == ChatType::StopTyping)
    {
        return;
    }

    ChatLine line;
    line.time = now;
    line.source = source;
    line.owner = owner;
    line.sourceType = sourceType;
    line.chatType = chatType;
    const AvatarName* name = avatar && names ? names->get(source) : nullptr;
    line.fromName = name ? name->completeName() : trimmed(std::move(fromName));
    if(line.fromName.empty())
    {
        line.fromName = "(Unnamed)";
    }
    message = expandTabs(message);
    line.emote = message.starts_with("/me ") || message.starts_with("/me'");
    if(line.emote)
    {
        line.text = line.fromName + message.substr(3);
    }
    else
    {
        line.text = chatType == ChatType::Whisper ? "whispers: "
                    : chatType == ChatType::Shout ? "shouts: "
                                                  : "";
        line.text += message;
    }

    if(avatar)
    {
        // A real line: no more "..." (and into the bubble).
        Bubble& b = bubbles_[source];
        b.typing = false;
        b.lines.push_back(line);
        b.latest = now;
        size_t characters = 0;
        for(const ChatLine& l : b.lines)
        {
            characters += l.text.size();
        }
        while(!b.lines.empty() && (characters > kBubbleCharacters || b.lines.size() > kBubbleLines))
        {
            characters -= b.lines.front().text.size();
            b.lines.pop_front();
        }
    }
    log_.push_back(std::move(line));
    while(log_.size() > kLogLines)
    {
        log_.pop_front();
    }
}

const LocalChat::Bubble* LocalChat::bubble(const Uuid& avatar, Clock::time_point now)
{
    const auto it = bubbles_.find(avatar);
    if(it == bubbles_.end())
    {
        return nullptr;
    }
    Bubble& b = it->second;
    if(!b.lines.empty() && now - b.latest > kBubbleTime)
    {
        b.lines.clear();
    }
    if(b.lines.empty() && !b.typing)
    {
        bubbles_.erase(it);
        return nullptr;
    }
    return &b;
}

void LocalChat::clear()
{
    bubbles_.clear();
    log_.clear();
}

} // namespace viewer
