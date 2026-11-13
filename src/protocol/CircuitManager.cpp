#include "viewer/protocol/CircuitManager.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/protocol/Messages.hpp"
#include "viewer/protocol/Packet.hpp"

#include <algorithm>
#include <deque>
#include <unordered_set>
#include <variant>

namespace viewer::protocol
{

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace
{

// How many received sequence numbers to remember for duplicate detection.
constexpr size_t kSeenHistory = 4096;
// Gaps in incoming sequence numbers: ids skipped (gaps under 16, as the
// official viewer) are lost unless they turn up within 16 round trips
// (LL_LOST_TIMEOUT_FACTOR; at most CircuitOptions::maxPacketLostTimeout).
constexpr uint32_t kMaxGap = 16;
constexpr int kLostTimeoutRoundTrips = 16;
constexpr uint32_t kSequenceMask = 0xFFFFFF; // 24-bit packet ids
constexpr size_t kMaxAcksPerPacketAck = 255;

} // namespace

struct CircuitManager::Circuit
{
    CircuitId id = 0;
    net::UdpSocket socket;
    net::Endpoint remote;

    uint32_t nextSequence = 1;
    std::vector<uint32_t> pendingAcks;
    Clock::time_point oldestPendingAck{};

    struct Unacked
    {
        std::vector<uint8_t> message;
        uint8_t flags = 0;
        Clock::time_point sentAt{};
        int resends = 0;
    };
    std::map<uint32_t, Unacked> unacked;

    std::unordered_set<uint32_t> seen;
    std::deque<uint32_t> seenOrder;
    // The next incoming sequence number in order, and skipped ones (when).
    std::optional<uint32_t> expectedIn;
    std::map<uint32_t, Clock::time_point> potentiallyLost;

    Clock::time_point lastReceived{};
    Clock::time_point lastPingSent{};
    uint8_t nextPingId = 0;
    std::optional<std::pair<uint8_t, Clock::time_point>> outstandingPing;
    std::chrono::milliseconds roundTrip{0};
    bool lostReported = false;

