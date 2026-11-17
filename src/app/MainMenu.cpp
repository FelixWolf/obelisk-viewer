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

std::unique_ptr<MainMenu> MainMenu::create(gui::Gui& gui)
{
    std::unique_ptr<MainMenu> menu(new MainMenu);
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

void MainMenu::update(const Status& status)
{
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
