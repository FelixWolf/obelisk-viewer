#include "viewer/protocol/Messages.hpp"
#include "viewer/protocol/Packet.hpp"

#include <doctest/doctest.h>

#include <format>
#include <set>
#include <string>

using namespace viewer::protocol;

namespace
{

std::string toHex(const std::vector<uint8_t>& bytes)
{
    std::string out;
    for(const uint8_t b : bytes)
    {
        out += std::format("{:02x}", b);
    }
    return out;
}

const viewer::Uuid kAgent = *viewer::Uuid::parse("d7f4aeca-88f1-42a1-b385-b9db18abb255");
const viewer::Uuid kSession = *viewer::Uuid::parse("11111111-2222-3333-4444-555555555555");

msg::AgentUpdate sampleAgentUpdate()
{
    msg::AgentUpdate m;
    m.AgentData.AgentID = kAgent;
    m.AgentData.SessionID = kSession;
    m.AgentData.BodyRotation = {0, 0, 0, 1};
    m.AgentData.HeadRotation = {0, 0, 0, 1};
    m.AgentData.State = 3;
    m.AgentData.CameraCenter = {128, 64.5f, 22};
    m.AgentData.CameraAtAxis = {1, 0, 0};
    m.AgentData.CameraLeftAxis = {0, 1, 0};
    m.AgentData.CameraUpAxis = {0, 0, 1};
    m.AgentData.Far = 64;
    m.AgentData.ControlFlags = 0x80000001;
    m.AgentData.Flags = 2;
    return m;
}

} // namespace

// Expected bytes were produced by pymetaverse (the user's Python client).
TEST_CASE("Messages encode exactly like pymetaverse")
{
    msg::UseCircuitCode use;
    use.CircuitCode.Code = 123456789;
    use.CircuitCode.SessionID = kSession;
    use.CircuitCode.ID = kAgent;
    CHECK(toHex(encodeMessage(use)) == "ffff000315cd5b0711111111222233334444555555555555d7f4aeca88"
                                       "f142a1b385b9db18abb255");

    CHECK(toHex(encodeMessage(sampleAgentUpdate())) ==
          "04d7f4aeca88f142a1b385b9db18abb25511111111222233334444555555555555"
          "000000000000000000000000000000000000000000000000" // two identity quaternions
          "0300000043000081420000b0410000803f0000000000000000000000000000803f00000000000000000000"
          "00000000803f000080420100008002");

    msg::PacketAck ack;
    ack.Packets.resize(3);
    ack.Packets[0].ID = 1;
    ack.Packets[1].ID = 2;
    ack.Packets[2].ID = 300;
    CHECK(toHex(encodeMessage(ack)) == "fffffffb0301000000020000002c010000");

    msg::ChatFromViewer chat;
    chat.AgentData.AgentID = kAgent;
    chat.ChatData.Message = fromString("hello");
    chat.ChatData.Type = 1;
    chat.ChatData.Channel = -5;
    CHECK(toHex(encodeMessage(chat)) ==
          "ffff0050d7f4aeca88f142a1b385b9db18abb25500000000000000000000000000000000060068656c6c6f00"
          "01fbffffff");
}

TEST_CASE("Packets frame exactly like pymetaverse")
{
    const auto body = encodeMessage(sampleAgentUpdate());
    const uint32_t acks[] = {7, 0x01020304};
    const auto packet = buildPacket(PacketFlags::Reliable | PacketFlags::Zerocoded, 42, body, acks);
    CHECK(toHex(packet) ==
          "d00000002a0004d7f4aeca88f142a1b385b9db18abb25511111111222233334444555555555555001803000"
          "343000281420002b0410002803f000e803f000e803f000280420100028002000000070102030402");

    const auto parsed = parsePacket(packet);
    REQUIRE(parsed.has_value());
    CHECK(parsed->sequence == 42);
    CHECK((parsed->flags & PacketFlags::Reliable) != 0);
    CHECK((parsed->acks == std::vector<uint32_t>{7, 0x01020304}));
    CHECK(parsed->message == body);

    const auto decoded = decodeMessage<msg::AgentUpdate>(parsed->message);
    REQUIRE(decoded.has_value());
    CHECK(decoded->AgentData.CameraCenter == Vector3{128, 64.5f, 22});
    CHECK(decoded->AgentData.ControlFlags == 0x80000001);
    CHECK(decoded->AgentData.BodyRotation.w == 1.0f);
}

