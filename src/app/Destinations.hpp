#pragma once

// Second Life's Destination Guide (the main menu's Destinations tab): its
// categories, and a category's places, from the guide's v2 API (a page of
// 100 at a time; What's Hot Now is the busiest places). Each picture comes
// from the web, decoded on the workers and given to the interface as
// image://destination-N; those no longer shown are let go when another list
// comes.
//
// Maturity: v2 answers every place whatever it's asked for, so the places
// are kept to those the resident's maturity (g, gm or gma) allows, here.

#include "viewer/core/JobSystem.hpp"
#include "viewer/core/MessageQueue.hpp"
#include "viewer/gui/Gui.hpp"
#include "viewer/net/HttpClient.hpp"

#include <chrono>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace viewer
{

struct Destination
{
    std::string name;
    std::string description;
    std::string location;      // "Region/x/y/z", URL-encoded (session::parseLocation)
    std::string maturity;      // "G", "M" or "A"
    std::string imageUrl;      // its picture (the mid-sized one)
    std::string image;         // "image://NAME" once it's here; empty until then
    std::string largeImageUrl; // the full-sized one (bigger), asked for when wanted
    std::string largeImage;
};

struct DestinationCategory
{
    std::string id; // its simple_name, in the API's addresses
    std::string name;
    std::string description;
    int count = 0; // places in it (of every maturity)
    std::string imageUrl;
    std::string image; // "image://NAME" once it's here
};

// The busiest places' category (shown first).
inline constexpr std::string_view kBusiestCategory = "whats-hot-now";
// Places a page of v2's answers has at most.
inline constexpr size_t kDestinationPage = 100;

// A list of places (the answer's [...]).
std::expected<std::vector<Destination>, std::string> parseDestinations(std::string_view json);
// The categories: those of the guide (its secondlife_dg channel) with
// places in them, by name, the busiest first.
std::expected<std::vector<DestinationCategory>, std::string>
parseDestinationCategories(std::string_view json);

// A page (from 1) of a category's places, most people first.
std::string categoryDestinationsUrl(std::string_view category, std::string_view maturity, int page);
std::string destinationCategoriesUrl();
// The places of those ratings ("G", "M", "A") a maturity ("g", "gm", "gma")
// allows.
void keepMaturity(std::vector<Destination>& places, std::string_view maturity);

class Destinations
{
public:
    Destinations(net::HttpClient& http, JobSystem& jobs, gui::Gui& gui);
    ~Destinations();

    Destinations(const Destinations&) = delete;
    Destinations& operator=(const Destinations&) = delete;

    // Asks for the categories (again).
    void fetchCategories();
    // Asks for a category's places, its first page. Another category's places go at once; the same
    // one's stay until the new ones are here.
    void fetch(const std::string& category, const std::string& maturity);
    // Asks for the category's next page (added to places()).
    void fetchMore();
    // The answers and pictures that arrived. Once a frame.
    void update();
    // Asks for a place's full-sized picture (shown alone).
    void wantLargeImage(size_t place);

    const std::vector<DestinationCategory>& categories() const { return categories_; }
    bool categoriesLoading() const { return categoriesLoading_; }
    bool categoriesFetched() const { return categoriesFetched_; }

    const std::vector<Destination>& places() const { return places_; }
    // The category the places are of, the maturity they're for, and when
    // they were asked for (nothing: never).
    const std::string& category() const { return category_; }
    const std::string& maturity() const { return maturity_; }
    std::optional<std::chrono::steady_clock::time_point> fetchedAt() const { return fetchedAt_; }
    bool loading() const { return loading_; }
    // The category has more pages than those asked for.
    bool hasMore() const { return hasMore_; }
    // Why the last fetch failed; empty if it didn't.
    const std::string& error() const { return error_; }
    // Changes whenever categories() or places() do (a new list, a picture).
    uint64_t version() const { return version_; }

private:
    struct Answer
    {
        enum class Kind
        {
            Categories,
            Places,
        };
        Kind kind = Kind::Places;
        uint64_t generation = 0;
        net::HttpResponse response;
    };
    struct Picture
    {
        std::string url;
        std::optional<gui::Pixels> pixels;
    };
    void ask(Answer::Kind kind, std::string url);
    void placesArrived(Answer& answer);
    // An image's name once here ("image://..."), else empty (and it's
    // asked for, unless it's coming or couldn't be had).
    std::string image(const std::string& url);
    // Lets go of the pictures nothing shows now.
    void forgetImages();

    net::HttpClient& http_;
    JobSystem& jobs_;
    gui::Gui& gui_;
    std::unique_ptr<net::HttpRequestGroup> group_;
    // Shared with the workers' jobs, which may finish after we've gone.
    std::shared_ptr<MessageQueue<Answer>> answers_ = std::make_shared<MessageQueue<Answer>>();
    std::shared_ptr<MessageQueue<Picture>> pictures_ = std::make_shared<MessageQueue<Picture>>();

    std::vector<DestinationCategory> categories_;
    bool categoriesLoading_ = false;
    bool categoriesFetched_ = false;

    std::vector<Destination> places_;
    std::string category_;
    std::string maturity_;
    int page_ = 0; // the last page asked for
    bool hasMore_ = false;
    std::string error_;
    bool loading_ = false;
    uint64_t generation_ = 0; // of the places asked for
    uint64_t version_ = 0;
    std::optional<std::chrono::steady_clock::time_point> fetchedAt_;
    // Pictures by address: the image's name once here, empty while coming,
    // and the ones that couldn't be had (not asked again).
    std::map<std::string, std::string> images_;
    int nextImage_ = 0;
};

} // namespace viewer
