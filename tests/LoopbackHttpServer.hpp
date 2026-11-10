#pragma once

// Tiny HTTP/1.1 server on 127.0.0.1 for testing HttpClient backends without
// touching the network. POSIX sockets only; one thread per connection;
// every response closes the connection.
//
// Routes:
//   GET  /hello          200 "hello", header X-Test: yes
//   *    /echo           200, body = request body, X-Method / X-Content-Type headers
//   *    /status/<code>  that status, empty body
//   GET  /redirect       302 -> /hello
//   *    /slow           waits up to 3 s (until the server stops), then 200 "slow"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace viewer::test
{

class LoopbackHttpServer
{
public:
    LoopbackHttpServer()
    {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int yes = 1;
        ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0; // any free port
        ::bind(listenFd_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        ::listen(listenFd_, 64);
        socklen_t length = sizeof(address);
        ::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        acceptThread_ = std::thread([this] { acceptLoop(); });
    }

    ~LoopbackHttpServer()
    {
        stopping_ = true;
        ::shutdown(listenFd_, SHUT_RDWR);
        ::close(listenFd_);
        acceptThread_.join();
        std::lock_guard lock(mutex_);
        for(auto& t : connections_)
        {
            t.join();
        }
    }

    std::string url(std::string_view path) const
    {
        return std::format("http://127.0.0.1:{}{}", port_, path);
    }

    int requestCount() const { return requests_; }

private:
    void acceptLoop()
    {
        while(!stopping_)
        {
            const int fd = ::accept(listenFd_, nullptr, nullptr);
            if(fd < 0)
            {
                return;
            }
            std::lock_guard lock(mutex_);
            connections_.emplace_back([this, fd] { serve(fd); });
        }
    }

    static std::string header(const std::string& head, std::string_view name)
    {
        // Case-insensitive search for "\r\nName:".
        std::string lowerHead = head;
        std::string lowerName(name);
        for(auto& c : lowerHead)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        for(auto& c : lowerName)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        const size_t at = lowerHead.find("\r\n" + lowerName + ":");
        if(at == std::string::npos)
        {
            return {};
        }
        size_t start = at + 2 + name.size() + 1;
        while(start < head.size() && head[start] == ' ')
        {
            start++;
        }
        return head.substr(start, head.find("\r\n", start) - start);
    }

    void serve(int fd)
    {
        std::string data;
        char buffer[4096];
        size_t headerEnd = std::string::npos;
        while(headerEnd == std::string::npos)
        {
            const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
            if(n <= 0)
            {
                ::close(fd);
                return;
            }
            data.append(buffer, static_cast<size_t>(n));
            headerEnd = data.find("\r\n\r\n");
        }
        const std::string head = data.substr(0, headerEnd);
        const size_t contentLength = std::stoul("0" + header(head, "Content-Length"));
        std::string body = data.substr(headerEnd + 4);
        while(body.size() < contentLength)
        {
            const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
            if(n <= 0)
            {
                break;
            }
            body.append(buffer, static_cast<size_t>(n));
        }
        requests_++;

        const std::string method = head.substr(0, head.find(' '));
        const size_t pathStart = head.find(' ') + 1;
        const std::string path = head.substr(pathStart, head.find(' ', pathStart) - pathStart);

        int status = 200;
        std::string extraHeaders;
        std::string responseBody;
        if(path == "/hello")
        {
            responseBody = "hello";
            extraHeaders = "X-Test: yes\r\n";
        }
        else if(path == "/echo")
        {
            responseBody = body;
            extraHeaders = std::format("X-Method: {}\r\nX-Content-Type: {}\r\n", method,
                                       header(head, "Content-Type"));
        }
        else if(path.starts_with("/status/"))
        {
            status = std::stoi(path.substr(8));
        }
        else if(path == "/redirect")
        {
            status = 302;
            extraHeaders = "Location: /hello\r\n";
        }
        else if(path == "/slow")
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while(!stopping_ && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            responseBody = "slow";
        }
        else
        {
            status = 404;
        }

        const std::string response =
            std::format("HTTP/1.1 {} Test\r\nContent-Length: {}\r\nConnection: close\r\n{}\r\n{}",
                        status, responseBody.size(), extraHeaders, responseBody);
        ::send(fd, response.data(), response.size(), MSG_NOSIGNAL);
        ::close(fd);
    }

    int listenFd_ = -1;
    uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    std::atomic<int> requests_{0};
    std::thread acceptThread_;
    std::mutex mutex_;
    std::vector<std::thread> connections_;
};

} // namespace viewer::test
