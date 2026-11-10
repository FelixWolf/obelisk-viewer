// CircuitManager against fake simulators on the loopback interface.
#include "FakeSimulator.hpp"

#include "viewer/protocol/CircuitManager.hpp"

#include <doctest/doctest.h>

using namespace viewer;
using namespace viewer::protocol;
using namespace std::chrono_literals;
using viewer::test::FakeSimulator;

namespace
{

CircuitOptions fastOptions()
{
    CircuitOptions o;
    o.minResendTimeout = 100ms;
    o.maxResendTimeout = 100ms;
    o.ackDelay = 10ms;
    o.pingInterval = 60s; // keep pings out of the way unless a test wants them
    return o;
}

// Drains events until one matches, or the timeout passes.
std::optional<CircuitEvent> waitForEvent(CircuitManager& circuits, auto predicate,
                                         std::chrono::milliseconds timeout = 3s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::vector<CircuitEvent> events;
    while(std::chrono::steady_clock::now() < deadline)
    {
        circuits.drain(events);
        for(auto& e : events)
        {
            if(predicate(e))
            {
                return std::move(e);
            }
        }
        std::this_thread::sleep_for(5ms);
    }
    return std::nullopt;
}

msg::RegionHandshake handshake(std::string_view name)
{
    msg::RegionHandshake m;
    m.RegionInfo.SimName = fromString(name);
    return m;
}

} // namespace

TEST_CASE("Circuit: reliable packets are resent until acked")
{
    FakeSimulator sim;
    CircuitManager circuits(fastOptions());
    const CircuitId id = circuits.open(sim.endpoint()).value();

    msg::UseCircuitCode use;
    use.CircuitCode.Code = 99;
    circuits.send(id, use, true);

    const auto first = sim.waitFor<msg::UseCircuitCode>();
    REQUIRE(first.has_value());
    CHECK(first->second.CircuitCode.Code == 99);
    CHECK((first->first.packet.flags & PacketFlags::Reliable) != 0);
    CHECK((first->first.packet.flags & PacketFlags::Resent) == 0);

    // Not acked: it comes again, flagged as resent, with the same sequence.
    const auto again = sim.waitFor<msg::UseCircuitCode>();
    REQUIRE(again.has_value());
    CHECK(again->first.packet.sequence == first->first.packet.sequence);
    CHECK((again->first.packet.flags & PacketFlags::Resent) != 0);

    sim.ack(first->first.packet.sequence);
    std::this_thread::sleep_for(400ms);
    while(sim.take<msg::UseCircuitCode>())
    {
    }
    std::this_thread::sleep_for(300ms);
    CHECK_FALSE(sim.take<msg::UseCircuitCode>().has_value()); // no more resends
    CHECK(circuits.stats(id)->unacked == 0);
    CHECK(circuits.stats(id)->resent >= 1);
}

TEST_CASE("Circuit: gives up on a reliable packet after maxResends")
{
    FakeSimulator sim;
    CircuitOptions options = fastOptions();
    options.maxResends = 2;
    options.minResendTimeout = options.maxResendTimeout = 30ms;
    CircuitManager circuits(options);
    const CircuitId id = circuits.open(sim.endpoint()).value();
    circuits.send(id, msg::LogoutRequest{}, true);
    std::this_thread::sleep_for(500ms);
    CHECK(sim.count<msg::LogoutRequest>() == 3); // original + 2 resends
    CHECK(circuits.stats(id)->reliableDropped == 1);
}

TEST_CASE("Circuit: incoming reliable packets are acked once delivered once")
{
    FakeSimulator sim;
    CircuitManager circuits(fastOptions());
    const CircuitId id = circuits.open(sim.endpoint()).value();
    circuits.send(id, msg::UseCircuitCode{}); // lets the fake learn our port
    REQUIRE(sim.waitFor<msg::UseCircuitCode>().has_value());

    sim.send(handshake("Alpha"), true, 10);
    sim.send(handshake("Alpha"), true, 10); // resent duplicate
    sim.send(handshake("Beta"), true, 11);

    std::vector<std::string> names;
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    std::vector<CircuitEvent> events;
    while(std::chrono::steady_clock::now() < deadline)
    {
        circuits.drain(events);
        for(const auto& e : events)
        {
            REQUIRE(e.circuit == id);
            if(auto m = e.decode<msg::RegionHandshake>())
            {
                names.push_back(toString(m->RegionInfo.SimName));
            }
        }
        std::this_thread::sleep_for(10ms);
    }
    CHECK((names == std::vector<std::string>{"Alpha", "Beta"}));
    const auto acked = sim.ackedSequences();
    CHECK(acked.contains(10));
    CHECK(acked.contains(11));
    CHECK(circuits.stats(id)->duplicates == 1);
}

