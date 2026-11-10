// Tests run against whichever backend createHttpClient() returns.
// The loopback server is POSIX-only for now.
#ifndef _WIN32

#include "LoopbackHttpServer.hpp"

#include "viewer/llsd/Serialization.hpp"
#include "viewer/net/HttpClient.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>

using namespace viewer::net;
using namespace std::chrono_literals;

namespace
{

// Collects responses from the I/O thread so the test thread can wait on them.
class Responses
{
public:
    HttpCallback callback()
    {
        return [this](HttpResponse&& response)
        {
            std::lock_guard lock(mutex_);
            responses_.push_back(std::move(response));
            arrived_.notify_all();
        };
    }

    bool waitFor(size_t count, std::chrono::milliseconds timeout = 5s)
    {
        std::unique_lock lock(mutex_);
        return arrived_.wait_for(lock, timeout, [&] { return responses_.size() >= count; });
    }

    size_t count()
    {
        std::lock_guard lock(mutex_);
        return responses_.size();
    }

    HttpResponse& operator[](size_t i)
    {
        std::lock_guard lock(mutex_);
        return responses_.at(i);
    }

private:
    std::mutex mutex_;
    std::condition_variable arrived_;
    std::vector<HttpResponse> responses_;
};

std::vector<uint8_t> toBytes(std::string_view s)
{
    return {s.begin(), s.end()};
}

} // namespace

TEST_CASE("HTTP GET returns status, body and lowercased headers")
{
    viewer::test::LoopbackHttpServer server;
    auto client = createHttpClient();
    Responses responses;

    const HttpRequestId id = client->send({.url = server.url("/hello")}, responses.callback());
    REQUIRE(responses.waitFor(1));
    HttpResponse& r = responses[0];
    CHECK(r.requestId == id);
    CHECK(r.error == HttpError::None);
    CHECK(r.ok());
    CHECK(r.status == 200);
    CHECK(r.bodyText() == "hello");
    CHECK(r.headers.find("x-test") == "yes");
    CHECK(r.headers.find("X-TEST") == "yes"); // lookups ignore case
}

TEST_CASE("HTTP POST and PUT send the body and headers")
{
    viewer::test::LoopbackHttpServer server;
    auto client = createHttpClient();
    Responses responses;

    HttpRequest post{.method = HttpMethod::Post, .url = server.url("/echo")};
    post.headers.add("Content-Type", "application/llsd+xml");
    post.body = toBytes("<llsd><integer>5</integer></llsd>");
    client->send(post, responses.callback());
    REQUIRE(responses.waitFor(1));
    CHECK(responses[0].bodyText() == "<llsd><integer>5</integer></llsd>");
    CHECK(responses[0].headers.find("x-method") == "POST");
    CHECK(responses[0].headers.find("x-content-type") == "application/llsd+xml");

    HttpRequest put{.method = HttpMethod::Put, .url = server.url("/echo")};
    put.body = toBytes("put body");
    client->send(put, responses.callback());
    REQUIRE(responses.waitFor(2));
    CHECK(responses[1].bodyText() == "put body");
    CHECK(responses[1].headers.find("x-method") == "PUT");

    // An empty POST is still a POST.
    client->send({.method = HttpMethod::Post, .url = server.url("/echo")}, responses.callback());
    REQUIRE(responses.waitFor(3));
    CHECK(responses[2].headers.find("x-method") == "POST");
    CHECK(responses[2].body.empty());
}

TEST_CASE("HTTP error statuses and redirects")
{
    viewer::test::LoopbackHttpServer server;
    auto client = createHttpClient();
    Responses responses;

    client->send({.url = server.url("/status/404")}, responses.callback());
    REQUIRE(responses.waitFor(1));
    CHECK(responses[0].error == HttpError::None);
    CHECK(responses[0].status == 404);
    CHECK_FALSE(responses[0].ok());

    client->send({.url = server.url("/redirect")}, responses.callback());
    REQUIRE(responses.waitFor(2));
    CHECK(responses[1].status == 200);
    CHECK(responses[1].bodyText() == "hello");
    CHECK(responses[1].headers.find("x-test") == "yes"); // final response's headers only
    CHECK_FALSE(responses[1].headers.find("location").has_value());

    client->send({.url = server.url("/redirect"), .followRedirects = false}, responses.callback());
    REQUIRE(responses.waitFor(3));
    CHECK(responses[2].status == 302);
}