    CircuitStats stats;
};

struct CircuitManager::Command
{
    struct Open
    {
        CircuitId id;
        net::UdpSocket socket;
        net::Endpoint remote;
    };
    struct Close
    {
        CircuitId id;
    };
    struct Send
    {
        CircuitId id;
        std::vector<uint8_t> message;
        bool zerocoded;
        bool reliable;
    };
    std::variant<Open, Close, Send> action;
};

CircuitManager::CircuitManager(CircuitOptions options) : options_(options)
{
    auto wake = net::UdpSocket::open(true);
    if(wake)
    {
        wakeSocket_ = std::move(*wake);
        wakeEndpoint_ = wakeSocket_.localEndpoint();
    }
    else
    {
        // Without it, commands are still picked up within one poll interval.
        log::warn("circuit wake socket unavailable: {}", wake.error());
    }
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

CircuitManager::~CircuitManager()
{
    thread_.request_stop();
    wake();
    if(thread_.joinable())
    {
        thread_.join();
    }
}

std::expected<CircuitId, std::string> CircuitManager::open(const net::Endpoint& remote)
{
    auto socket = net::UdpSocket::open();
    if(!socket)
    {
        return std::unexpected(socket.error());
    }
    if(!socket->connect(remote))
    {
        return std::unexpected("couldn't connect a UDP socket to " + remote.toString());
    }
    const CircuitId id = nextId_++;
    {
        std::lock_guard lock(statsMutex_);
        stats_[id].remote = remote;
    }
    commands_.push({Command::Open{id, std::move(*socket), remote}});
    wake();
    return id;
}

void CircuitManager::close(CircuitId circuit)
{
    commands_.push({Command::Close{circuit}});
    wake();
}

void CircuitManager::sendMessage(CircuitId circuit, std::vector<uint8_t> message, bool zerocoded,
                                 bool reliable)
{
    commands_.push({Command::Send{circuit, std::move(message), zerocoded, reliable}});
    wake();
}

std::optional<CircuitStats> CircuitManager::stats(CircuitId circuit) const
{
    std::lock_guard lock(statsMutex_);
    const auto it = stats_.find(circuit);
    if(it == stats_.end())
    {
        return std::nullopt;
    }
    return it->second;
}

void CircuitManager::wake()
{
    if(wakeSocket_.isOpen())
    {
        const uint8_t byte = 0;
        wakeSocket_.sendTo({&byte, 1}, wakeEndpoint_);
    }
}

void CircuitManager::run(std::stop_token stop)
{
    std::vector<net::UdpSocket::Handle> handles;
    std::vector<Circuit*> order;
    std::vector<bool> readable;
    while(!stop.stop_requested())
    {
        handles.clear();
        order.clear();
        if(wakeSocket_.isOpen())
        {
            handles.push_back(wakeSocket_.handle());
        }
        for(auto& [id, circuit] : circuits_)
        {
            handles.push_back(circuit->socket.handle());
            order.push_back(circuit.get());
        }
        // Short timeout: resends, acks and pings are driven from here.
        net::waitReadable(handles, readable, 10ms);

        const size_t first = wakeSocket_.isOpen() ? 1 : 0;
        if(first == 1 && readable[0])
        {
            uint8_t buffer[16];
            while(wakeSocket_.receive(buffer))
            {
            }
        }

        const auto now = Clock::now();
        for(size_t i = 0; i < order.size(); i++)
        {
            if(readable[first + i])
            {
                receive(*order[i], now);
            }
        }
        // After receiving, so replies and acks can go out in the same pass.
        processCommands();
        for(auto& [id, circuit] : circuits_)
        {
            tick(*circuit, now);
        }
        if(now - lastStatsPublish_ >= 250ms)
        {
            publishStats();
            lastStatsPublish_ = now;
        }
    }
}

void CircuitManager::processCommands()
{
    std::vector<Command> commands;
    commands_.drain(commands);
    const auto now = Clock::now();
    for(Command& command : commands)
    {
        if(auto* open = std::get_if<Command::Open>(&command.action))
        {
            auto circuit = std::make_unique<Circuit>();
            circuit->id = open->id;
            circuit->socket = std::move(open->socket);
            circuit->remote = open->remote;
            circuit->lastReceived = now;
            circuit->lastPingSent = now;
            circuit->stats.remote = open->remote;
            circuits_.emplace(open->id, std::move(circuit));
        }
        else if(auto* close = std::get_if<Command::Close>(&command.action))
        {
            circuits_.erase(close->id);
            std::lock_guard lock(statsMutex_);
            stats_.erase(close->id);
        }
        else if(auto* send = std::get_if<Command::Send>(&command.action))
        {
            const auto it = circuits_.find(send->id);
            if(it != circuits_.end())
            {
                sendNow(*it->second, send->message, send->zerocoded, send->reliable, now);
            }
        }
    }
}

void CircuitManager::receive(Circuit& circuit, Clock::time_point now)
{
    // Larger than any valid packet; anything bigger is truncated and dropped
    // as malformed.
    uint8_t buffer[8192];
    while(auto size = circuit.socket.receive(buffer))
    {
        handlePacket(circuit, std::span<const uint8_t>(buffer, *size), now);
    }
}

void CircuitManager::handlePacket(Circuit& circuit, std::span<const uint8_t> data,
                                  Clock::time_point now)
{
    auto packet = parsePacket(data);
    if(!packet)
    {
        circuit.stats.malformed++;
        return;
    }
    circuit.stats.packetsIn++;
    circuit.lastReceived = now;
    circuit.lostReported = false;

    for(const uint32_t ack : packet->acks)
    {
        circuit.unacked.erase(ack);
    }
    // Ack every reliable packet, including duplicates: the first ack may
    // have been lost, which is why it was resent.
    if(packet->flags & PacketFlags::Reliable)
    {
        if(circuit.pendingAcks.empty())
        {
            circuit.oldestPendingAck = now;
        }
        circuit.pendingAcks.push_back(packet->sequence);
    }
    if(!circuit.seen.insert(packet->sequence).second)
    {
        circuit.stats.duplicates++;
        return;
    }
    circuit.seenOrder.push_back(packet->sequence);
    if(circuit.seenOrder.size() > kSeenHistory)
    {
        circuit.seen.erase(circuit.seenOrder.front());
        circuit.seenOrder.pop_front();
    }
    trackSequence(circuit, packet->sequence, (packet->flags & PacketFlags::Resent) != 0, now);

    const auto id = readMessageId(packet->message);
    if(!id)
    {
        circuit.stats.malformed++;
        return;
    }

    switch(id->first)
    {
    case msg::PacketAck::kInfo.id:
        if(auto ack = decodeMessage<msg::PacketAck>(packet->message))
        {
            for(const auto& block : ack->Packets)
            {
                circuit.unacked.erase(block.ID);
            }
        }
        return;
    case msg::StartPingCheck::kInfo.id:
        if(auto ping = decodeMessage<msg::StartPingCheck>(packet->message))
        {
            msg::CompletePingCheck reply;
            reply.PingID.PingID = ping->PingID.PingID;
            sendNow(circuit, encodeMessage(reply), msg::CompletePingCheck::kInfo.zerocoded, false,
                    now);
        }
        return;
    case msg::CompletePingCheck::kInfo.id:
        if(auto pong = decodeMessage<msg::CompletePingCheck>(packet->message))
        {
            if(circuit.outstandingPing && circuit.outstandingPing->first == pong->PingID.PingID)
            {
                circuit.roundTrip = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - circuit.outstandingPing->second);
                circuit.outstandingPing.reset();
            }
        }
        return;
    default:
        break;
    }

    CircuitEvent event;
    event.circuit = circuit.id;
    event.messageId = id->first;
    event.message = std::move(packet->message);
    event.received = now;
    events_.push(std::move(event));
}

void CircuitManager::sendNow(Circuit& circuit, std::span<const uint8_t> message, bool zerocoded,
                             bool reliable, Clock::time_point now)
{
    uint8_t flags = 0;
    if(zerocoded)
    {
        flags |= PacketFlags::Zerocoded;
    }
    if(reliable)
    {
        flags |= PacketFlags::Reliable;
    }
    const uint32_t sequence = circuit.nextSequence++;
    if(reliable)
    {
        circuit.unacked[sequence] =
            Circuit::Unacked{std::vector<uint8_t>(message.begin(), message.end()), flags, now, 0};
    }
    sendPacket(circuit, flags, sequence, message, true);
}

void CircuitManager::sendPacket(Circuit& circuit, uint8_t flags, uint32_t sequence,
                                std::span<const uint8_t> message, bool carryAcks)
{
    size_t acksUsed = 0;
    const auto packet = buildPacket(flags, sequence, message,
                                    carryAcks ? std::span<const uint32_t>(circuit.pendingAcks)
                                              : std::span<const uint32_t>(),
                                    &acksUsed);
    circuit.pendingAcks.erase(circuit.pendingAcks.begin(),
                              circuit.pendingAcks.begin() + static_cast<ptrdiff_t>(acksUsed));
    circuit.socket.send(packet);
    circuit.stats.packetsOut++;
}

void CircuitManager::flushAcks(Circuit& circuit)
{
    while(!circuit.pendingAcks.empty())
    {
        const size_t count = std::min(circuit.pendingAcks.size(), kMaxAcksPerPacketAck);
        msg::PacketAck ack;
        ack.Packets.resize(count);
        for(size_t i = 0; i < count; i++)
        {
            ack.Packets[i].ID = circuit.pendingAcks[i];
        }
        circuit.pendingAcks.erase(circuit.pendingAcks.begin(),
                                  circuit.pendingAcks.begin() + static_cast<ptrdiff_t>(count));
        sendPacket(circuit, 0, circuit.nextSequence++, encodeMessage(ack), false);
    }
}

void CircuitManager::trackSequence(Circuit& circuit, uint32_t sequence, bool resent,
                                   Clock::time_point now)
{
    if(!circuit.expectedIn)
    {
        circuit.expectedIn = (sequence + 1) & kSequenceMask;
        return;
    }
    const uint32_t ahead = (sequence - *circuit.expectedIn) & kSequenceMask;
    if(ahead == 0)
    {
        circuit.expectedIn = (sequence + 1) & kSequenceMask;
    }
    else if(ahead < kSequenceMask / 2)
    {
        // Past a gap. A resend coming out of order says nothing about loss.
        if(!resent && ahead < kMaxGap)
        {
            for(uint32_t i = 0; i < ahead; i++)
            {
                circuit.potentiallyLost.emplace((*circuit.expectedIn + i) & kSequenceMask, now);
            }
        }
        circuit.expectedIn = (sequence + 1) & kSequenceMask;
    }
    else
    {
        // Late: one we skipped turned up after all.
        circuit.potentiallyLost.erase(sequence);
    }
}

void CircuitManager::tick(Circuit& circuit, Clock::time_point now)
{
    // Skipped packets that didn't turn up are lost.
    const auto lostAfter =
        circuit.roundTrip > 0ms
            ? std::min<Clock::duration>(circuit.roundTrip * kLostTimeoutRoundTrips,
                                        options_.maxPacketLostTimeout)
            : Clock::duration(options_.maxPacketLostTimeout);
    for(auto it = circuit.potentiallyLost.begin(); it != circuit.potentiallyLost.end();)
    {
        if(now - it->second >= lostAfter)
        {
            circuit.stats.packetsLost++;
            it = circuit.potentiallyLost.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // Resend reliable packets that weren't acked in time.
    const auto timeout =
        std::clamp(circuit.roundTrip * 3, options_.minResendTimeout, options_.maxResendTimeout);
    for(auto it = circuit.unacked.begin(); it != circuit.unacked.end();)
    {
        Circuit::Unacked& u = it->second;
        if(now - u.sentAt < timeout)
        {
            ++it;
            continue;
        }
        if(u.resends >= options_.maxResends)
        {
            circuit.stats.reliableDropped++;
            const auto id = readMessageId(u.message);
            log::warn("circuit {}: giving up on reliable packet {} (message 0x{:08X})",
                      circuit.remote.toString(), it->first, id ? id->first : 0);
            it = circuit.unacked.erase(it);
            continue;
        }
        u.resends++;
        u.sentAt = now;
        circuit.stats.resent++;
        sendPacket(circuit, static_cast<uint8_t>(u.flags | PacketFlags::Resent), it->first,
                   u.message, true);
        ++it;
    }

    if(!circuit.pendingAcks.empty() && now - circuit.oldestPendingAck >= options_.ackDelay)
    {
        flushAcks(circuit);
    }

    if(now - circuit.lastPingSent >= options_.pingInterval)
    {
        msg::StartPingCheck ping;
        ping.PingID.PingID = circuit.nextPingId++;
        ping.PingID.OldestUnacked =
            circuit.unacked.empty() ? circuit.nextSequence : circuit.unacked.begin()->first;
        circuit.outstandingPing = {ping.PingID.PingID, now};
        circuit.lastPingSent = now;
        sendNow(circuit, encodeMessage(ping), msg::StartPingCheck::kInfo.zerocoded, false, now);
    }

    if(!circuit.lostReported && now - circuit.lastReceived >= options_.lostTimeout)
    {
        circuit.lostReported = true;
        events_.push(CircuitEvent{CircuitEvent::Kind::Lost, circuit.id, 0, {}, now});
    }
}

void CircuitManager::publishStats()
{
    std::lock_guard lock(statsMutex_);
    for(auto& [id, circuit] : circuits_)
    {
        circuit->stats.roundTrip = circuit->roundTrip;
        circuit->stats.unacked = circuit->unacked.size();
        circuit->stats.lastReceived = circuit->lastReceived;
        stats_[id] = circuit->stats;
    }
}

} // namespace viewer::protocol
