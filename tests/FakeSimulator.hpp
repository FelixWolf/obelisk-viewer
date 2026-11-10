#pragma once

// A pretend simulator on 127.0.0.1 for circuit and session tests. It records
// every packet it receives (on its own thread) and can send messages back to
// whoever last talked to it.

#include "viewer/net/UdpSocket.hpp"
#include "viewer/protocol/Messages.hpp"
#include "viewer/protocol/Packet.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

namespace viewer::test
{

class FakeSimulator
{
public:
    struct Received
    {
        protocol::Packet packet;
        protocol::MessageId id = 0;
        net::Endpoint from;
    };

    FakeSimulator()
    {
        socket_ = std::move(net::UdpSocket::open(true).value());
        endpoint_ = socket_.localEndpoint();
        thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
    }

    const net::Endpoint& endpoint() const { return endpoint_; }

    // Waits for a message of type M that hasn't been taken yet, and takes it.
    template <protocol::Message M>
    std::optional<std::pair<Received, M>>
    waitFor(std::chrono::milliseconds timeout = std::chrono::seconds(3))
    {
        std::unique_lock lock(mutex_);
        std::optional<std::pair<Received, M>> result;
        arrived_.wait_for(lock, timeout, [&] { return (result = takeLocked<M>()).has_value(); });
        return result;
    }

    // Non-blocking version of waitFor.
    template <protocol::Message M>
    std::optional<std::pair<Received, M>> take()
    {
        std::lock_guard lock(mutex_);
        return takeLocked<M>();
    }

    template <protocol::Message M>
    size_t count()
    {
        std::lock_guard lock(mutex_);
        size_t n = 0;
        for(const auto& r : received_)
        {
            n += r.id == M::kInfo.id ? 1 : 0;
        }
        return n;
    }

    // Every sequence number the viewer acknowledged, appended or in PacketAck.
    std::set<uint32_t> ackedSequences()
    {
        std::lock_guard lock(mutex_);
        std::set<uint32_t> acked;
        for(const auto& r : received_)
        {
            acked.insert(r.packet.acks.begin(), r.packet.acks.end());
            if(r.id == protocol::msg::PacketAck::kInfo.id)
            {
                if(auto ack = protocol::decodeMessage<protocol::msg::PacketAck>(r.packet.message))
                {
                    for(const auto& block : ack->Packets)
                    {
                        acked.insert(block.ID);
                    }
                }
            }
        }
        return acked;
    }

    // Sends to the last peer; returns the sequence number used.
    template <protocol::Message M>
    uint32_t send(const M& message, bool reliable = false, std::optional<uint32_t> sequence = {})
    {
        const uint32_t seq = sequence.value_or(nextSequence_++);
        const auto packet = protocol::buildPacket(
            static_cast<uint8_t>(reliable ? protocol::PacketFlags::Reliable : 0), seq,
            protocol::encodeMessage(message));
        std::lock_guard lock(mutex_);
        socket_.sendTo(packet, peer_);
        return seq;
    }

    void ack(uint32_t sequence)
    {
        protocol::msg::PacketAck ack;
        ack.Packets.resize(1);
        ack.Packets[0].ID = sequence;
        send(ack);
    }

    net::Endpoint peer()
    {
        std::lock_guard lock(mutex_);
        return peer_;
    }

private:
    template <protocol::Message M>
    std::optional<std::pair<Received, M>> takeLocked()
    {
        for(size_t i = 0; i < received_.size(); i++)
        {
            if(taken_[i] || received_[i].id != M::kInfo.id)
            {
                continue;
            }
            auto message = protocol::decodeMessage<M>(received_[i].packet.message);
            if(!message)
            {
                continue;
            }
            taken_[i] = true;
            return std::pair<Received, M>{received_[i], std::move(*message)};
        }
        return std::nullopt;
    }

    void run(std::stop_token stop)
    {
        std::vector<bool> readable;
        uint8_t buffer[8192];
        while(!stop.stop_requested())
        {
            const net::UdpSocket::Handle handle = socket_.handle();
            net::waitReadable({&handle, 1}, readable, std::chrono::milliseconds(10));
            net::Endpoint from;
            while(auto size = socket_.receive(buffer, &from))
            {
                auto packet = protocol::parsePacket({buffer, *size});
                if(!packet)
                {
                    continue;
                }
                const auto id = protocol::readMessageId(packet->message);
                std::lock_guard lock(mutex_);
                peer_ = from;
                received_.push_back(Received{std::move(*packet), id ? id->first : 0, from});
                taken_.push_back(false);
                arrived_.notify_all();
            }
        }
    }

    net::UdpSocket socket_;
    net::Endpoint endpoint_;
    uint32_t nextSequence_ = 1;
    std::mutex mutex_;
    std::condition_variable arrived_;
    net::Endpoint peer_;
    std::vector<Received> received_;
    std::vector<bool> taken_;
    std::jthread thread_;
};

} // namespace viewer::test
