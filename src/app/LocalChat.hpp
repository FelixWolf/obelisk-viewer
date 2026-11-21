#pragma once

// Local chat heard (ChatFromSimulator), as the official viewer takes it
// (process_chat_from_simulator): a log for a chat window, and for each
// avatar what its chat bubble shows (LLVOAvatar::addChat, mChats, mTyping):
// its latest lines, at most 12 and 1023 characters, all gone 24 s after the
// latest, and whether it's typing. Only fully audible chat counts.

#include "AvatarNames.hpp"

#include "viewer/core/Uuid.hpp"
#include "viewer/protocol/CircuitManager.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace viewer
{

// ChatFromSimulator's ChatType and SourceType.
namespace ChatType
{
inline constexpr uint8_t Whisper = 0;
inline constexpr uint8_t Normal = 1;
inline constexpr uint8_t Shout = 2;
inline constexpr uint8_t StartTyping = 4;
inline constexpr uint8_t StopTyping = 5;
inline constexpr uint8_t Debug = 6;
inline constexpr uint8_t Owner = 8;
} // namespace ChatType
namespace ChatSource
{
inline constexpr uint8_t System = 0;
inline constexpr uint8_t Agent = 1;
inline constexpr uint8_t Object = 2;
} // namespace ChatSource

struct ChatLine
{
    std::chrono::steady_clock::time_point time{};
    std::chrono::system_clock::time_point when{}; // the wall clock's, to show
    Uuid source{};
    Uuid owner{};
    uint8_t sourceType = ChatSource::System;
    uint8_t chatType = ChatType::Normal;
    std::string fromName; // for avatars, their complete name if known
    // As shown: "/me waves" as "Name waves"; "whispers: " or "shouts: " before
    // whispered or shouted lines.
    std::string text;
    bool emote = false; // "/me": the name is in the text
};

class LocalChat
{
public:
    using Clock = std::chrono::steady_clock;
    // How long a bubble line stays new, then normal, then old (3 of these:
    // CHAT_FADE_TIME), and how long after the latest line they all go
    // (BUBBLE_CHAT_TIME).
    static constexpr auto kFadeTime = std::chrono::seconds(8);
    static constexpr auto kBubbleTime = kFadeTime * 3;

    // A ChatFromSimulator message.
    void handle(const protocol::CircuitEvent& event, AvatarNames& names, Clock::time_point now);
    // The same, decoded (for tests and the above).
    void handle(uint8_t sourceType, uint8_t chatType, uint8_t audible, const Uuid& source,
                const Uuid& owner, std::string fromName, std::string message, AvatarNames* names,
                Clock::time_point now);

    struct Bubble
    {
        std::deque<ChatLine> lines;
        bool typing = false;
        Clock::time_point typingSince{};
        Clock::time_point latest{}; // the latest line
    };
    // An avatar's bubble if it has lines or is typing (lines stale by `now`
    // are dropped first).
    const Bubble* bubble(const Uuid& avatar, Clock::time_point now);

    const std::deque<ChatLine>& log() const { return log_; }
    // Lines ever added to the log (its newest line's number; changes with
    // each), and the number of its oldest line.
    uint64_t added() const { return added_; }
    uint64_t firstNumber() const { return added_ - log_.size() + 1; }
    void clear();

private:
    std::unordered_map<Uuid, Bubble> bubbles_;
    std::deque<ChatLine> log_;
    uint64_t added_ = 0;
};

// What's typed into local chat: "/5 text" says "text" on channel 5, as the
// official viewer's chat bar (LLChatUtils: a slash and digits, then a
// space); anything else on channel 0 ("/me" included: the region makes it
// an emote). Nothing to say if there's no text.
struct ChatInput
{
    int32_t channel = 0;
    std::string text;
};
std::optional<ChatInput> parseChatInput(std::string_view typed);

} // namespace viewer