TEST_CASE("Circuit: pings are answered and not passed on")
{
    FakeSimulator sim;
    CircuitOptions options = fastOptions();
    options.pingInterval = 100ms;
    CircuitManager circuits(options);
    const CircuitId id = circuits.open(sim.endpoint()).value();

    // We ping the simulator...
    const auto ping = sim.waitFor<msg::StartPingCheck>();
    REQUIRE(ping.has_value());
    msg::CompletePingCheck pong;
    pong.PingID.PingID = ping->second.PingID.PingID;
    sim.send(pong);

    // ...and answer its pings.
    msg::StartPingCheck theirs;
    theirs.PingID.PingID = 77;
    sim.send(theirs);
    const auto answer = sim.waitFor<msg::CompletePingCheck>();
    REQUIRE(answer.has_value());
    CHECK(answer->second.PingID.PingID == 77);

    const auto passedOn = waitForEvent(
        circuits, [](const CircuitEvent& e)
        { return e.is<msg::StartPingCheck>() || e.is<msg::CompletePingCheck>(); }, 300ms);
    CHECK_FALSE(passedOn.has_value());
    (void)id;
}

TEST_CASE("Circuit: several simulators at once")
{
    FakeSimulator a;
    FakeSimulator b;
    CircuitManager circuits(fastOptions());
    const CircuitId idA = circuits.open(a.endpoint()).value();
    const CircuitId idB = circuits.open(b.endpoint()).value();
    CHECK(idA != idB);

    msg::UseCircuitCode toA;
    toA.CircuitCode.Code = 1;
    msg::UseCircuitCode toB;
    toB.CircuitCode.Code = 2;
    circuits.send(idA, toA);
    circuits.send(idB, toB);
    CHECK(a.waitFor<msg::UseCircuitCode>()->second.CircuitCode.Code == 1);
    CHECK(b.waitFor<msg::UseCircuitCode>()->second.CircuitCode.Code == 2);
    // Each simulator talks to its own socket.
    CHECK(a.peer() != b.peer());

    b.send(handshake("from b"));
    a.send(handshake("from a"));
    std::map<CircuitId, std::string> names;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    std::vector<CircuitEvent> events;
    while(names.size() < 2 && std::chrono::steady_clock::now() < deadline)
    {
        circuits.drain(events);
        for(const auto& e : events)
        {
            if(auto m = e.decode<msg::RegionHandshake>())
            {
                names[e.circuit] = toString(m->RegionInfo.SimName);
            }
        }
        std::this_thread::sleep_for(5ms);
    }
    CHECK(names[idA] == "from a");
    CHECK(names[idB] == "from b");

    // Closing one leaves the other working.
    circuits.close(idA);
    b.send(handshake("still here"));
    CHECK(waitForEvent(circuits, [&](const CircuitEvent& e) { return e.circuit == idB; })
              .has_value());
}

TEST_CASE("Circuit: datagrams from strangers are ignored")
{
    FakeSimulator sim;
    CircuitManager circuits(fastOptions());
    const CircuitId id = circuits.open(sim.endpoint()).value();
    circuits.send(id, msg::UseCircuitCode{});
    REQUIRE(sim.waitFor<msg::UseCircuitCode>().has_value());

    // The stranger sends to our circuit's port.
    auto socket = net::UdpSocket::open(true).value();
    socket.sendTo(buildPacket(0, 1, encodeMessage(handshake("intruder"))), sim.peer());
    CHECK_FALSE(
        waitForEvent(circuits, [](const CircuitEvent&) { return true; }, 300ms).has_value());
}

TEST_CASE("Circuit: silence is reported as lost")
{
    FakeSimulator sim;
    CircuitOptions options = fastOptions();
    options.lostTimeout = 200ms;
    CircuitManager circuits(options);
    const CircuitId id = circuits.open(sim.endpoint()).value();
    const auto lost = waitForEvent(
        circuits, [](const CircuitEvent& e) { return e.kind == CircuitEvent::Kind::Lost; }, 2s);
    REQUIRE(lost.has_value());
    CHECK(lost->circuit == id);
}
