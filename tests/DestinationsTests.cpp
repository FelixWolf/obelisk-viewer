#include "Destinations.hpp"

#include <doctest/doctest.h>

using namespace viewer;

TEST_CASE("Destinations: a category's places, as the guide's API gives them")
{
    CHECK(categoryDestinationsUrl("art", "gma", 2) ==
          "https://worldaping.agni.lindenlab.com/v2/destinations/art/"
          "?format=json&sort_by=population&sort_dir=desc&flatten&maturity=gma&page=2");

    const auto places = parseDestinations(R"([
        {"name": "Peak Lounge", "description": "Music and dancing.",
         "location": "PEAK%20Lounge/42/120/3001", "maturity": "M",
         "assets": [{"type": "fullsize", "data": "https://example.com/full.jpg"},
                    {"type": "midsize", "data": "https://example.com/mid.jpg"},
                    {"type": "thumbnail", "data": "https://example.com/thumb.jpg"}],
         "population": {"current": 0}},
        {"name": "Small", "location": "Somewhere/1/2/3", "maturity": "G",
         "assets": [{"type": "thumbnail", "data": "https://example.com/t.jpg"}]},
        {"name": "Nowhere"}
    ])");
    REQUIRE(places);
    REQUIRE(places->size() == 2); // without a location, it can't be gone to
    const Destination& peak = places->front();
    CHECK(peak.name == "Peak Lounge");
    CHECK(peak.description == "Music and dancing.");
    CHECK(peak.location == "PEAK%20Lounge/42/120/3001");
    CHECK(peak.region == "PEAK Lounge");
    CHECK(peak.maturity == "M");
    CHECK(peak.imageUrl == "https://example.com/mid.jpg");
    CHECK(peak.largeImageUrl == "https://example.com/full.jpg");
    CHECK(peak.image.empty());
    CHECK(places->back().imageUrl == "https://example.com/t.jpg");
    CHECK(places->back().largeImageUrl == "https://example.com/t.jpg"); // no full-sized one

    // The guide answers every maturity: kept to the resident's here.
    std::vector<Destination> general = *places;
    keepMaturity(general, "g");
    REQUIRE(general.size() == 1);
    CHECK(general.front().name == "Small");
    std::vector<Destination> all = *places;
    keepMaturity(all, "gma");
    CHECK(all.size() == 2);

    CHECK_FALSE(parseDestinations("not json"));
    CHECK_FALSE(parseDestinations(R"({"destinations": []})"));
}

TEST_CASE("Destinations: the guide's categories")
{
    CHECK(destinationCategoriesUrl() ==
          "https://worldaping.agni.lindenlab.com/v2/destinations/categories/"
          "?format=json&limit=500");

    const auto categories = parseDestinationCategories(R"([
        {"id": 3, "name": "Music", "simple_name": "music", "description": "Concerts.",
         "image": "https://example.com/music.jpg", "count": 713, "channels": ["secondlife_dg"]},
        {"id": 1, "name": "Art", "simple_name": "art", "description": "Galleries.",
         "image": "https://example.com/art.jpg", "count": 364,
         "channels": ["login_page", "secondlife_dg"]},
        {"id": 2, "name": "What's Hot Now", "simple_name": "whats-hot-now", "count": 24,
         "channels": ["secondlife_dg"]},
        {"id": 9, "name": "Empty", "simple_name": "empty", "count": 0, "channels": ["secondlife_dg"]},
        {"id": 7, "name": "Portal", "simple_name": "portal_new", "count": 5, "channels": ["portals"]},
        {"id": 8, "name": "Nowhere", "simple_name": "nowhere", "count": 5, "channels": null}
    ])");
    REQUIRE(categories);
    // The guide's, with places in them: the busiest first, then by name.
    REQUIRE(categories->size() == 3);
    CHECK(categories->at(0).id == "whats-hot-now");
    CHECK(categories->at(1).id == "art");
    CHECK(categories->at(1).name == "Art");
    CHECK(categories->at(1).description == "Galleries.");
    CHECK(categories->at(1).count == 364);
    CHECK(categories->at(1).imageUrl == "https://example.com/art.jpg");
    CHECK(categories->at(2).id == "music");
    CHECK_FALSE(parseDestinationCategories(R"({"categories": []})"));
}
