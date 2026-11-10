#include "viewer/net/UdpSocket.hpp"

#include <charconv>
#include <cstring>
#include <format>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace viewer::net
{

namespace
{

#if defined(_WIN32)
constexpr UdpSocket::Handle kInvalid = INVALID_SOCKET;
std::string lastError()
{
    return std::format("winsock error {}", WSAGetLastError());
}
#else
constexpr UdpSocket::Handle kInvalid = -1;
std::string lastError()
{
    return std::strerror(errno);
}
#endif

sockaddr_in toSockaddr(const Endpoint& e)
{
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(e.address);
    a.sin_port = htons(e.port);
    return a;
}

Endpoint fromSockaddr(const sockaddr_in& a)
{
    return Endpoint{ntohl(a.sin_addr.s_addr), ntohs(a.sin_port)};
}

} // namespace

std::optional<Endpoint> Endpoint::parse(std::string_view address, uint16_t port)
{
    uint32_t value = 0;
    const char* p = address.data();
    const char* end = address.data() + address.size();
    for(int part = 0; part < 4; part++)
    {
        if(part > 0)
        {
            if(p == end || *p != '.')
            {
                return std::nullopt;
            }
            p++;
        }
        unsigned int octet = 0;
        const auto result = std::from_chars(p, end, octet);
        if(result.ec != std::errc() || octet > 255 || result.ptr == p)
        {
            return std::nullopt;
        }
        p = result.ptr;
        value = value << 8 | octet;
    }
    if(p != end)
    {
        return std::nullopt;
    }
    return Endpoint{value, port};
}

std::optional<Endpoint> Endpoint::parse(std::string_view addressAndPort)
{
    const auto colon = addressAndPort.rfind(':');
    if(colon == std::string_view::npos)
    {
        return std::nullopt;
    }
    const std::string_view portText = addressAndPort.substr(colon + 1);
    uint16_t port = 0;
    const auto result = std::from_chars(portText.data(), portText.data() + portText.size(), port);
    if(result.ec != std::errc() || result.ptr != portText.data() + portText.size())
    {
        return std::nullopt;
    }
    return parse(addressAndPort.substr(0, colon), port);
}

std::string Endpoint::toString() const
{
    return std::format("{}.{}.{}.{}:{}", address >> 24, (address >> 16) & 0xFF,
                       (address >> 8) & 0xFF, address & 0xFF, port);
}

std::expected<UdpSocket, std::string> UdpSocket::open(bool loopbackOnly)
{
#if defined(_WIN32)
    // Winsock reference-counts WSAStartup; close() balances it.
    WSADATA data;
    if(WSAStartup(MAKEWORD(2, 2), &data) != 0)
    {
        return std::unexpected("WSAStartup failed");
    }
#endif
    UdpSocket s;
    s.handle_ = static_cast<Handle>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if(s.handle_ == kInvalid)
    {
        const std::string error = lastError();
#if defined(_WIN32)
        WSACleanup();
#endif
        return std::unexpected("socket: " + error);
    }

#if defined(_WIN32)
    u_long nonBlocking = 1;
    ioctlsocket(s.handle_, FIONBIO, &nonBlocking);
    // Don't report ICMP port unreachable as WSAECONNRESET on later receives.
    BOOL reportReset = FALSE;
    DWORD bytes = 0;
    WSAIoctl(s.handle_, _WSAIOW(IOC_VENDOR, 12), &reportReset, sizeof(reportReset), nullptr, 0,
             &bytes, nullptr, nullptr);
#else
    ::fcntl(s.handle_, F_SETFL, ::fcntl(s.handle_, F_GETFL, 0) | O_NONBLOCK);
#endif

    // Room for bursts of object updates.
    int bufferSize = 1 << 20;
    ::setsockopt(s.handle_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufferSize),
                 sizeof(bufferSize));

    const Endpoint local{loopbackOnly ? 0x7F000001u : 0u, 0};
    const sockaddr_in address = toSockaddr(local);
    if(::bind(s.handle_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
    {
        return std::unexpected("bind: " + lastError());
    }
    return s;
}

UdpSocket::~UdpSocket()
{
    close();
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept : handle_(other.handle_)
{
    other.handle_ = kInvalid;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept
{
    if(this != &other)
    {
        close();
        handle_ = other.handle_;
        other.handle_ = kInvalid;
    }
    return *this;
}

void UdpSocket::close()
{
    if(handle_ == kInvalid)
    {
        return;
    }
#if defined(_WIN32)
    ::closesocket(handle_);
    WSACleanup();
#else
    ::close(handle_);
#endif
    handle_ = kInvalid;
}

bool UdpSocket::isOpen() const
{
    return handle_ != kInvalid;
}

Endpoint UdpSocket::localEndpoint() const
{
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    if(::getsockname(handle_, reinterpret_cast<sockaddr*>(&address), &length) != 0)
    {
        return {};
    }
    return fromSockaddr(address);
}

bool UdpSocket::connect(const Endpoint& remote)
{
    const sockaddr_in address = toSockaddr(remote);
    return ::connect(handle_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
}

bool UdpSocket::send(std::span<const uint8_t> data)
{
#if defined(_WIN32)
    const int length = static_cast<int>(data.size());
#else
    const size_t length = data.size();
#endif
    const auto sent = ::send(handle_, reinterpret_cast<const char*>(data.data()), length, 0);
    return sent >= 0 && static_cast<size_t>(sent) == data.size();
}

bool UdpSocket::sendTo(std::span<const uint8_t> data, const Endpoint& to)
{
    const sockaddr_in address = toSockaddr(to);
#if defined(_WIN32)
    const int length = static_cast<int>(data.size());
#else
    const size_t length = data.size();
#endif
    const auto sent = ::sendto(handle_, reinterpret_cast<const char*>(data.data()), length, 0,
                               reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    return sent >= 0 && static_cast<size_t>(sent) == data.size();
}

std::optional<size_t> UdpSocket::receive(std::span<uint8_t> buffer, Endpoint* from)
{
    sockaddr_in address{};
    socklen_t length = sizeof(address);
#if defined(_WIN32)
    const int capacity = static_cast<int>(buffer.size());
#else
    const size_t capacity = buffer.size();
#endif
    const auto received = ::recvfrom(handle_, reinterpret_cast<char*>(buffer.data()), capacity, 0,
                                     reinterpret_cast<sockaddr*>(&address), &length);
    if(received < 0)
    {
        return std::nullopt;
    }
    if(from)
    {
        *from = fromSockaddr(address);
    }
    return static_cast<size_t>(received);
}

bool waitReadable(std::span<const UdpSocket::Handle> sockets, std::vector<bool>& readable,
                  std::chrono::milliseconds timeout)
{
    std::vector<pollfd> fds(sockets.size());
    for(size_t i = 0; i < sockets.size(); i++)
    {
        fds[i].fd = sockets[i];
        fds[i].events = POLLIN;
    }
#if defined(_WIN32)
    const int result =
        WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), static_cast<INT>(timeout.count()));
#else
    const int result = ::poll(fds.data(), fds.size(), static_cast<int>(timeout.count()));
#endif
    readable.assign(sockets.size(), false);
    if(result < 0)
    {
        return errno == EINTR;
    }
    for(size_t i = 0; i < sockets.size(); i++)
    {
        readable[i] = (fds[i].revents & (POLLIN | POLLERR)) != 0;
    }
    return true;
}

} // namespace viewer::net
