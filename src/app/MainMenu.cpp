#include "MainMenu.hpp"

#include "viewer/core/Log.hpp"

#include "EmbeddedAssets.hpp"

#include <RmlUi/Core.h>

#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_version.h>

#include <chrono>
#include <exception>
#include <format>

namespace viewer
{

namespace
{

// The time now in SLT (Pacific: Second Life's time) and here, and the
// hour here (for the greeting).
struct Clocks
{
    std::string slt;
    std::string sltDate;
    std::string local;
    std::string localDate;
    int localHour = 12;
};

Clocks clocks()
{
    using namespace std::chrono;
    const auto now = floor<seconds>(system_clock::now());
    Clocks c;
    try
    {
        const zoned_time slt("America/Los_Angeles", now);
        c.slt = std::format("{:%H:%M}", slt);
        c.sltDate = std::format("{:%a %e %b}", slt);
    }
    catch(const std::exception&)
    {
        // No time zone database: Pacific Standard Time, no daylight saving.
        const auto slt = now - hours(8);
        c.slt = std::format("{:%H:%M}", slt);
        c.sltDate = std::format("{:%a %e %b}", slt);
    }
    try
    {
        const zoned_time local(current_zone(), now);
        c.local = std::format("{:%H:%M}", local);
        c.localDate = std::format("{:%a %e %b}", local);
        c.localHour = static_cast<int>(
            hh_mm_ss(local.get_local_time() - floor<days>(local.get_local_time())).hours().count());
    }
    catch(const std::exception&)
    {
        c.local = std::format("{:%H:%M}", now);
        c.localDate = std::format("{:%a %e %b}", now);
        c.localHour = static_cast<int>(hh_mm_ss(now - floor<days>(now)).hours().count());
    }
    return c;
}

} // namespace

std::unique_ptr<MainMenu> MainMenu::create(gui::Gui& gui, Destinations& destinations,
                                           GridPopulation& population)
{
    std::unique_ptr<MainMenu> menu(new MainMenu);
    menu->destinations_ = &destinations;
    menu->population_ = &population;
    menu->surface_ = gui.createSurface("main menu", kSize);
    if(!menu->surface_)
    {
        return nullptr;
    }
    menu->tabs_ = {{"home", "Home"},           {"destinations", "Destinations"},
                   {"inventory", "Inventory"}, {"outfits", "Outfits"},
                   {"parcel", "Parcel"},       {"media", "Media"},
                   {"options", "Options"},     {"about", "About"}};
    menu->readLicenses();
    menu->tabName_ = "Home";

    // The page's data (data-model="menu" in the document), before it's
    // loaded.
    Rml::DataModelConstructor model = menu->surface_->context()->CreateDataModel("menu");
    if(!model)
    {
        log::error("the main menu's data model couldn't be made");
        return nullptr;
    }
    if(auto tab = model.RegisterStruct<Tab>())
    {
        tab.RegisterMember("id", &Tab::id);
        tab.RegisterMember("name", &Tab::name);
    }
    model.RegisterArray<std::vector<Tab>>();
    MainMenu* m = menu.get();
    model.Bind("tabs", &m->tabs_);
    model.Bind("tab", &m->tab_);
    model.Bind("tabName", &m->tabName_);
    model.Bind("viewerFps", &m->viewerFps_);
    model.Bind("regionFps", &m->regionFps_);
    model.Bind("dilation", &m->dilation_);
    model.Bind("viewerFpsText", &m->viewerFpsText_);
    model.Bind("regionFpsText", &m->regionFpsText_);
    model.Bind("dilationText", &m->dilationText_);
    model.Bind("slt", &m->slt_);
    model.Bind("sltDate", &m->sltDate_);
    model.Bind("local", &m->local_);
    model.Bind("localDate", &m->localDate_);
    model.Bind("region", &m->region_);
    model.Bind("position", &m->position_);
    model.Bind("greeting", &m->greeting_);
    if(auto place = model.RegisterStruct<Place>())
    {
        place.RegisterMember("name", &Place::name);
        place.RegisterMember("description", &Place::description);
        place.RegisterMember("maturity", &Place::maturity);
        place.RegisterMember("rating", &Place::rating);
        place.RegisterMember("image", &Place::image);
        place.RegisterMember("largeImage", &Place::largeImage);
        place.RegisterMember("people", &Place::people);
    }
    model.RegisterArray<std::vector<Place>>();
    if(auto category = model.RegisterStruct<Category>())
    {
        category.RegisterMember("id", &Category::id);
        category.RegisterMember("name", &Category::name);
        category.RegisterMember("places", &Category::places);
        category.RegisterMember("image", &Category::image);
    }
    model.RegisterArray<std::vector<Category>>();
    model.Bind("categories", &m->categories_);
    model.Bind("categoriesNote", &m->categoriesNote_);
    model.Bind("category", &m->category_);
    model.Bind("categoryName", &m->categoryName_);
    model.Bind("categoryText", &m->categoryText_);
    model.Bind("hasMore", &m->hasMore_);
    model.Bind("loadingMore", &m->loadingMore_);
    model.Bind("places", &m->places_);
    model.Bind("placesNote", &m->placesNote_);
    model.Bind("chosen", &m->chosen_);
    model.Bind("pick", &m->pick_);
    model.Bind("inWorld", &m->inWorld_);
    model.BindEventCallback("teleport",
                            [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                if(m->inWorld_ && m->chosen_ >= 0 &&
                                   m->chosen_ < static_cast<int>(m->placeLocations_.size()))
                                {
                                    m->teleport_ =
                                        m->placeLocations_[static_cast<size_t>(m->chosen_)];
                                }
                            });
    model.BindEventCallback("refresh",
                            [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                Destinations& d = *m->destinations_;
                                if(m->category_.empty())
                                {
                                    d.fetchCategories();
                                }
                                else
                                {
                                    d.fetch(m->category_, d.maturity());
                                }
                            });
    model.BindEventCallback("more", [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            { m->destinations_->fetchMore(); });
    if(auto rule = model.RegisterStruct<Rule>())
    {
        rule.RegisterMember("name", &Rule::name);
        rule.RegisterMember("state", &Rule::state);
        rule.RegisterMember("text", &Rule::text);
    }
    model.RegisterArray<std::vector<Rule>>();
    model.Bind("parcelKnown", &m->parcelKnown_);
    model.Bind("parcelName", &m->parcelName_);
    model.Bind("parcelWhere", &m->parcelWhere_);
    model.Bind("parcelText", &m->parcelText_);
    model.Bind("parcelImage", &m->parcelImage_);
    model.Bind("parcelOwner", &m->parcelOwner_);
    model.Bind("parcelGroup", &m->parcelGroup_);
    model.Bind("parcelArea", &m->parcelArea_);
    model.Bind("parcelPrims", &m->parcelPrims_);
    model.Bind("parcelSale", &m->parcelSale_);
    model.Bind("parcelMusic", &m->parcelMusic_);
    model.Bind("parcelRules", &m->parcelRules_);
    model.Bind("estateName", &m->estateName_);
    model.Bind("estateOwner", &m->estateOwner_);
    model.Bind("estateCovenant", &m->estateCovenant_);
    model.RegisterArray<std::vector<Rml::String>>();
    model.Bind("version", &m->version_);
    model.Bind("builtWith", &m->builtWith_);
    model.Bind("licenses", &m->licenseNames_);
    model.Bind("license", &m->license_);
    model.Bind("licenseText", &m->licenseText_);
    model.BindEventCallback("close", [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            { m->setOpen(false); });
    menu->model_ = model.GetModelHandle();

    if(!menu->surface_->load("main_menu.rml"))
    {
        return nullptr;
    }
    return menu;
}

void MainMenu::setOpen(bool open)
{
    if(open == open_)
    {
        return;
    }
    open_ = open;
    if(!open)
    {
        surface_->pointerLeave();
    }
}

void MainMenu::set(const char* variable, std::string& field, std::string value)
{
    if(field != value)
    {
        field = std::move(value);
        model_.DirtyVariable(variable);
    }
}

void MainMenu::readLicenses()
{
    // Each starts with a record separator, its name and a newline
    // (cmake/Licenses.cmake).
    const std::string_view all(reinterpret_cast<const char*>(assets_embedded::kLicensesTxt),
                               sizeof(assets_embedded::kLicensesTxt));
    size_t at = all.find('\x1e');
    while(at != std::string_view::npos)
    {
        const size_t nameEnd = all.find('\n', at);
        if(nameEnd == std::string_view::npos)
        {
            break;
        }
        const size_t next = all.find('\x1e', nameEnd);
        licenseNames_.emplace_back(all.substr(at + 1, nameEnd - at - 1));
        licenses_.emplace_back(all.substr(nameEnd + 1, next == std::string_view::npos
                                                           ? std::string_view::npos
                                                           : next - nameEnd - 1));
        at = next;
    }
    licenseText_ = licenses_.empty() ? std::string() : licenses_.front();
    shownLicense_ = 0;

    version_ = VIEWER_VERSION;
#ifdef NDEBUG
    version_ += " (release build)";
#else
    version_ += " (debug build)";
#endif
    const int sdl = SDL_GetVersion();
    builtWith_ =
        std::format("With SDL {}.{}.{}, RmlUi {}", SDL_VERSIONNUM_MAJOR(sdl),
                    SDL_VERSIONNUM_MINOR(sdl), SDL_VERSIONNUM_MICRO(sdl), Rml::GetVersion());
}

void MainMenu::updateDestinations(const Status& status)
{
    if(inWorld_ != status.inWorld)
    {
        inWorld_ = status.inWorld;
        model_.DirtyVariable("inWorld");
    }
    Destinations& d = *destinations_;
    const bool shown = tab_ == "destinations";
    // The categories, when the tab is first shown (Refresh: again).
    if(shown && !d.categoriesFetched() && !d.categoriesLoading())
    {
        d.fetchCategories();
    }
    // A category's places when it's chosen, and for another maturity.
    if(shown && !category_.empty() &&
       (d.category() != category_ || (!d.loading() && d.maturity() != status.maturity)))
    {
        d.fetch(category_, status.maturity);
    }
    const auto put = [&](const char* variable, auto& field, auto value)
    {
        if(field != value)
        {
            field = std::move(value);
            model_.DirtyVariable(variable);
        }
    };
    put("categoriesNote", categoriesNote_,
        Rml::String(d.categoriesLoading() && d.categories().empty()
                        ? "Finding the guide's categories..."
                    : d.categoriesFetched() && d.categories().empty()
                        ? "The guide's categories couldn't be had (Refresh to try again)."
                        : ""));
    put("placesNote", placesNote_,
        Rml::String(d.loading() && d.places().empty()          ? "Finding places..."
                    : !d.error().empty() && d.places().empty() ? d.error()
                    : !d.loading() && d.places().empty()
                        ? "Nowhere here your maturity setting allows."
                        : ""));
    put("hasMore", hasMore_, d.hasMore() && !d.loading());
    put("loadingMore", loadingMore_, d.loading() && !d.places().empty());

    bool chosenChanged = chosen_ != shownChosen_;
    const bool listsChanged = d.version() != placesVersion_ || status.maturity != maturity_;
    if(listsChanged)
    {
        maturity_ = status.maturity;
        // The chosen one stays chosen if it's still there.
        const std::string was = chosen_ >= 0 && chosen_ < static_cast<int>(placeLocations_.size())
                                    ? placeLocations_[static_cast<size_t>(chosen_)]
                                    : std::string();
        placesVersion_ = d.version();
        std::vector<Category> categories;
        // The categories don't say their maturity: those for adults say so
        // in their names.
        const bool adult = maturity_.find('a') != std::string::npos;
        for(const DestinationCategory& c : d.categories())
        {
            if(!adult && c.name.starts_with("Adult"))
            {
                continue;
            }
            categories.push_back({c.id, c.name,
                                  std::format("{} {}", c.count, c.count == 1 ? "place" : "places"),
                                  c.image});
        }
        put("categories", categories_, std::move(categories));

        std::vector<Place> places;
        placeLocations_.clear();
        placeRegions_.clear();
        const int wasChosen = chosen_;
        chosen_ = -1;
        for(const Destination& place : d.places())
        {
            Place p;
            p.name = place.name;
            p.description = place.description;
            p.rating = place.maturity == "A" ? "a" : place.maturity == "M" ? "m" : "g";
            p.maturity = p.rating == "a" ? "Adult" : p.rating == "m" ? "Moderate" : "General";
            p.image = place.image;
            p.largeImage = place.largeImage;
            if(place.location == was)
            {
                chosen_ = static_cast<int>(places.size());
            }
            places.push_back(std::move(p));
            placeLocations_.push_back(place.location);
            placeRegions_.push_back(place.region);
        }
        put("places", places_, std::move(places));
        if(chosen_ != wasChosen)
        {
            model_.DirtyVariable("chosen");
        }
        chosenChanged = true;
    }
    // How many are at each place (the map's count of the region), looked
    // at every second while the places are shown: asking is what has them
    // counted again when the count's old.
    const double seconds = static_cast<double>(SDL_GetTicks()) / 1000.0;
    if(shown && !category_.empty() &&
       (listsChanged || population_->version() != populationVersion_ ||
        seconds - populationAt_ >= 1.0))
    {
        populationVersion_ = population_->version();
        populationAt_ = seconds;
        const auto now = GridPopulation::Clock::now();
        bool changed = false;
        for(size_t i = 0; i < places_.size() && i < placeRegions_.size(); i++)
        {
            const std::optional<int> n = population_->people(placeRegions_[i], now);
            const Rml::String people = !n        ? ""
                                       : *n == 0 ? "No one there"
                                       : *n == 1 ? "1 person"
                                                 : std::format("{} people", *n);
            if(places_[i].people != people)
            {
                places_[i].people = people;
                changed = true;
            }
        }
        if(changed)
        {
            model_.DirtyVariable("places");
            chosenChanged = true; // its count too
        }
    }

    // The category's name and description (when it's chosen, or once the
    // categories are here).
    if(category_ != shownCategory_ || listsChanged)
    {
        shownCategory_ = category_;
        Rml::String name;
        Rml::String text;
        for(const DestinationCategory& c : d.categories())
        {
            if(c.id == category_)
            {
                name = c.name;
                text = c.description;
            }
        }
        put("categoryName", categoryName_, name);
        put("categoryText", categoryText_, text);
    }
    if(chosenChanged)
    {
        if(chosen_ >= 0)
        {
            d.wantLargeImage(static_cast<size_t>(chosen_));
        }
        shownChosen_ = chosen_;
        pick_ = chosen_ >= 0 && chosen_ < static_cast<int>(places_.size())
                    ? places_[static_cast<size_t>(chosen_)]
                    : Place{};
        model_.DirtyVariable("pick");
    }
}

namespace
{

// 12345 as "12,345".
std::string grouped(int n)
{
    std::string digits = std::to_string(n < 0 ? -n : n);
    for(int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3)
    {
        digits.insert(static_cast<size_t>(i), ",");
    }
    return n < 0 ? "-" + digits : digits;
}

} // namespace

void MainMenu::updateParcel(const Status& status)
{
    const auto put = [&](const char* variable, auto& field, auto value)
    {
        if(field != value)
        {
            field = std::move(value);
            model_.DirtyVariable(variable);
        }
    };
    put("parcelKnown", parcelKnown_, status.parcel != nullptr);
    if(!status.parcel)
    {
        return;
    }
    namespace pf = parcel_flags;
    const Parcel& p = *status.parcel;
    put("parcelName", parcelName_, Rml::String(p.name.empty() ? "(No name)" : p.name));
    const std::string_view category = parcelCategoryName(p.category);
    put("parcelWhere", parcelWhere_,
        category.empty() ? status.region : std::format("{} · {}", status.region, category));
    put("parcelText", parcelText_,
        Rml::String(p.description.empty() ? "No description." : p.description));
    put("parcelImage", parcelImage_,
        p.snapshot.isNull() ? Rml::String()
                            : std::format("asset://{}?size=512", p.snapshot.toString()));
    const std::string owner = status.parcelOwner.empty() ? "..." : status.parcelOwner;
    put("parcelOwner", parcelOwner_, p.groupOwned ? owner + " (group)" : owner);
    put("parcelGroup", parcelGroup_,
        p.group.isNull()             ? Rml::String("None")
        : status.parcelGroup.empty() ? Rml::String("...")
                                     : Rml::String(status.parcelGroup));
    put("parcelArea", parcelArea_, std::format("{} m²", grouped(p.area)));
    put("parcelPrims", parcelPrims_,
        std::format("{} of {}", grouped(p.prims), grouped(p.maxPrims)));
    put("parcelSale", parcelSale_,
        p.has(pf::kForSale) ? std::format("For sale: L${}", grouped(p.salePrice)) : Rml::String());
    put("parcelMusic", parcelMusic_, Rml::String(p.musicUrl));

    const auto rule =
        [](const char* name, bool yes, bool group, const char* yesText, const char* noText)
    {
        return Rule{name,
                    yes     ? "yes"
                    : group ? "group"
                            : "no",
                    yes     ? yesText
                    : group ? "Group only"
                            : noText};
    };
    std::vector<Rule> rules = {
        rule("Flying", p.has(pf::kAllowFly), false, "Allowed", "Not allowed"),
        rule("Building", p.has(pf::kCreateObjects), p.has(pf::kCreateGroupObjects), "Anyone",
             "Not allowed"),
        rule("Scripts", p.has(pf::kAllowOtherScripts), p.has(pf::kAllowGroupScripts), "Anyone's",
             "Not allowed"),
        rule("Safe", !p.has(pf::kAllowDamage), false, "No damage", "Damage on"),
        rule("Pushing", !p.has(pf::kRestrictPush), false, "Allowed", "Restricted"),
        rule("Terraforming", p.has(pf::kAllowTerraform), false, "Allowed", "Not allowed"),
        rule("Voice", p.has(pf::kAllowVoice), false, "Allowed", "Not allowed"),
        rule("Access", !p.has(pf::kUseAccessGroup) && !p.has(pf::kUseAccessList),
             p.has(pf::kUseAccessGroup), "Open", "Restricted"),
        rule("Avatars", p.seeAvatars, false, "Seen from outside", "Hidden from outside"),
        rule("Sounds", !p.has(pf::kSoundLocal), false, "Heard outside", "Kept inside"),
    };
    put("parcelRules", parcelRules_, std::move(rules));

    put("estateName", estateName_,
        Rml::String(status.estate ? status.estate->name : std::string("...")));
    const bool owned = status.estate && !status.estate->owner.isNull();
    put("estateOwner", estateOwner_,
        Rml::String(!owned                       ? std::string()
                    : status.estateOwner.empty() ? std::string("...")
                                                 : status.estateOwner));
    put("estateCovenant", estateCovenant_,
        Rml::String(!status.estate                     ? ""
                    : status.estate->covenant.isNull() ? "No covenant"
                                                       : "Has a covenant (not shown here yet)"));
}

void MainMenu::update(const Status& status)
{
    updateDestinations(status);
    updateParcel(status);
    // The license picked on About.
    if(license_ != shownLicense_ && license_ >= 0 && license_ < static_cast<int>(licenses_.size()))
    {
        shownLicense_ = license_;
        licenseText_ = licenses_[static_cast<size_t>(license_)];
        model_.DirtyVariable("licenseText");
    }
    // The chosen tab's name (the page sets which).
    for(const Tab& t : tabs_)
    {
        if(t.id == tab_ && t.name != tabName_)
        {
            tabName_ = t.name;
            model_.DirtyVariable("tabName");
        }
    }

    const Clocks c = clocks();
    set("slt", slt_, c.slt);
    set("sltDate", sltDate_, c.sltDate);
    set("local", local_, c.local);
    set("localDate", localDate_, c.localDate);
    const char* part = c.localHour < 5    ? "Good evening"
                       : c.localHour < 12 ? "Good morning"
                       : c.localHour < 18 ? "Good afternoon"
                                          : "Good evening";
    set("greeting", greeting_,
        status.name.empty() ? std::string(part) + "." : std::format("{}, {}.", part, status.name));
    set("region", region_, status.region.empty() ? "Not in a region" : status.region);
    set("position", position_, status.position);

    // The numbers a few times a second, so they can be read.
    const double now = static_cast<double>(SDL_GetTicks()) / 1000.0;
    if(now - lastNumbers_ >= 0.25)
    {
        lastNumbers_ = now;
        viewerFps_ = status.viewerFps;
        regionFps_ = status.regionFps.value_or(0.0f);
        dilation_ = status.timeDilation.value_or(1.0f);
        model_.DirtyVariable("viewerFps");
        model_.DirtyVariable("regionFps");
        model_.DirtyVariable("dilation");
        set("viewerFpsText", viewerFpsText_,
            std::format("{:.0f}", static_cast<double>(status.viewerFps)));
        set("regionFpsText", regionFpsText_,
            status.regionFps ? std::format("{:.1f}", static_cast<double>(*status.regionFps))
                             : std::string("-"));
        set("dilationText", dilationText_,
            status.timeDilation ? std::format("{:.2f}", static_cast<double>(*status.timeDilation))
                                : std::string("-"));
    }
    surface_->update();
}

void MainMenu::reload()
{
    surface_->reload();
}

} // namespace viewer
