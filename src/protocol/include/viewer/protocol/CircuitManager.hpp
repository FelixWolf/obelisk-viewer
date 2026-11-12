#pragma once

// UDP circuits to simulators. One CircuitManager runs one network thread that
// serves every circuit; each circuit (one per simulator) has its own UDP
// socket connected to that simulator, so the OS drops datagrams from anyone
// else.
//
// The network thread handles the transport layer by itself: sequence
// numbers, acks (piggybacked, or PacketAck when there's nothing to carry
// them), resending reliable packets, dropping duplicates, and answering and
// sending pings. Every other message is handed to the owner as a
// CircuitEvent, still encoded; decode the ones you care about with
// event.decode<msg::Foo>().
//
// Threading: all public functions may be called from any thread. Events are
// collected with drain(), normally by the Session on the main thread.

#include "viewer/core/MessageQueue.hpp"
#include "viewer/net/UdpSocket.hpp"
#include "viewer/protocol/MessageTypes.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace viewer::protocol
{

using CircuitId = uint32_t;

struct CircuitEvent
{
    enum class Kind : uint8_t
    {
        Message, // a message arrived
        Lost,    // nothing received for CircuitOptions::lostTimeout
    };
    Kind kind = Kind::Message;
    CircuitId circuit = 0;
    MessageId messageId = 0;
    std::vector<uint8_t> message{};                   // message number + body, zero-decoded
    std::chrono::steady_clock::time_point received{}; // when the packet arrived

    template <Message M>
    bool is() const
    {
        return kind == Kind::Message && messageId == M::kInfo.id;
    }
    template <Message M>
    std::optional<M> decode() const
    {
        return decodeMessage<M>(message);
    }
};

struct CircuitOptions
{
    std::chrono::milliseconds pingInterval{5'000};
    std::chrono::milliseconds lostTimeout{30'000};
    // Acks that couldn't ride along on outgoing packets go out as PacketAck
    // after at most this long.
    std::chrono::milliseconds ackDelay{50};
    // Reliable packets are resent after max(min, 3 x round trip), at most
    // maxResends times, then dropped.
    std::chrono::milliseconds minResendTimeout{500};
    std::chrono::milliseconds maxResendTimeout{5'000};
    int maxResends = 4;
};

struct CircuitStats
{
    net::Endpoint remote{};
    std::chrono::milliseconds roundTrip{0};
    uint64_t packetsIn = 0;
    uint64_t packetsOut = 0;
    uint64_t resent = 0;
    uint64_t reliableDropped = 0; // gave up after maxResends
    uint64_t duplicates = 0;
    uint64_t malformed = 0;
    size_t unacked = 0;
    std::chrono::steady_clock::time_point lastReceived{}; // the latest packet's arrival
};

class CircuitManager
{
public:
    explicit CircuitManager(CircuitOptions options = {});
    // Stops the network thread and closes every circuit without sending
    // anything (log out first if the simulators should know).
    ~CircuitManager();

    CircuitManager(const CircuitManager&) = delete;
    CircuitManager& operator=(const CircuitManager&) = delete;

    std::expected<CircuitId, std::string> open(const net::Endpoint& remote);
    void close(CircuitId circuit);

    template <Message M>
    void send(CircuitId circuit, const M& message, bool reliable = false)
    {
        sendMessage(circuit, encodeMessage(message), M::kInfo.zerocoded, reliable);
    }
    // `message` is the message number plus body (see encodeMessage()).
    void sendMessage(CircuitId circuit, std::vector<uint8_t> message, bool zerocoded,
                     bool reliable);

    void drain(std::vector<CircuitEvent>& out) { events_.drain(out); }

    // A snapshot, refreshed by the network thread a few times per second.
    std::optional<CircuitStats> stats(CircuitId circuit) const;

private:
    struct Circuit;
    struct Command;

    void run(std::stop_token stop);
    void wake();
    void processCommands();
    void receive(Circuit& circuit, std::chrono::steady_clock::time_point now);
    void handlePacket(Circuit& circuit, std::span<const uint8_t> data,
                      std::chrono::steady_clock::time_point now);
    void tick(Circuit& circuit, std::chrono::steady_clock::time_point now);
    void sendPacket(Circuit& circuit, uint8_t flags, uint32_t sequence,
                    std::span<const uint8_t> message, bool carryAcks);
    void sendNow(Circuit& circuit, std::span<const uint8_t> message, bool zerocoded, bool reliable,
                 std::chrono::steady_clock::time_point now);
    void flushAcks(Circuit& circuit);
    void publishStats();

    CircuitOptions options_;
    std::atomic<CircuitId> nextId_{1};
    MessageQueue<Command> commands_;
    MessageQueue<CircuitEvent> events_;
    net::UdpSocket wakeSocket_;
    net::Endpoint wakeEndpoint_;

    mutable std::mutex statsMutex_;
    std::map<CircuitId, CircuitStats> stats_;

    // Owned by the network thread.
    std::map<CircuitId, std::unique_ptr<Circuit>> circuits_;
    std::chrono::steady_clock::time_point lastStatsPublish_{};

    std::jthread thread_; // last: started after everything above exists
};

} // namespace viewer::protocol
