#pragma once

// Minimal non-blocking IPv4 UDP socket (POSIX sockets / Winsock). Second Life
// circuits are IPv4 only.

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace viewer::net
{

struct Endpoint
{
    uint32_t address = 0; // a.b.c.d is (a << 24) | (b << 16) | (c << 8) | d
    uint16_t port = 0;

    // "a.b.c.d" plus a port, or "a.b.c.d:port".
    static std::optional<Endpoint> parse(std::string_view address, uint16_t port);
    static std::optional<Endpoint> parse(std::string_view addressAndPort);
    std::string toString() const; // "a.b.c.d:port"

    auto operator<=>(const Endpoint&) const = default;
};

class UdpSocket
{
public:
#if defined(_WIN32)
    using Handle = uintptr_t;
#else
    using Handle = int;
#endif

    // Binds to 0.0.0.0 (or 127.0.0.1 if loopbackOnly) on an ephemeral port.
    static std::expected<UdpSocket, std::string> open(bool loopbackOnly = false);

    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool isOpen() const;
    Handle handle() const { return handle_; }
    Endpoint localEndpoint() const;

    // Restricts the socket to one peer: sends go there and datagrams from
    // anyone else are dropped by the OS.
    bool connect(const Endpoint& remote);

    // Sends to the connected peer, or to `to`. False if the OS refused.
    bool send(std::span<const uint8_t> data);
    bool sendTo(std::span<const uint8_t> data, const Endpoint& to);

    // Non-blocking. Returns the datagram size, or nothing if none is waiting
    // (or on error, e.g. ICMP port unreachable on a connected socket).
    std::optional<size_t> receive(std::span<uint8_t> buffer, Endpoint* from = nullptr);

private:
    void close();

#if defined(_WIN32)
    Handle handle_ = ~Handle{0};
#else
    Handle handle_ = -1;
#endif
};

// Waits until at least one socket is readable or the timeout passes, and
// marks which ones are readable. Returns false on error.
bool waitReadable(std::span<const UdpSocket::Handle> sockets, std::vector<bool>& readable,
                  std::chrono::milliseconds timeout);

} // namespace viewer::net
