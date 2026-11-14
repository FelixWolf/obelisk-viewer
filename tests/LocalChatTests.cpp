#include "LocalChat.hpp"

#include <doctest/doctest.h>

#include <string>

using namespace viewer;

namespace
{

const Uuid kAvatar = *Uuid::parse("6f1cbf7d-7945-47cb-8428-8cc14a60e94c");
const Uuid kObject = *Uuid::parse("39654a9d-4b26-08d3-353e-2b8b37eb5bdc");

void hear(LocalChat& chat, uint8_t source, uint8_t type, const Uuid& from, std::string text,
          LocalChat::Clock::time_point now, uint8_t audible = 1)
{
    chat.handle(source, type, audible, from, {}, "Some One", std::move(text), nullptr, now);
}

} // namespace

TEST_CASE("Local chat: lines as shown, and who heard them")
{
    LocalChat chat;
    const auto t0 = LocalChat::Clock::now();
    hear(chat, ChatSource::Agent, ChatType::Normal, kAvatar, "hello", t0);
    hear(chat, ChatSource::Agent, ChatType::Whisper, kAvatar, "psst", t0);
    hear(chat, ChatSource::Agent, ChatType::Shout, kAvatar, "HEY", t0);
    hear(chat, ChatSource::Agent, ChatType::Normal, kAvatar, "/me waves", t0);
    hear(chat, ChatSource::Agent, ChatType::Normal, kAvatar, "/me's here", t0);
    hear(chat, ChatSource::Object, ChatType::Normal, kObject, "a\tb", t0);
    hear(chat, ChatSource::Agent, ChatType::Normal, kAvatar, "too far", t0, 0);
    REQUIRE(chat.log().size() == 6);
    CHECK(chat.log()[0].text == "hello");
    CHECK(chat.log()[1].text == "whispers: psst");
    CHECK(chat.log()[2].text == "shouts: HEY");
    CHECK(chat.log()[3].text == "Some One waves");
    CHECK(chat.log()[3].emote);
    CHECK(chat.log()[4].text == "Some One's here");
    CHECK(chat.log()[5].text == "a    b");

    // Avatars' lines go in their bubble; objects' don't.
    const LocalChat::Bubble* bubble = chat.bubble(kAvatar, t0);
    REQUIRE(bubble);
    CHECK(bubble->lines.size() == 5);
    CHECK(chat.bubble(kObject, t0) == nullptr);
}

TEST_CASE("Local chat: bubbles type, keep 12 lines, and go 24 s after the latest")
{
    LocalChat chat;
    const auto t0 = LocalChat::Clock::now();
    hear(chat, ChatSource::Agent, ChatType::StartTyping, kAvatar, "", t0);
    const LocalChat::Bubble* typing = chat.bubble(kAvatar, t0);
    REQUIRE(typing);
    CHECK(typing->typing);
    CHECK(typing->lines.empty());
    CHECK(chat.log().empty());
    hear(chat, ChatSource::Agent, ChatType::StopTyping, kAvatar, "", t0);
    CHECK(chat.bubble(kAvatar, t0) == nullptr);

    hear(chat, ChatSource::Agent, ChatType::StartTyping, kAvatar, "", t0);
    for(int i = 0; i < 15; i++)
    {
        hear(chat, ChatSource::Agent, ChatType::Normal, kAvatar, std::to_string(i), t0);
    }
    const LocalChat::Bubble* full = chat.bubble(kAvatar, t0);
    REQUIRE(full);
    CHECK_FALSE(full->typing); // a line ends typing
    CHECK(full->lines.size() == 12);
    CHECK(full->lines.front().text == "3");

    CHECK(chat.bubble(kAvatar, t0 + std::chrono::seconds(20)) != nullptr);
    CHECK(chat.bubble(kAvatar, t0 + std::chrono::seconds(25)) == nullptr);
    CHECK(chat.log().size() == 15); // the log keeps them
}