TEST_CASE("Zero coding")
{
    const std::vector<uint8_t> data = {1, 0, 0, 0, 2, 0, 3};
    CHECK((zeroEncode(data) == std::vector<uint8_t>{1, 0, 3, 2, 0, 1, 3}));
    CHECK((zeroDecode(zeroEncode(data)).value() == data));

    // Runs longer than 255 split.
    const std::vector<uint8_t> zeros(300, 0);
    CHECK((zeroEncode(zeros) == std::vector<uint8_t>{0, 255, 0, 45}));
    CHECK((zeroDecode(zeroEncode(zeros)).value() == zeros));

    CHECK_FALSE(zeroDecode(std::vector<uint8_t>{5, 0}).has_value()); // zero without a count
    // Expansion bombs are refused.
    std::vector<uint8_t> bomb;
    for(int i = 0; i < 100; i++)
    {
        bomb.push_back(0);
        bomb.push_back(255);
    }
    CHECK_FALSE(zeroDecode(bomb, 1000).has_value());

    // Zero coding that wouldn't help is skipped and the flag cleared.
    const std::vector<uint8_t> noZeros = {0xFF, 0xFF, 0x00, 0x03, 1, 2, 3};
    const auto packet = buildPacket(PacketFlags::Zerocoded, 1, noZeros);
    CHECK((packet[0] & PacketFlags::Zerocoded) == 0);
}

TEST_CASE("Malformed packets are rejected")
{
    CHECK_FALSE(parsePacket(std::vector<uint8_t>{0, 0, 0}).has_value());
    // Extra header longer than the packet.
    CHECK_FALSE(parsePacket(std::vector<uint8_t>{0, 0, 0, 0, 1, 50, 1}).has_value());
    // Claims 3 acks but has room for none.
    CHECK_FALSE(
        parsePacket(std::vector<uint8_t>{PacketFlags::Ack, 0, 0, 0, 1, 0, 1, 3}).has_value());
}

TEST_CASE("Message numbers")
{
    CHECK(msg::StartPingCheck::kInfo.id == 1);
    CHECK(msg::StartPingCheck::kInfo.frequency == Frequency::High);
    CHECK(msg::UseCircuitCode::kInfo.id == 0xFFFF0003);
    CHECK(msg::PacketAck::kInfo.id == 0xFFFFFFFB);
    CHECK(msg::RegionHandshake::kInfo.trusted);
    CHECK(msg::AgentUpdate::kInfo.zerocoded);

    for(const MessageInfo& info : allMessages())
    {
        std::vector<uint8_t> out;
        MessageWriter w(out);
        w.messageId(info.id, info.frequency);
        const auto read = readMessageId(out);
        REQUIRE(read.has_value());
        CHECK(read->first == info.id);
        CHECK(read->second == out.size());
    }

    CHECK(allMessages().size() > 400);
    std::set<MessageId> ids;
    for(const MessageInfo& info : allMessages())
    {
        CHECK(ids.insert(info.id).second);
    }
    CHECK(findMessage(0xFFFF0003)->name == "UseCircuitCode");
    CHECK(findMessage("RegionHandshake")->id == msg::RegionHandshake::kInfo.id);
    CHECK(findMessage(0x12345678) == nullptr);
}

TEST_CASE("Messages round-trip: variable, multiple and renamed blocks")
{
    msg::CoarseLocationUpdate coarse;
    coarse.Location.resize(2);
    coarse.Location[1].X = 200;
    coarse.Index.You = -1;
    coarse.AgentData.resize(1);
    coarse.AgentData[0].AgentID = kAgent;
    const auto back = decodeMessage<msg::CoarseLocationUpdate>(encodeMessage(coarse));
    REQUIRE(back.has_value());
    REQUIRE(back->Location.size() == 2);
    CHECK(back->Location[1].X == 200);
    CHECK(back->Index.You == -1);
    CHECK(back->AgentData[0].AgentID == kAgent);

    msg::NeighborList neighbours; // Multiple 4
    neighbours.NeighborBlock[3].IP = 0x7F000001;
    neighbours.NeighborBlock[3].Port = 13005;
    const auto encoded = encodeMessage(neighbours);
    const auto neighboursBack = decodeMessage<msg::NeighborList>(encoded);
    REQUIRE(neighboursBack.has_value());
    CHECK(neighboursBack->NeighborBlock[3].IP == 0x7F000001);
    CHECK(neighboursBack->NeighborBlock[3].Port == 13005);

    // A block named like its message gets a trailing underscore.
    msg::SimulatorLoad load;
    load.SimulatorLoad_.TimeDilation = 0.5f;
    CHECK(decodeMessage<msg::SimulatorLoad>(encodeMessage(load))->SimulatorLoad_.TimeDilation ==
          0.5f);

    // Truncated messages don't decode; the wrong message number doesn't either.
    auto truncated = encodeMessage(coarse);
    truncated.resize(truncated.size() - 3);
    CHECK_FALSE(decodeMessage<msg::CoarseLocationUpdate>(truncated).has_value());
    CHECK_FALSE(decodeMessage<msg::UseCircuitCode>(encodeMessage(coarse)).has_value());
}

TEST_CASE("Quaternions are normalized and rebuilt")
{
    msg::AgentUpdate m;
    m.AgentData.BodyRotation = {0, 0, -0.6f, -0.8f}; // w < 0: sent as its negation
    const auto back = decodeMessage<msg::AgentUpdate>(encodeMessage(m));
    REQUIRE(back.has_value());
    CHECK(back->AgentData.BodyRotation.z == doctest::Approx(0.6));
    CHECK(back->AgentData.BodyRotation.w == doctest::Approx(0.8));
}
