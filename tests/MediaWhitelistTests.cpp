#include "MediaWhitelist.hpp"

#include <doctest/doctest.h>

using namespace viewer;

namespace
{

constexpr const char* kList = R"({
  "schema": 1,
  "sites": [
    {"host": "YouTube.com", "subdomains": true, "path": "/"},
    {"host": "example.org", "subdomains": false, "path": "/media/"},
    {"host": "com", "subdomains": true, "path": "/"},
    {"host": "*.evil.net", "subdomains": true, "path": "/"},
    {"host": "nopath.net", "subdomains": true, "path": null}
  ],
  "refresh": 600
})";

} // namespace

TEST_CASE("media whitelist: what's read")
{
    const auto list = parseMediaWhitelist(kList);
    REQUIRE(list);
    CHECK(list->sites.size() == 2); // the senseless left out
    CHECK(list->refresh == 600);
    CHECK_FALSE(parseMediaWhitelist(R"({"schema": 2, "sites": []})"));
    CHECK_FALSE(parseMediaWhitelist(R"({"schema": 1})"));
    CHECK_FALSE(parseMediaWhitelist("<html>"));
    CHECK_FALSE(MediaWhitelist{}.allows("https://youtube.com/"));
}

TEST_CASE("media whitelist: hosts")
{
    const auto list = parseMediaWhitelist(kList);
    REQUIRE(list);
    for(const char* url :
        {"https://youtube.com", "https://youtube.com/watch?v=1", "HTTPS://WWW.YouTube.com/",
         "https://www.youtube.com:443/", "https://youtube.com./", "https://youtube.com?x=a@b",
         "https://youtube.com/%E2%9C%93"})
    {
        CAPTURE(url);
        CHECK(list->allows(url));
    }
    for(const char* url : {"",
                           "https://",
                           "youtube.com",
                           "http://youtube.com/",
                           "file:///youtube.com",
                           "javascript:alert(1)",
                           "https://youtube.com.evil.net/",
                           "https://notyoutube.com/",
                           "https://evil.net/youtube.com",
                           "https://youtube.com@evil.net/",
                           "https://me:pw@youtube.com/",
                           "https://evil.net\\@youtube.com/",
                           "https://evil.net\\.youtube.com/",
                           "https://youtube.com:8443/",
                           "https://youtube%2ecom/",
                           "https://you\ttube.com/",
                           " https://youtube.com/",
                           "https:/youtube.com/",
                           "https:///youtube.com/",
                           "https://evil.net#.youtube.com/",
                           "https://evil.net?.youtube.com/",
                           "https://.youtube.com/",
                           "https://x..youtube.com/",
                           "https://other.com/",
                           "https://[::1]/",
                           "https://www.example.org/media/a",
                           "https://youtube.c\xC3\xB6m/"})
    {
        CAPTURE(url);
        CHECK_FALSE(list->allows(url));
    }
}

TEST_CASE("media whitelist: paths")
{
    const auto list = parseMediaWhitelist(kList);
    REQUIRE(list);
    for(const char* url : {"https://example.org/media", "https://example.org/media/",
                           "https://example.org/media/a/b.webm?x=..#y"})
    {
        CAPTURE(url);
        CHECK(list->allows(url));
    }
    for(const char* url : {"https://example.org/", "https://example.org/mediaplayer",
                           "https://example.org/Media/", "https://example.org/media/../x",
                           "https://example.org/media/%2e%2e/x", "https://example.org/media/.."})
    {
        CAPTURE(url);
        CHECK_FALSE(list->allows(url));
    }
}

TEST_CASE("media whitelist: how media starts")
{
    const auto list = parseMediaWhitelist(kList);
    REQUIRE(list);
    const char* listed = "https://youtube.com/";
    const char* other = "https://elsewhere.net/";
    using enum MediaStart;
    CHECK(mediaStart(MediaPlay::Always, true, other, *list) == Play);
    CHECK(mediaStart(MediaPlay::Always, false, listed, *list) == Click);
    CHECK(mediaStart(MediaPlay::Whitelist, true, listed, *list) == Play);
    CHECK(mediaStart(MediaPlay::Whitelist, false, listed, *list) == Click);
    CHECK(mediaStart(MediaPlay::Whitelist, true, other, *list) == Click);
    CHECK(mediaStart(MediaPlay::Click, true, listed, *list) == Click);
    CHECK(mediaStart(MediaPlay::Disable, true, listed, *list) == Never);

    CHECK(mediaPlayFrom("always") == MediaPlay::Always);
    CHECK(mediaPlayFrom("whitelist") == MediaPlay::Whitelist);
    CHECK(mediaPlayFrom("click") == MediaPlay::Click);
    CHECK(mediaPlayFrom("disable") == MediaPlay::Disable);
    CHECK(mediaPlayFrom("") == MediaPlay::Whitelist);
}
