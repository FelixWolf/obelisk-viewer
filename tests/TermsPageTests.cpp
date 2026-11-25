#include "TermsPage.hpp"

#include "viewer/session/Login.hpp"

#include <doctest/doctest.h>

using namespace viewer;

TEST_CASE("Terms page: text, headings, lists and https links only")
{
    const auto blocks = parseTerms(
        "<head><title>No</title><style>p { x }</style></head><body><!-- <p>no</p> -->"
        "<h3><p>1.  TITLE &amp; more</p></h3>"
        "<p>Read <a title=\"a > b\" href=\"https://example.com/?a=1&amp;b=2\">the\n terms</a> "
        "<b>now</b>&nbsp;{{ x }} 1 < 2.</p><p><a href=\"#top\">Return to top</a></p>"
        "<ol><li>One</li><li><p>Two</p></li></ol>"
        "<p><a href='javascript:x'>bad</a> &#8220;q&#x201d;</p></body>");
    REQUIRE(blocks.size() == 5);
    CHECK(blocks[0].heading);
    CHECK(blocks[0].spans[0].text == "1. TITLE & more");
    REQUIRE(blocks[1].spans.size() == 4);
    CHECK(blocks[1].spans[0].text == "Read ");
    CHECK(blocks[1].spans[1].text == "the terms ");
    CHECK(blocks[1].spans[1].link == "https://example.com/?a=1&b=2");
    CHECK(blocks[1].spans[2].bold);
    CHECK(blocks[1].spans[3].text == "{{ x }} 1 < 2.");
    CHECK(blocks[2].item);
    CHECK(blocks[2].spans[0].text == "1. One");
    CHECK(blocks[3].spans[0].text == "2. Two");
    CHECK(blocks[4].spans[0].link.empty());
    CHECK(blocks[4].spans[0].text == "bad \xE2\x80\x9Cq\xE2\x80\x9D");
    CHECK_FALSE(plausibleTerms(blocks));
}

TEST_CASE("Terms page: broken pages end, and which grids' it is")
{
    for(const char* html : {"", "<", "<p", "<!--", "<a href=\"", "<style>x", "&#xFFFFFFFFF;", "</"})
    {
        CHECK_FALSE(plausibleTerms(parseTerms(html)));
    }
    CHECK(plausibleTerms(parseTerms("<p>" + std::string(3000, 'x') + "</p>")));
    CHECK(usesLindenTerms(session::kAgniLoginUri));
    CHECK(usesLindenTerms(session::kAditiLoginUri));
    CHECK_FALSE(usesLindenTerms("https://grid.example/login"));
}
