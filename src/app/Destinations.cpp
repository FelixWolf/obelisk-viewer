#include "Destinations.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/llsd/Serialization.hpp"

#include <algorithm>
#include <format>
#include <iterator>
#include <set>

namespace viewer
{

namespace
{

constexpr std::string_view kGuide = "https://worldaping.agni.lindenlab.com/v2/destinations/";
const std::string kFailed = "\x01"; // in images_: the picture couldn't be had

// The places of an answer's list.
std::vector<Destination> readPlaces(const llsd::Value& list)
{
    std::vector<Destination> places;
    for(size_t i = 0; i < list.size(); i++)
    {
        const llsd::Value& d = list.at(i);
        Destination place;
        place.name = d["name"].asString();
        place.description = d["description"].asString();
        place.location = d["location"].asString();
        place.maturity = d["maturity"].asString();
        const llsd::Value& assets = d["assets"];
        for(size_t a = 0; a < assets.size(); a++)
        {
            const std::string type = assets.at(a)["type"].asString();
            // The mid-sized picture (250 by 150), else whatever there is;
            // the full-sized one (about 660 by 400) for the place alone.
            if(type == "midsize" || place.imageUrl.empty())
            {
                place.imageUrl = assets.at(a)["data"].asString();
            }
            if(type == "fullsize")
            {
                place.largeImageUrl = assets.at(a)["data"].asString();
            }
        }
        if(place.largeImageUrl.empty())
        {
            place.largeImageUrl = place.imageUrl;
        }
        if(!place.name.empty() && !place.location.empty())
        {
            places.push_back(std::move(place));
        }
    }
    return places;
}

} // namespace

std::string categoryDestinationsUrl(std::string_view category, std::string_view maturity, int page)
{
    return std::format(
        "{}{}/?format=json&sort_by=population&sort_dir=desc&flatten&maturity={}&page={}", kGuide,
        category, maturity, page);
}

std::string destinationCategoriesUrl()
{
    // Every one in one answer (100 a page otherwise).
    return std::format("{}categories/?format=json&limit=500", kGuide);
}

void keepMaturity(std::vector<Destination>& places, std::string_view maturity)
{
    std::erase_if(places,
                  [&](const Destination& place)
                  {
                      const char rating = place.maturity.empty() ? 'g' : place.maturity[0];
                      return maturity.find(static_cast<char>(rating | 0x20)) ==
                             std::string_view::npos;
                  });
}

std::expected<std::vector<Destination>, std::string> parseDestinations(std::string_view json)
{
    auto root = llsd::fromJson(json);
    if(!root)
    {
        return std::unexpected(root.error());
    }
    if(!root->isArray())
    {
        return std::unexpected("no destinations in the answer");
    }
    return readPlaces(*root);
}

std::expected<std::vector<DestinationCategory>, std::string>
parseDestinationCategories(std::string_view json)
{
    auto root = llsd::fromJson(json);
    if(!root)
    {
        return std::unexpected(root.error());
    }
    if(!root->isArray())
    {
        return std::unexpected("no categories in the answer");
    }
    std::vector<DestinationCategory> categories;
    for(size_t i = 0; i < root->size(); i++)
    {
        const llsd::Value& c = root->at(i);
        bool guide = false;
        const llsd::Value& channels = c["channels"];
        for(size_t k = 0; k < channels.size(); k++)
        {
            guide = guide || channels.at(k).asString() == "secondlife_dg";
        }
        DestinationCategory category;
        category.id = c["simple_name"].asString();
        category.name = c["name"].asString();
        category.description = c["description"].asString();
        category.count = c["count"].asInteger();
        category.imageUrl = c["image"].asString();
        if(guide && category.count > 0 && !category.id.empty() && !category.name.empty())
        {
            categories.push_back(std::move(category));
        }
    }
    std::ranges::sort(categories,
                      [](const DestinationCategory& a, const DestinationCategory& b)
                      {
                          const bool aBusiest = a.id == kBusiestCategory;
                          const bool bBusiest = b.id == kBusiestCategory;
                          return aBusiest != bBusiest ? aBusiest : a.name < b.name;
                      });
    return categories;
}

Destinations::Destinations(net::HttpClient& http, JobSystem& jobs, gui::Gui& gui)
    : http_(http), jobs_(jobs), gui_(gui), group_(std::make_unique<net::HttpRequestGroup>(http))
{
}

Destinations::~Destinations()
{
    group_.reset(); // no answers after this
    for(const auto& [url, name] : images_)
    {
        if(!name.empty() && name != kFailed)
        {
            gui_.removeImage(name);
        }
    }
}

void Destinations::ask(Answer::Kind kind, std::string url)
{
    net::HttpRequest request;
    request.url = std::move(url);
    request.headers.add("Accept", "application/json");
    auto answers = answers_;
    const uint64_t generation = generation_;
    group_->send(std::move(request), [answers, kind, generation](net::HttpResponse&& response)
                 { answers->push({kind, generation, std::move(response)}); });
}

void Destinations::fetchCategories()
{
    if(categoriesLoading_)
    {
        return;
    }
    categoriesLoading_ = true;
    ask(Answer::Kind::Categories, destinationCategoriesUrl());
}

void Destinations::fetch(const std::string& category, const std::string& maturity)
{
    ++generation_; // what was coming for the last isn't wanted
    if(category != category_)
    {
        places_.clear();
        version_++;
    }
    category_ = category;
    maturity_ = maturity;
    page_ = 1;
    hasMore_ = false;
    loading_ = true;
    error_.clear();
    fetchedAt_ = std::chrono::steady_clock::now();
    ask(Answer::Kind::Places, categoryDestinationsUrl(category, maturity, page_));
}

void Destinations::fetchMore()
{
    if(loading_ || !hasMore_)
    {
        return;
    }
    page_++;
    loading_ = true;
    ask(Answer::Kind::Places, categoryDestinationsUrl(category_, maturity_, page_));
}

std::string Destinations::image(const std::string& url)
{
    if(url.empty())
    {
        return {};
    }
    const auto known = images_.find(url);
    if(known != images_.end())
    {
        return known->second.empty() || known->second == kFailed ? std::string()
                                                                 : "image://" + known->second;
    }
    images_[url] = "";
    net::HttpRequest request;
    request.url = url;
    auto pictures = pictures_;
    JobSystem* jobs = &jobs_;
    // The workers outlive us (the app's), and the queue is shared.
    group_->send(std::move(request),
                 [pictures, jobs, url](net::HttpResponse&& response)
                 {
                     if(!response.ok())
                     {
                         pictures->push({url, std::nullopt});
                         return;
                     }
                     jobs->submit([pictures, url, body = std::move(response.body)]
                                  { pictures->push({url, gui::decodeImage(body)}); });
                 });
    return {};
}

void Destinations::forgetImages()
{
    std::set<std::string_view> shown;
    for(const DestinationCategory& c : categories_)
    {
        shown.insert(c.imageUrl);
    }
    for(const Destination& p : places_)
    {
        shown.insert(p.imageUrl);
        shown.insert(p.largeImageUrl);
    }
    std::erase_if(images_,
                  [&](const auto& entry)
                  {
                      const auto& [url, name] = entry;
                      if(name.empty() || name == kFailed || shown.contains(url))
                      {
                          return false; // coming, not to be had, or shown
                      }
                      gui_.removeImage(name);
                      return true;
                  });
}

void Destinations::wantLargeImage(size_t place)
{
    if(place < places_.size() && places_[place].largeImage.empty())
    {
        places_[place].largeImage = image(places_[place].largeImageUrl);
        if(!places_[place].largeImage.empty())
        {
            version_++;
        }
    }
}

void Destinations::placesArrived(Answer& a)
{
    if(!a.response.ok())
    {
        loading_ = false;
        error_ = a.response.error != net::HttpError::None
                     ? std::format("Couldn't reach the Destination Guide ({}).",
                                   net::errorName(a.response.error))
                     : std::format("The Destination Guide answered {}.", a.response.status);
        log::warn("destinations: {}", error_);
        return;
    }
    auto places = parseDestinations(a.response.bodyText());
    if(!places)
    {
        loading_ = false;
        error_ = "The Destination Guide's answer couldn't be read.";
        log::warn("destinations: {}", places.error());
        return;
    }
    loading_ = false;
    hasMore_ = places->size() >= kDestinationPage;
    keepMaturity(*places, maturity_);
    for(Destination& place : *places)
    {
        place.image = image(place.imageUrl);
        place.largeImage = images_.contains(place.largeImageUrl)
                               ? image(place.largeImageUrl)
                               : std::string(); // asked for when wanted
    }
    if(page_ <= 1)
    {
        places_ = std::move(*places);
        forgetImages();
    }
    else
    {
        std::ranges::move(*places, std::back_inserter(places_));
    }
    version_++;
}

void Destinations::update()
{
    std::vector<Answer> answers;
    answers_->drain(answers);
    for(Answer& a : answers)
    {
        if(a.kind == Answer::Kind::Categories)
        {
            categoriesLoading_ = false;
            categoriesFetched_ = true;
            if(!a.response.ok())
            {
                log::warn("destination categories: answered {} ({})", a.response.status,
                          net::errorName(a.response.error));
                continue;
            }
            auto categories = parseDestinationCategories(a.response.bodyText());
            if(!categories)
            {
                log::warn("destination categories: {}", categories.error());
                continue;
            }
            categories_ = std::move(*categories);
            for(DestinationCategory& c : categories_)
            {
                c.image = image(c.imageUrl);
            }
            version_++;
        }
        else if(a.generation == generation_)
        {
            placesArrived(a);
        }
    }

    std::vector<Picture> pictures;
    pictures_->drain(pictures);
    for(Picture& p : pictures)
    {
        const auto entry = images_.find(p.url);
        if(entry == images_.end() || !entry->second.empty())
        {
            continue; // not asked for now
        }
        if(!p.pixels)
        {
            entry->second = kFailed;
            continue;
        }
        entry->second = std::format("destination-{}", nextImage_++);
        gui_.setImage(entry->second, std::move(*p.pixels));
        const std::string name = "image://" + entry->second;
        for(Destination& place : places_)
        {
            if(place.imageUrl == p.url)
            {
                place.image = name;
            }
            if(place.largeImageUrl == p.url)
            {
                place.largeImage = name;
            }
        }
        for(DestinationCategory& c : categories_)
        {
            if(c.imageUrl == p.url)
            {
                c.image = name;
            }
        }
        version_++;
    }
}

} // namespace viewer
