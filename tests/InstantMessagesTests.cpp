#include "InstantMessages.hpp"

#include "viewer/protocol/Messages.hpp"

#include <doctest/doctest.h>

#include <string>

using namespace viewer;
namespace msg = viewer::protocol::msg;

namespace
{

const Uuid kSelf = *Uuid::parse("11111111-1111-1111-1111-111111111111");
const Uuid kAnn = *Uuid::parse("22222222-2222-2222-2222-222222222222");
const Uuid kGroup = *Uuid::parse("6a1b2c3d-0000-4000-8000-00000000abcd");
const Uuid kObject = *Uuid::parse("44444444-4444-4444-4444-444444444444");
const auto kNow = std::chrono::system_clock::time_point(std::chrono::seconds(1'700'000'000));

msg::ImprovedInstantMessage im(const Uuid& from, const Uuid& to, uint8_t dialog,
                               const Uuid& session, std::string text,
                               std::string name = "Ann Resident")
{
    msg::ImprovedInstantMessage m;
    m.AgentData.AgentID = from;
    m.MessageBlock.ToAgentID = to;
    m.MessageBlock.Dialog = dialog;
    m.MessageBlock.ID = session;
    m.MessageBlock.FromAgentName = protocol::fromString(name);
    m.MessageBlock.Message = protocol::fromString(text);
    m.MessageBlock.BinaryBucket = {0};
    return m;
}

} // namespace

TEST_CASE("Instant messages: one to one, its session the ids XORed")
{
    const Uuid session = directSession(kSelf, kAnn);
    CHECK(session == directSession(kAnn, kSelf));
    CHECK(session != kSelf);
    CHECK(directSession(kSelf, kSelf) == kSelf);

    InstantMessages ims;
    ims.handle(im(kAnn, kSelf, ImDialog::NothingSpecial, session, "hi!"), kSelf, kNow);
    REQUIRE(ims.conversations().size() == 1);
    const ImConversation& c = ims.conversations()[0];
    CHECK(c.kind == ImConversation::Kind::Direct);
    CHECK(c.other == kAnn);
    CHECK(c.name == "Ann"); // no " Resident"
    REQUIRE(c.lines.size() == 1);
    CHECK(c.lines[0].text == "hi!");
    CHECK_FALSE(c.lines[0].ours);

    // Ours shows at once, and goes to Ann.
    const auto out = ims.send(session, "hello", kSelf, "Me Resident", kNow);
    REQUIRE(out);
    CHECK(out->to == kAnn);
    CHECK(out->session == session);
    CHECK(out->dialog == ImDialog::NothingSpecial);
    REQUIRE(ims.find(session)->lines.size() == 2);
    CHECK(ims.find(session)->lines[1].ours);
    CHECK(ims.find(session)->lines[1].fromName == "Me");
    CHECK_FALSE(ims.send(session, "", kSelf, "Me", kNow));
    CHECK_FALSE(ims.send(kGroup, "hi", kSelf, "Me", kNow)); // no such conversation
}

TEST_CASE("Instant messages: typing, automatic answers, and objects' messages")
{
    InstantMessages ims;
    const Uuid session = directSession(kSelf, kAnn);
    ims.handle(im(kAnn, kSelf, ImDialog::TypingStart, session, ""), kSelf, kNow);
    REQUIRE(ims.find(session));
    CHECK(ims.find(session)->typing);
    CHECK(ims.find(session)->lines.empty());
    ims.handle(im(kAnn, kSelf, ImDialog::NothingSpecial, session, "there"), kSelf, kNow);
    CHECK_FALSE(ims.find(session)->typing); // a line ends it

    ims.handle(im(kAnn, kSelf, ImDialog::BusyAutoResponse, session, "Busy!"), kSelf, kNow);
    CHECK(ims.find(session)->lines.back().response);

    const auto object = ims.handle(
        im(kAnn, kSelf, ImDialog::FromTask, kObject, "Your rent is due", "Rent Box"), kSelf, kNow);
    REQUIRE(object);
    CHECK(object->object == kObject);
    CHECK(object->owner == kAnn);
    CHECK(object->name == "Rent Box");
    CHECK(ims.conversations().size() == 1); // not a conversation
}

TEST_CASE("Instant messages: group chats start with an invitation, then come as session sends")
{
    InstantMessages ims;
    // Before the invitation: not ours to see.
    ims.handle(im(kAnn, kGroup, ImDialog::SessionSend, kGroup, "early"), kSelf, kNow);
    CHECK(ims.conversations().empty());

    llsd::Value body;
    llsd::Value& p = body["instantmessage"]["message_params"];
    p["id"] = kGroup;
    p["from_id"] = kAnn;
    p["from_name"] = "Ann Resident";
    p["message"] = "Hello group";
    const std::string name = "Builders";
    llsd::Binary bucket(name.begin(), name.end());
    bucket.push_back(0);
    p["data"]["binary_bucket"] = bucket;
    const auto accept = ims.invitation(body, kSelf, kNow);
    REQUIRE(accept);
    CHECK(*accept == kGroup);
    const ImConversation* c = ims.find(kGroup);
    REQUIRE(c);
    CHECK(c->kind == ImConversation::Kind::Session);
    CHECK(c->name == "Builders");
    REQUIRE(c->lines.size() == 1);
    CHECK(c->lines[0].fromName == "Ann");

    ims.handle(im(kAnn, kGroup, ImDialog::SessionSend, kGroup, "second"), kSelf, kNow);
    CHECK(ims.find(kGroup)->lines.size() == 2);

    // Ours goes to the session and isn't shown until it comes back.
    const auto out = ims.send(kGroup, "me too", kSelf, "Me", kNow);
    REQUIRE(out);
    CHECK(out->to == kGroup);
    CHECK(out->dialog == ImDialog::SessionSend);
    CHECK(ims.find(kGroup)->lines.size() == 2);
    ims.handle(im(kSelf, kGroup, ImDialog::SessionSend, kGroup, "me too", "Me Resident"), kSelf,
               kNow);
    REQUIRE(ims.find(kGroup)->lines.size() == 3);
    CHECK(ims.find(kGroup)->lines.back().ours);
}

TEST_CASE("Instant messages: what came while away, newest conversations first")
{
    InstantMessages ims;
    llsd::Value reply;
    llsd::Value& m0 = reply["messages"][0];
    m0["from_agent_id"] = kAnn;
    m0["from_agent_name"] = "Ann Resident";
    m0["dialog"] = 0;
    m0["message"] = "Are you there?";
    m0["timestamp"] = 1'600'000'000;
    llsd::Value& m1 = reply["messages"][1];
    m1["from_agent_id"] = kAnn;
    m1["from_agent_name"] = "Rent Box";
    m1["dialog"] = static_cast<int32_t>(ImDialog::FromTask);
    m1["transaction-id"] = kObject;
    m1["message"] = "Rent due";
    const auto objects = ims.offline(reply, kSelf);
    REQUIRE(objects.size() == 1);
    CHECK(objects[0].text == "Rent due");
    const ImConversation* c = ims.find(directSession(kSelf, kAnn));
    REQUIRE(c);
    REQUIRE(c->lines.size() == 1);
    CHECK(c->lines[0].saved);
    CHECK(c->lines[0].when ==
          std::chrono::system_clock::time_point(std::chrono::seconds(1'600'000'000)));

    // A newer one elsewhere goes first.
    const Uuid bo = *Uuid::parse("55555555-5555-5555-5555-555555555555");
    ims.handle(im(bo, kSelf, ImDialog::NothingSpecial, directSession(kSelf, bo), "yo", "Bo Smith"),
               kSelf, kNow);
    REQUIRE(ims.conversations().size() == 2);
    CHECK(ims.conversations()[0].other == bo);

    const uint64_t before = ims.version();
    ims.clear();
    CHECK(ims.conversations().empty());
    CHECK(ims.version() != before);
}
