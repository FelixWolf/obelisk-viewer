#include "MainMenu.hpp"

#include "viewer/core/Log.hpp"

#include "EmbeddedAssets.hpp"

#include <RmlUi/Core.h>

#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_version.h>

#include <chrono>
#include <cmath>
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

// 1234567 as "1,234,567".
std::string groupThousands(int32_t value)
{
    const std::string digits = std::to_string(value < 0 ? -static_cast<int64_t>(value) : value);
    std::string out = value < 0 ? "-" : "";
    for(size_t i = 0; i < digits.size(); i++)
    {
        if(i > 0 && (digits.size() - i) % 3 == 0)
        {
            out += ',';
        }
        out += digits[i];
    }
    return out;
}

} // namespace

std::unique_ptr<MainMenu> MainMenu::create(gui::Gui& gui, Destinations& destinations,
                                           GridPopulation& population, InventoryBrowser& inventory,
                                           Settings& settings)
{
    std::unique_ptr<MainMenu> menu(new MainMenu);
    menu->settings_ = std::make_unique<SettingsView>(settings);
    menu->inventory_ = &inventory;
    menu->destinations_ = &destinations;
    menu->population_ = &population;
    menu->surface_ = gui.createSurface("main menu", kSize);
    if(!menu->surface_)
    {
        return nullptr;
    }
    menu->tabs_ = {{"home", "Home"},           {"chat", "Chat"},
                   {"people", "People"},       {"destinations", "Destinations"},
                   {"inventory", "Inventory"}, {"outfits", "Outfits"},
                   {"parcel", "Parcel"},       {"media", "Media"},
                   {"settings", "Settings"},   {"about", "About"}};
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
    model.Bind("displayName", &m->displayName_);
    model.Bind("picture", &m->picture_);
    model.Bind("balance", &m->balance_);
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
    if(auto entry = model.RegisterStruct<InvEntry>())
    {
        entry.RegisterMember("name", &InvEntry::name);
        entry.RegisterMember("kind", &InvEntry::kind);
        entry.RegisterMember("icon", &InvEntry::icon);
        entry.RegisterMember("link", &InvEntry::link);
        entry.RegisterMember("broken", &InvEntry::broken);
        entry.RegisterMember("worn", &InvEntry::worn);
    }
    model.RegisterArray<std::vector<InvEntry>>();
    if(auto item = model.RegisterStruct<InvItem>())
    {
        item.RegisterMember("name", &InvItem::name);
        item.RegisterMember("kind", &InvItem::kind);
        item.RegisterMember("icon", &InvItem::icon);
        item.RegisterMember("description", &InvItem::description);
        item.RegisterMember("creator", &InvItem::creator);
        item.RegisterMember("permissions", &InvItem::permissions);
        item.RegisterMember("created", &InvItem::created);
        item.RegisterMember("image", &InvItem::image);
        item.RegisterMember("link", &InvItem::link);
        item.RegisterMember("attachable", &InvItem::attachable);
        item.RegisterMember("clothing", &InvItem::clothing);
        item.RegisterMember("bodyPart", &InvItem::bodyPart);
        item.RegisterMember("worn", &InvItem::worn);
        item.RegisterMember("changing", &InvItem::changing);
    }
    model.RegisterArray<std::vector<Rml::String>>();
    model.Bind("invCrumbs", &m->invCrumbs_);
    model.Bind("invEntries", &m->invEntries_);
    model.Bind("invNote", &m->invNote_);
    model.Bind("invItemShown", &m->invItemShown_);
    model.Bind("invItem", &m->invItem_);
    const auto index = [](const Rml::VariantList& arguments)
    { return arguments.empty() ? -1 : arguments.front().Get<int>(-1); };
    model.BindEventCallback(
        "invOpen", [m, index](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& arguments)
        { m->openInventoryEntry(index(arguments)); });
    model.BindEventCallback(
        "invUp",
        [m, index](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& arguments)
        {
            // To a folder on the path (its crumb).
            const int depth = index(arguments);
            if(depth >= 0 && depth + 1 < static_cast<int>(m->invPath_.size()))
            {
                m->invPath_.resize(static_cast<size_t>(depth) + 1);
            }
            m->invShown_ = Uuid();
        });
    model.BindEventCallback("invBack",
                            [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                if(!m->invShown_.isNull())
                                {
                                    m->invShown_ = Uuid();
                                }
                                else if(m->invPath_.size() > 1)
                                {
                                    m->invPath_.pop_back();
                                }
                            });
    model.BindEventCallback("invAttach",
                            [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                if(!m->invTarget_.isNull())
                                {
                                    m->inventoryAction_ = InventoryAction{
                                        InventoryAction::Kind::Attach, m->invTarget_};
                                }
                            });
    model.BindEventCallback("invDetach",
                            [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                if(!m->invTarget_.isNull())
                                {
                                    m->inventoryAction_ = InventoryAction{
                                        InventoryAction::Kind::Detach, m->invTarget_};
                                }
                            });
    for(const auto& [event, kind] : {std::pair{"invWear", InventoryAction::Kind::Wear},
                                     std::pair{"invAdd", InventoryAction::Kind::Add},
                                     std::pair{"invTakeOff", InventoryAction::Kind::TakeOff}})
    {
        model.BindEventCallback(
            event,
            [m, kind](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
            {
                if(!m->invTarget_.isNull())
                {
                    m->inventoryAction_ = InventoryAction{kind, m->invTarget_};
                }
            });
    }
    model.BindEventCallback("invRefresh",
                            [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                if(!m->invPath_.empty())
                                {
                                    m->inventory_->refresh(m->invPath_.back());
                                }
                            });
    if(auto card = model.RegisterStruct<OutfitCard>())
    {
        card.RegisterMember("name", &OutfitCard::name);
        card.RegisterMember("image", &OutfitCard::image);
        card.RegisterMember("worn", &OutfitCard::worn);
    }
    model.RegisterArray<std::vector<OutfitCard>>();
    model.Bind("outfits", &m->outfitCards_);
    model.Bind("outfitsNote", &m->outfitsNote_);
    model.Bind("outfitPage", &m->outfitPage_);
    model.Bind("outfit", &m->outfit_);
    model.Bind("outfitItems", &m->outfitItems_);
    model.Bind("outfitNote", &m->outfitNote_);
    model.Bind("outfitChanging", &m->outfitChanging_);
    model.BindEventCallback(
        "outfitOpen",
        [m, index](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& arguments)
        {
            const int i = index(arguments);
            if(i >= 0 && i < static_cast<int>(m->outfitIds_.size()))
            {
                m->outfitShown_ = m->outfitIds_[static_cast<size_t>(i)];
            }
        });
    model.BindEventCallback("outfitBack",
                            [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            { m->outfitShown_ = Uuid(); });
    model.BindEventCallback("outfitWear",
                            [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                if(!m->outfitShown_.isNull())
                                {
                                    m->inventoryAction_ = InventoryAction{
                                        InventoryAction::Kind::WearOutfit, m->outfitShown_};
                                }
                            });
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
    if(auto row = model.RegisterStruct<MediaRow>())
    {
        row.RegisterMember("object", &MediaRow::object);
        row.RegisterMember("where", &MediaRow::where);
        row.RegisterMember("face", &MediaRow::face);
        row.RegisterMember("url", &MediaRow::url);
        row.RegisterMember("details", &MediaRow::details);
    }
    model.RegisterArray<std::vector<MediaRow>>();
    model.Bind("musicUrl", &m->musicUrl_);
    model.Bind("musicState", &m->musicState_);
    model.Bind("musicDetail", &m->musicDetail_);
    model.Bind("musicStation", &m->musicStation_);
    model.Bind("musicTitle", &m->musicTitle_);
    model.Bind("musicError", &m->musicError_);
    model.Bind("musicOn", &m->musicOn_);
    model.Bind("musicVolume", &m->musicVolume_);
    model.Bind("musicAuto", &m->musicAuto_);
    model.Bind("videoUrl", &m->videoUrl_);
    model.Bind("videoDetail", &m->videoDetail_);
    model.Bind("videoNote", &m->videoNote_);
    model.Bind("primMedia", &m->primMedia_);
    model.Bind("primMediaNote", &m->primMediaNote_);
    const auto mediaAction = [&model, m](const char* name, MediaAction action)
    {
        model.BindEventCallback(
            name, [m, action](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
            { m->mediaAction_ = action; });
    };
    model.Bind("voice", &m->voice_);
    model.Bind("microphone", &m->microphone_);
    model.BindEventCallback("toggleMicrophone",
                            [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            { m->microphonePressed_ = true; });
    mediaAction("musicPlay", MediaAction::PlayMusic);
    mediaAction("musicStop", MediaAction::StopMusic);
    mediaAction("louder", MediaAction::Louder);
    mediaAction("quieter", MediaAction::Quieter);
    model.Bind("estateName", &m->estateName_);
    model.Bind("estateOwner", &m->estateOwner_);
    model.Bind("estateCovenant", &m->estateCovenant_);
    model.Bind("appName", &m->appName_);
    model.Bind("version", &m->version_);
    model.Bind("builtWith", &m->builtWith_);
    model.Bind("licenses", &m->licenseNames_);
    model.Bind("license", &m->license_);
    model.Bind("licenseText", &m->licenseText_);
    model.BindEventCallback("close", [m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            { m->setOpen(false); });
    model.BindEventCallback("logOut", [m](Rml::DataModelHandle, Rml::Event&,
                                          const Rml::VariantList&) { m->logOut_ = true; });
    m->chat_.bind(model);
    m->people_.bind(model);
    m->settings_->bind(model);
    menu->model_ = model.GetModelHandle();
    m->chat_.setHandle(menu->model_);
    m->people_.setHandle(menu->model_);
    m->settings_->setHandle(menu->model_);

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
        rule("Privacy", p.seeAvatars, false, "Visible", "Hidden"),
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

void MainMenu::openInventoryEntry(int index)
{
    if(index < 0 || index >= static_cast<int>(invEntriesKnown_.size()))
    {
        return;
    }
    const InventoryEntry& e = invEntriesKnown_[static_cast<size_t>(index)];
    if(e.folder)
    {
        // A folder link opens what it points at.
        const inventory::Item* link = e.link ? inventory_->model().item(e.id) : nullptr;
        const Uuid folder = link ? link->linkedId : e.id;
        if(!e.broken)
        {
            invPath_.push_back(folder);
        }
        return;
    }
    invShown_ = e.id;
}

void MainMenu::updateInventory(const Status& status)
{
    const auto put = [&](const char* variable, auto& field, auto value)
    {
        if(field != value)
        {
            field = std::move(value);
            model_.DirtyVariable(variable);
        }
    };
    if(!status.inWorld)
    {
        invPath_.clear();
        invShown_ = Uuid();
        put("invNote", invNote_, Rml::String("Log in to see your inventory."));
        put("invEntries", invEntries_, std::vector<InvEntry>());
        put("invCrumbs", invCrumbs_, std::vector<Rml::String>());
        put("invItemShown", invItemShown_, false);
        invEntriesKnown_.clear();
        invFolderShown_ = Uuid();
        return;
    }
    const std::optional<Uuid> root = inventory_->root();
    if(!root)
    {
        put("invNote", invNote_, Rml::String("Opening your inventory..."));
        return;
    }
    if(invPath_.empty() || invPath_.front() != *root)
    {
        invPath_ = {*root};
    }
    const inventory::Model& model = inventory_->model();
    const InventoryBrowser::State state = inventory_->open(invPath_.back());
    if(inventory_->version() != invVersion_ || invPath_.size() != invPathShown_ ||
       invPath_.back() != invFolderShown_)
    {
        invVersion_ = inventory_->version();
        invPathShown_ = invPath_.size();
        invFolderShown_ = invPath_.back();
        // Where we are: the folders opened, the root as "Inventory".
        std::vector<Rml::String> crumbs;
        for(size_t i = 0; i < invPath_.size(); i++)
        {
            const inventory::Folder* f = model.folder(invPath_[i]);
            crumbs.push_back(i == 0 ? Rml::String("Inventory") : f ? f->name : Rml::String("..."));
        }
        put("invCrumbs", invCrumbs_, std::move(crumbs));

        invEntriesKnown_ = inventoryEntries(model, invPath_.back());
        std::vector<InvEntry> entries;
        for(const InventoryEntry& e : invEntriesKnown_)
        {
            entries.push_back(
                {e.name, std::string(e.kind.name), std::string(e.kind.icon), e.link, e.broken});
        }
        put("invEntries", invEntries_, std::move(entries));
        invWornShown_.clear(); // marked again below
        put("invNote", invNote_,
            Rml::String(!invEntriesKnown_.empty()                   ? ""
                        : state == InventoryBrowser::State::Loading ? "Opening..."
                        : state == InventoryBrowser::State::Failed
                            ? "This folder couldn't be opened (Refresh to try again)."
                            : "This folder is empty."));
    }

    // What's worn, marked (an attachment, or a link to one).
    const double seconds = static_cast<double>(SDL_GetTicks()) / 1000.0;
    if(status.worn && (invWornShown_ != *status.worn || seconds - invWornAt_ >= 0.5))
    {
        invWornShown_ = *status.worn;
        invWornAt_ = seconds;
        bool changed = false;
        for(size_t i = 0; i < invEntriesKnown_.size() && i < invEntries_.size(); i++)
        {
            const inventory::Item* target =
                invEntriesKnown_[i].folder ? nullptr : model.resolve(invEntriesKnown_[i].id);
            const bool worn = target && (status.worn->contains(target->id) ||
                                         (CurrentOutfit::isWearable(*target) && status.wearing &&
                                          status.wearing(target->id)));
            if(invEntries_[i].worn != worn)
            {
                invEntries_[i].worn = worn;
                changed = true;
            }
        }
        if(changed)
        {
            model_.DirtyVariable("invEntries");
        }
    }

    // An item alone: what it is, who made it, what may be done with it.
    InvItem item;
    if(const inventory::Item* own = invShown_.isNull() ? nullptr : model.item(invShown_))
    {
        const inventory::Item* target = model.resolve(invShown_);
        const inventory::Item& i = target ? *target : *own;
        const InventoryKind kind = inventoryKind(i.type, i.inventoryType);
        item.name = own->name;
        item.kind = std::string(kind.name);
        item.icon = std::string(kind.icon);
        item.description = i.description.empty() ? "No description." : i.description;
        const std::string creator = avatarNames_ && !i.permissions.creator.isNull()
                                        ? avatarNames_(i.permissions.creator)
                                        : std::string();
        item.creator = i.permissions.creator.isNull() ? "Unknown"
                       : creator.empty()              ? "..."
                                                      : creator;
        // What the owner (us) may do (llpermissionsflags.h's PERM_*).
        constexpr uint32_t kCopy = 0x8000;
        constexpr uint32_t kModify = 0x4000;
        constexpr uint32_t kTransfer = 0x2000;
        const uint32_t mask = i.permissions.ownerMask;
        std::string perms;
        for(const auto& [bit, name] : {std::pair{kCopy, "Copy"}, std::pair{kModify, "Modify"},
                                       std::pair{kTransfer, "Transfer"}})
        {
            perms +=
                std::format("{}{}{}", perms.empty() ? "" : " · ", mask & bit ? "" : "No ", name);
        }
        item.permissions = perms;
        if(i.createdAt > 0)
        {
            const std::chrono::sys_seconds when{std::chrono::seconds(i.createdAt)};
            item.created = std::format("{:%e %B %Y}", when);
        }
        item.image = i.type == 0 && !i.assetId.isNull()
                         ? std::format("asset://{}?size=512", i.assetId.toString())
                         : Rml::String();
        item.link =
            own->isLink() ? (target ? "A link to it" : "A link (to something not here)") : "";
        invTarget_ = i.id;
        item.attachable = i.type == inventory::kObject && (!own->isLink() || target);
        item.clothing = i.type == CurrentOutfit::kClothing;
        item.bodyPart = i.type == CurrentOutfit::kBodyPart;
        item.worn = (status.worn && status.worn->contains(i.id)) ||
                    ((item.clothing || item.bodyPart) && status.wearing && status.wearing(i.id));
        item.changing = status.changing && status.changing(i.id);
    }
    else
    {
        invShown_ = Uuid();
        invTarget_ = Uuid();
    }
    put("invItemShown", invItemShown_, !invShown_.isNull());
    put("invItem", invItem_, std::move(item));
}

void MainMenu::updateOutfits(const Status& status)
{
    const auto put = [&](const char* variable, auto& field, auto value)
    {
        if(field != value)
        {
            field = std::move(value);
            model_.DirtyVariable(variable);
        }
    };
    const auto thumbnail = [](const inventory::Folder& f)
    {
        return f.thumbnail.isNull() ? Rml::String()
                                    : std::format("asset://{}?size=256", f.thumbnail.toString());
    };
    const auto note = [&](const char* text)
    {
        put("outfitsNote", outfitsNote_, Rml::String(text));
        put("outfits", outfitCards_, std::vector<OutfitCard>());
        outfitIds_.clear();
        outfitShown_ = Uuid();
        put("outfitPage", outfitPage_, false);
    };
    if(!status.inWorld)
    {
        note("Log in to see your outfits.");
        return;
    }
    if(!inventory_->root())
    {
        note("Opening your inventory...");
        return;
    }
    const inventory::Model& model = inventory_->model();
    const inventory::Folder* mine = model.folderOfType(inventory::kMyOutfits);
    if(!mine)
    {
        note("There's no My Outfits folder.");
        return;
    }
    const InventoryBrowser::State state = inventory_->open(mine->id);
    put("outfitChanging", outfitChanging_, status.outfitChanging);
    if(inventory_->version() != outfitsVersion_ || outfitShown_ != outfitShownBuilt_ ||
       status.baseOutfit != outfitBaseShown_)
    {
        outfitsVersion_ = inventory_->version();
        outfitBaseShown_ = status.baseOutfit;
        // The outfits, by name.
        std::vector<const inventory::Folder*> folders = model.folders(mine->id);
        std::ranges::sort(folders, [](const inventory::Folder* a, const inventory::Folder* b)
                          { return a->name < b->name; });
        outfitIds_.clear();
        std::vector<OutfitCard> cards;
        for(const inventory::Folder* f : folders)
        {
            outfitIds_.push_back(f->id);
            cards.push_back({f->name, thumbnail(*f), f->id == status.baseOutfit});
        }
        put("outfits", outfitCards_, std::move(cards));
        put("outfitsNote", outfitsNote_,
            Rml::String(!outfitIds_.empty()                         ? ""
                        : state == InventoryBrowser::State::Loading ? "Finding your outfits..."
                        : state == InventoryBrowser::State::Failed ? "Your outfits couldn't be had."
                                                                   : "No outfits saved yet."));

        // One alone: its picture and what's in it.
        outfitShownBuilt_ = outfitShown_;
        const inventory::Folder* shown =
            outfitShown_.isNull() ? nullptr : model.folder(outfitShown_);
        if(!shown)
        {
            outfitShown_ = Uuid();
            outfitShownBuilt_ = Uuid();
        }
        put("outfitPage", outfitPage_, shown != nullptr);
        if(shown)
        {
            const InventoryBrowser::State items = inventory_->openLinks(shown->id);
            put("outfit", outfit_,
                OutfitCard{shown->name, thumbnail(*shown), shown->id == status.baseOutfit});
            std::vector<InvEntry> entries;
            for(const InventoryEntry& e : inventoryEntries(model, shown->id))
            {
                entries.push_back({e.name, std::string(e.kind.name), std::string(e.kind.icon),
                                   e.link, e.broken, false});
            }
            put("outfitNote", outfitNote_,
                Rml::String(!entries.empty()                            ? ""
                            : items == InventoryBrowser::State::Loading ? "Opening..."
                                                                        : "This outfit is empty."));
            put("outfitItems", outfitItems_, std::move(entries));
        }
    }
}

void MainMenu::updateMedia(const Status& status)
{
    const auto put = [&](const char* variable, auto& field, auto value)
    {
        if(field != value)
        {
            field = std::move(value);
            model_.DirtyVariable(variable);
        }
    };
    const Parcel* parcel = status.parcel;

    // Music: the parcel's URL, and the stream (if it plays that).
    const MusicStream::Status* m = status.music ? &status.music->status() : nullptr;
    const std::string url = parcel ? parcel->musicUrl : std::string();
    put("musicUrl", musicUrl_, Rml::String(url));
    const bool on =
        m && m->state != MusicStream::State::Stopped && m->state != MusicStream::State::Failed;
    put("musicOn", musicOn_, on);
    std::string state = "Stopped";
    std::string detail;
    if(m && m->state != MusicStream::State::Stopped)
    {
        state = std::string(MusicStream::stateName(m->state));
        if(m->format != audio::AudioStream::Format::Unknown)
        {
            detail = std::string(audio::AudioStream::formatName(m->format));
        }
        if(m->sampleRate > 0)
        {
            detail += std::format(" · {:.1f} kHz {}", m->sampleRate / 1000.0,
                                  m->channels == 1 ? "mono" : "stereo");
        }
        if(m->bitrate > 0)
        {
            detail += std::format(" · {} kbps", m->bitrate);
        }
    }
    put("musicState", musicState_, Rml::String(state));
    put("musicDetail", musicDetail_, Rml::String(detail));
    put("musicStation", musicStation_, Rml::String(m && on ? m->station : std::string()));
    put("musicTitle", musicTitle_, Rml::String(m && on ? m->title : std::string()));
    put("musicError", musicError_,
        Rml::String(m && m->state == MusicStream::State::Failed ? m->error : std::string()));
    put("musicVolume", musicVolume_,
        status.music ? static_cast<int>(std::lround(status.music->volume() * 100.0f)) : 0);
    put("musicAuto", musicAuto_,
        Rml::String(status.musicAutoPlay ? "Starts by itself on parcels with music."
                                         : "Stopped: parcels' music waits for Play."));

    // The parcel's video (or page): said, not shown yet.
    const ParcelMedia* video = parcel && !parcel->media.url.empty() ? &parcel->media : nullptr;
    put("videoUrl", videoUrl_, Rml::String(video ? video->url : std::string()));
    std::string videoDetail;
    if(video)
    {
        videoDetail = video->type.empty() ? std::string("Type not said") : video->type;
        if(video->width > 0 && video->height > 0)
        {
            videoDetail += std::format(" · {}×{}", video->width, video->height);
        }
        if(video->loop)
        {
            videoDetail += " · loops";
        }
        if(!video->description.empty())
        {
            videoDetail += " · " + video->description;
        }
    }
    put("videoDetail", videoDetail_, Rml::String(videoDetail));
    std::string videoNote = "Video and web pages can't be shown yet.";
    if(video && status.parcelMediaPlay && status.parcelMediaPlay->command)
    {
        namespace c = parcel_media_command;
        const uint32_t command = *status.parcelMediaPlay->command;
        videoNote += command == c::kPause                         ? " A script paused it."
                     : command == c::kPlay || command == c::kLoop ? " A script started it."
                                                                  : " A script stopped it.";
    }
    put("videoNote", videoNote_, Rml::String(videoNote));

    // Prims' media, face by face.
    std::vector<MediaRow> rows;
    for(const Status::PrimMedia& p : status.primMedia)
    {
        if(!p.media)
        {
            continue;
        }
        bool first = true;
        for(size_t f = 0; f < p.media->faces.size(); f++)
        {
            const auto& e = p.media->faces[f];
            if(!e)
            {
                continue;
            }
            MediaRow row;
            if(first)
            {
                row.object = p.name.empty() ? "(Object)" : p.name;
                row.where = std::format("{:.0f} m", p.distance);
                first = false;
            }
            row.face = std::format("Face {}", f);
            row.url = e->url().empty() ? "(No URL)" : e->url();
            std::string d;
            const auto add = [&](std::string_view part)
            {
                d += d.empty() ? "" : " · ";
                d += part;
            };
            if(status.primMediaState)
            {
                if(const std::string doing = status.primMediaState(p.object, f); !doing.empty())
                {
                    add(doing);
                }
            }
            if(e->autoPlay)
            {
                add("plays by itself");
            }
            if(e->autoLoop)
            {
                add("loops");
            }
            if(e->width > 0 && e->height > 0)
            {
                add(std::format("{}×{}", e->width, e->height));
            }
            if(e->whitelistEnable)
            {
                add(std::format("{} allowed site{}", e->whitelist.size(),
                                e->whitelist.size() == 1 ? "" : "s"));
            }
            if(!e->currentUrl.empty() && e->currentUrl != e->homeUrl && !e->homeUrl.empty())
            {
                add("home: " + e->homeUrl);
            }
            row.details = d;
            rows.push_back(std::move(row));
        }
    }
    put("primMedia", primMedia_, std::move(rows));
    put("primMediaNote", primMediaNote_,
        Rml::String(status.primMediaTotal == 0 ? "No media on prims around."
                    : status.primMediaTotal > status.primMedia.size()
                        ? std::format("The nearest {} of {} prims with media.",
                                      status.primMedia.size(), status.primMediaTotal)
                        : std::string()));
}

void MainMenu::update(const Status& status)
{
    updateDestinations(status);
    updateParcel(status);
    set("voice", voice_, status.voice);
    if(microphone_ != status.microphone)
    {
        microphone_ = status.microphone;
        model_.DirtyVariable("microphone");
    }
    if(tab_ == "media")
    {
        updateMedia(status);
    }
    if(status.chat && status.ims)
    {
        chat_.update(*status.chat, *status.ims, status.self, tab_ == "chat", status.headset,
                     surface_->document());
    }
    if(tab_ == "people" && status.friends && status.nearby && status.names)
    {
        people_.update(*status.friends, *status.nearby, status.names, status.here,
                       status.voiceStatus);
    }
    if(tab_ == "inventory")
    {
        updateInventory(status);
    }
    if(tab_ == "outfits")
    {
        updateOutfits(status);
    }
    if(tab_ == "settings")
    {
        settings_->update(status.headset);
    }
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
    set("displayName", displayName_, status.displayName);
    set("picture", picture_,
        status.picture.isNull() ? std::string()
                                : std::format("asset://{}?size=256", status.picture.toString()));
    set("balance", balance_,
        status.balance ? "L$" + groupThousands(*status.balance) : std::string());
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
    // Chat follows its newest line (once laid out).
    if(chat_.scrollAfterLayout(surface_->document()))
    {
        surface_->update();
    }
}

void MainMenu::reload()
{
    surface_->reload();
}

} // namespace viewer