TEST_CASE("HTTP transport errors are reported, not thrown")
{
    viewer::test::LoopbackHttpServer server;
    auto client = createHttpClient();
    Responses responses;

    client->send({.url = server.url("/slow"), .timeout = 200ms}, responses.callback());
    client->send({.url = "http://127.0.0.1:1/"}, responses.callback()); // nothing listens on port 1
    client->send({.url = "notascheme://x"}, responses.callback());
    REQUIRE(responses.waitFor(3));

    std::vector<HttpError> errors;
    for(size_t i = 0; i < 3; i++)
    {
        errors.push_back(responses[i].error);
        CHECK(responses[i].status == 0);
        CHECK_FALSE(responses[i].errorMessage.empty());
    }
    CHECK(std::ranges::count(errors, HttpError::Timeout) == 1);
    CHECK(std::ranges::count(errors, HttpError::CouldNotConnect) == 1);
    CHECK(std::ranges::count(errors, HttpError::InvalidRequest) == 1);
}

TEST_CASE("HTTP cancel stops the callback from running")
{
    viewer::test::LoopbackHttpServer server;
    auto client = createHttpClient();
    Responses responses;

    const HttpRequestId slow = client->send({.url = server.url("/slow")}, responses.callback());
    std::this_thread::sleep_for(50ms); // let it reach the server
    CHECK(client->cancel(slow));
    CHECK_FALSE(client->cancel(slow)); // already cancelled

    client->send({.url = server.url("/hello")}, responses.callback());
    REQUIRE(responses.waitFor(1));
    std::this_thread::sleep_for(100ms);
    CHECK(responses.count() == 1);
    CHECK(responses[0].bodyText() == "hello");
}

TEST_CASE("HTTP many concurrent requests all complete")
{
    viewer::test::LoopbackHttpServer server;
    auto client = createHttpClient({.maxConnectionsPerHost = 4});
    Responses responses;

    constexpr size_t kCount = 100;
    for(size_t i = 0; i < kCount; i++)
    {
        HttpRequest request{.method = HttpMethod::Post, .url = server.url("/echo")};
        request.body = toBytes(std::to_string(i));
        client->send(std::move(request), responses.callback());
    }
    REQUIRE(responses.waitFor(kCount, 10s));
    for(size_t i = 0; i < kCount; i++)
    {
        CHECK(responses[i].status == 200);
    }
}

TEST_CASE("HttpRequestGroup cancels its pending requests when destroyed")
{
    viewer::test::LoopbackHttpServer server;
    auto client = createHttpClient();
    Responses responses;
    {
        HttpRequestGroup group(*client);
        group.send({.url = server.url("/hello")}, responses.callback());
        REQUIRE(responses.waitFor(1));
        CHECK(group.pendingCount() == 0); // finished requests are forgotten

        for(int i = 0; i < 5; i++)
        {
            group.send({.url = server.url("/slow")}, responses.callback());
        }
        CHECK(group.pendingCount() == 5);
    } // destroyed with five requests in flight
    std::this_thread::sleep_for(100ms);
    CHECK(responses.count() == 1);
}

TEST_CASE("Destroying the client with requests in flight is safe")
{
    viewer::test::LoopbackHttpServer server;
    Responses responses;
    {
        auto client = createHttpClient();
        for(int i = 0; i < 5; i++)
        {
            client->send({.url = server.url("/slow")}, responses.callback());
        }
        std::this_thread::sleep_for(50ms);
    }
    CHECK(responses.count() == 0);
}

TEST_CASE("HTTP + LLSD: POST an LLSD document and parse the reply")
{
    viewer::test::LoopbackHttpServer server;
    auto client = createHttpClient();
    Responses responses;

    viewer::llsd::Value body;
    body["ack"] = 7;
    body["done"] = false;
    HttpRequest request{.method = HttpMethod::Post, .url = server.url("/echo")};
    request.headers.add("Content-Type", std::string(viewer::llsd::kXmlContentType));
    const std::string xml = viewer::llsd::toXml(body);
    request.body.assign(xml.begin(), xml.end());

    client->send(std::move(request), responses.callback());
    REQUIRE(responses.waitFor(1));
    const auto reply = viewer::llsd::parse(responses[0].bodyText());
    REQUIRE(reply.has_value());
    CHECK(reply->operator[]("ack").asInteger() == 7);
    CHECK(*reply == body);
}

#endif // _WIN32
