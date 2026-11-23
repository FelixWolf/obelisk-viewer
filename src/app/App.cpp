#include "App.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/image/Png.hpp"
#include "viewer/image/TextureProcessing.hpp"
#include "viewer/protocol/Messages.hpp"

#include "EmbeddedAssets.hpp"
#include "FloatingText.hpp"
#include "NameTags.hpp"
#include "PanelGeometry.hpp"

#include <SDL3/SDL.h>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <print>

#ifdef __linux__
#include <unistd.h>
#endif

namespace viewer
{
namespace fs = std::filesystem;

namespace
{

// No product name yet; this is the directory name used under the platform's
// cache location.
constexpr const char* kAppDirectoryName = "viewer";

// Without a home scene, where the login lobby starts (the test scene's
// ground, around the origin), at eye height; the login panel stands in
// front of it, north.
constexpr glm::vec3 kLobbyStart(1.5f, -6.0f, 1.7f);
// Eyes over the home scene's floor (a standing adult's).
constexpr float kLobbyEyeHeight = 1.7f;

// A marker's forward (its local +Y), flat.
glm::vec3 markerForward(const glm::quat& rotation)
{
    glm::vec3 forward = rotation * glm::vec3(0.0f, 1.0f, 0.0f);
    forward.z = 0.0f;
    return glm::length(forward) > 1e-4f ? glm::normalize(forward) : glm::vec3(0.0f, 1.0f, 0.0f);
}

fs::path defaultCacheDirectory()
{
#if defined(_WIN32)
    if(const char* local = std::getenv("LOCALAPPDATA"))
    {
        return fs::path(local) / kAppDirectoryName / "cache";
    }
#elif defined(__APPLE__)
    if(const char* home = std::getenv("HOME"))
    {
        return fs::path(home) / "Library" / "Caches" / kAppDirectoryName;
    }
#else
    if(const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
    {
        return fs::path(xdg) / kAppDirectoryName;
    }
    if(const char* home = std::getenv("HOME"))
    {
        return fs::path(home) / ".cache" / kAppDirectoryName;
    }
#endif
    return fs::temp_directory_path() / kAppDirectoryName;
}

// Where `region`'s corner is, in meters from `origin`'s.
glm::vec3 regionOffset(uint64_t region, uint64_t origin)
{
    return glm::vec3(
        static_cast<float>(static_cast<int64_t>(region >> 32) - static_cast<int64_t>(origin >> 32)),
        static_cast<float>(static_cast<int64_t>(region & 0xFFFFFFFFu) -
                           static_cast<int64_t>(origin & 0xFFFFFFFFu)),
        0.0f);
}

void printUsage()
{
    std::println(stderr,
                 "usage: viewer [--cache-dir PATH] [--import-textures DIR] [--grid LOGIN_URI] "
                 "[--login-file PATH] [--no-vsync] [--start LOCATION] [--teleport LOCATION] "
                 "[--walk DEGREES] [--day-time 0..1] [--sky ASSET] [--no-shadows] [--texture-memory MIB] "
                 "[--bandwidth KBPS] [--pointer X,Y] [--point-at UUID] [--click-at SECONDS] "
                 "[--say TEXT] [--say-at SECONDS] [--dump-avatar NAME|self|UUID] [--dump-at "
                 "SECONDS] [--front-view] [--no-occlusion] [--stereo-benchmark [SIZE]] "
                 "[--first-person] [--vr] [--vr-overscan PERCENT] [--vr-eye-height METRES] "
                 "[--ui-dir DIR] [--home-scene PATH] [--viewerconfig URL|off] "
                 "[--main-menu [TAB]] [--developer] "
                 "[--without shadows,lights,probes,glow,exposure,clusters,particles,water]");
}

} // namespace

std::optional<AppOptions> parseArguments(std::span<char*> args)
{
    AppOptions options;
    for(size_t i = 1; i < args.size(); i++)
    {
        const std::string_view arg = args[i];
        const bool hasValue = i + 1 < args.size();
        if(arg == "--cache-dir" && hasValue)
        {
            i++;
            options.cacheDirectory = args[i];
        }
        else if(arg == "--import-textures" && hasValue)
        {
            i++;
            options.importDirectory = args[i];
        }
        else if(arg == "--camera" && hasValue)
        {
            i++;
            std::array<float, 6> c{};
            if(std::sscanf(args[i], "%f %f %f %f %f %f", &c[0], &c[1], &c[2], &c[3], &c[4],
                           &c[5]) != 6)
            {
                printUsage();
                return std::nullopt;
            }
            options.camera = c;
        }
        else if(arg == "--test-scene")
        {
            options.testScene = true;
        }
        else if(arg == "--start" && hasValue)
        {
            i++;
            options.start = args[i];
        }
        else if(arg == "--no-vsync")
        {
            options.vsync = false;
        }
        else if(arg == "--first-person")
        {
            options.firstPerson = true;
        }
        else if(arg == "--vr")
        {
            options.vr = true;
        }
        else if(arg == "--vr-overscan" && hasValue)
        {
            i++;
            options.vrOverscan = std::strtof(args[i], nullptr) / 100.0f;
        }
        else if(arg == "--developer")
        {
            options.developer = true;
        }
        else if(arg == "--main-menu")
        {
            options.mainMenu = true;
            // And a tab to show ("destinations", ...).
            if(hasValue && !std::string_view(args[i + 1]).starts_with("--"))
            {
                options.mainMenuTab = args[++i];
            }
        }
        else if(arg == "--ui-dir" && hasValue)
        {
            i++;
            options.uiDirectory = args[i];
        }
        else if(arg == "--home-scene" && hasValue)
        {
            i++;
            options.homeScene = args[i];
        }
        else if(arg == "--viewerconfig" && hasValue)
        {
            i++;
            options.viewerConfig =
                std::string_view(args[i]) == "off" ? std::string() : std::string(args[i]);
        }
        else if(arg == "--vr-eye-height" && hasValue)
        {
            i++;
            options.vrEyeHeight = std::strtof(args[i], nullptr);
        }
        else if(arg == "--pbr-terrain")
        {
            options.pbrTerrain = true;
        }
        else if(arg == "--screenshot" && hasValue)
        {
            i++;
            options.screenshotPath = args[i];
        }
        else if(arg == "--screenshot-delay" && hasValue)
        {
            i++;
            options.screenshotDelay = std::strtof(args[i], nullptr);
        }
        else if(arg == "--login-file" && hasValue)
        {
            i++;
            options.loginFile = args[i];
        }
        else if(arg == "--pointer" && hasValue)
        {
            i++;
            std::array<float, 2> p{};
            if(std::sscanf(args[i], "%f,%f", &p[0], &p[1]) != 2)
            {
                printUsage();
                return std::nullopt;
            }
            options.pointer = p;
        }
        else if(arg == "--point-at" && hasValue)
        {
            i++;
            const auto id = Uuid::parse(args[i]);
            if(!id)
            {
                printUsage();
                return std::nullopt;
            }
            options.pointAt = *id;
        }
        else if(arg == "--stereo-benchmark")
        {
            options.stereoSize = 2160;
            if(hasValue && args[i + 1][0] != '-')
            {
                i++;
                options.stereoSize = static_cast<uint32_t>(std::strtoul(args[i], nullptr, 10));
            }
        }
        else if(arg == "--without" && hasValue)
        {
            i++;
            std::string_view list = args[i];
            while(!list.empty())
            {
                const size_t comma = list.find(',');
                options.without.emplace_back(list.substr(0, comma));
                list =
                    comma == std::string_view::npos ? std::string_view() : list.substr(comma + 1);
            }
        }
        else if(arg == "--no-occlusion")
        {
            options.occlusion = false;
        }
        else if(arg == "--say" && hasValue)
        {
            i++;
            options.say = args[i];
        }
        else if(arg == "--say-at" && hasValue)
        {
            i++;
            options.sayAt = std::strtof(args[i], nullptr);
        }
        else if(arg == "--click-at" && hasValue)
        {
            i++;
            options.clickAt = std::strtof(args[i], nullptr);
        }
        else if(arg == "--dump-avatar" && hasValue)
        {
            i++;
            options.dumpAvatar = args[i];
        }
        else if(arg == "--front-view")
        {
            options.frontView = true;
        }
        else if(arg == "--dump-at" && hasValue)
        {
            i++;
            options.dumpAt = std::strtof(args[i], nullptr);
        }
        else if(arg == "--teleport" && hasValue)
        {
            i++;
            options.teleport = args[i];
        }
        else if(arg == "--walk" && hasValue)
        {
            i++;
            options.walk = std::strtof(args[i], nullptr);
        }
        else if(arg == "--no-shadows")
        {
            options.shadows = false;
        }
        else if(arg == "--texture-memory" && hasValue)
        {
            i++;
            options.textureMemory =
                static_cast<uint32_t>(std::clamp(std::strtol(args[i], nullptr, 10), 16L, 65536L));
        }
        else if(arg == "--bandwidth" && hasValue)
        {
            i++;
            // The official viewer's range (getMaxBandwidthKbps).
            options.bandwidth = std::clamp(std::strtof(args[i], nullptr), 100.0f, 10000.0f);
        }
        else if(arg == "--sky" && hasValue)
        {
            i++;
            options.skyAsset = Uuid::parse(args[i]);
        }
        else if(arg == "--day-time" && hasValue)
        {
            i++;
            options.dayTime = std::strtof(args[i], nullptr);
        }
        else if(arg == "--grid" && hasValue)
        {
            i++;
            options.loginUri = args[i];
            options.gridGiven = true;
        }
        else
        {
            printUsage();
            return std::nullopt;
        }
    }
    return options;
}

App::SdlRuntime::~SdlRuntime()
{
    if(initialized)
    {
        SDL_Quit();
    }
}

std::unique_ptr<App> App::create(const AppOptions& options)
{
    std::unique_ptr<App> app(new App());

    if(!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
    {
        log::error("SDL_Init failed: {}", SDL_GetError());
        return nullptr;
    }
    app->sdl_.initialized = true;

    app->window_.reset(SDL_CreateWindow("Viewer", 1600, 900,
                                        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY));
    if(!app->window_)
    {
        log::error("SDL_CreateWindow failed: {}", SDL_GetError());
        return nullptr;
    }

    render::RendererOptions renderOptions;
#ifdef VIEWER_GPU_DEBUG
    renderOptions.gpuDebug = true;
#endif
    renderOptions.vsync = options.vsync;
    if(options.vr)
    {
        app->headset_ = xr::Headset::prepare(options.vrOverscan);
        renderOptions.deviceProperties = [headset = app->headset_.get()](SDL_PropertiesID props)
        { headset->addDeviceProperties(props); };
    }
    app->renderer_ = render::Renderer::create(app->window_.get(), renderOptions);
    if(!app->renderer_)
    {
        return nullptr;
    }
    if(app->headset_ &&
       (!app->renderer_->extendedDevice() || !app->headset_->start(app->renderer_->device())))
    {
        log::warn("--vr: no headset to show the world in; on the screen only");
        app->headset_.reset();
    }

    app->ui_ = ui::ImGuiLayer::create(app->window_.get(), *app->renderer_);
    if(!app->ui_)
    {
        return nullptr;
    }
    {
        // The viewer's own interface; without it, there's no main menu.
        const char* base = SDL_GetBasePath();
        gui::Gui::Options guiOptions;
        guiOptions.documents = options.uiDirectory.empty()
                                   ? fs::path(base ? base : "") / "assets" / "ui"
                                   : options.uiDirectory;
        guiOptions.windowFormat = app->renderer_->swapchainFormat();
        app->gui_ = gui::Gui::create(app->renderer_->device(), guiOptions);
        if(app->gui_)
        {
            // A text field on a page takes the keyboard: the window's text
            // input on (and off again after).
            app->gui_->setKeyboard(
                [window = app->window_.get()](bool on)
                { on ? SDL_StartTextInput(window) : SDL_StopTextInput(window); });
            // asset://UUID: a texture from the grid's asset servers, as the
            // world's are (loading while it's shown; nothing drawn until
            // it's in). ?size=N: how many texels across to load it at
            // (256 if not said), and the size it's laid out at when the
            // page gives it none.
            app->gui_->addImageScheme(
                "asset",
                [app = app.get()](std::string_view rest) -> std::optional<gui::SchemeImage>
                {
                    int size = 256;
                    if(const size_t query = rest.find('?'); query != std::string_view::npos)
                    {
                        const std::string_view params = rest.substr(query + 1);
                        rest = rest.substr(0, query);
                        if(params.starts_with("size="))
                        {
                            size = std::clamp(std::atoi(std::string(params.substr(5)).c_str()),
                                              static_cast<int>(TextureManager::kMinSize),
                                              static_cast<int>(TextureManager::kMaxSize));
                        }
                    }
                    const std::optional<Uuid> id = Uuid::parse(rest);
                    if(!id)
                    {
                        return std::nullopt;
                    }
                    return gui::SchemeImage{
                        glm::ivec2(size), [app, id = *id, size]
                        {
                            const render::GpuTexture* texture =
                                app->textures_ ? app->textures_->get(id, static_cast<float>(size))
                                               : nullptr;
                            // Sampled as sRGB, straight alpha, bottom-up
                            // (TextureManager).
                            return gui::ExternalImage{texture ? texture->get() : nullptr, true,
                                                      false, true};
                        }};
                });
        }
    }

    app->jobs_ = std::make_unique<JobSystem>();
    app->j2kDecoder_ = image::createJ2kDecoder();

    const fs::path cacheRoot =
        options.cacheDirectory.empty() ? defaultCacheDirectory() : options.cacheDirectory;
    // Every disk cache read and write goes through this (one file per entry
    // for now; see DiskCache.hpp).
    app->diskCache_ = std::make_unique<FileCache>(cacheRoot);
    app->textureCache_ =
        std::make_unique<assets::TextureCache>(*app->diskCache_, *app->jobs_, *app->j2kDecoder_);
    log::info("{} worker threads; cache at {}", app->jobs_->threadCount(),
              app->diskCache_->describe());

    app->textureBrowser_ = std::make_unique<TextureBrowser>(*app->textureCache_, *app->renderer_);
    if(!options.importDirectory.empty())
    {
        app->textureBrowser_->importDirectory(options.importDirectory);
    }

    app->sceneAssets_ = std::make_unique<SceneAssets>(*app->renderer_);
    app->loadTerrainBlendRamp();
    app->developer_ = options.developer;
    app->showTestScene_ = options.testScene;
    app->pbrTerrain_ = options.pbrTerrain;
    // In a headset, the view is from the avatar's eyes.
    app->wantFirstPerson_ = options.firstPerson || app->headset_ != nullptr;
    app->vrEyeHeight_ = options.vrEyeHeight;
    app->controls_.setMenu(app->radialMenu());
    app->controls_.setSubmit(
        [app = app.get()](const std::string& text)
        {
            // A text box's reply, if the keyboard was opened for one (and
            // it's still the one shown); else local chat.
            const ScriptNotice* notice = app->scriptDialogs_.front();
            const auto forDialog = std::exchange(app->keyboardForDialog_, std::nullopt);
            if(forDialog && notice && notice->serial == *forDialog)
            {
                if(const auto r = app->scriptDialogs_.reply(text); r && app->session_)
                {
                    app->session_->replyToScriptDialog(r->region, r->object, r->channel, r->index,
                                                       r->label);
                }
            }
            else
            {
                const auto into = std::exchange(app->keyboardForChat_, std::nullopt);
                app->sayIn(into.value_or(ChatView::kNearby), text);
            }
        });
    app->camera_.lookAt(glm::vec3(0.0f, -6.0f, 1.0f), 34.0f, 90.0f, 30.0f);
    if(options.camera)
    {
        const auto& c = *options.camera;
        app->camera_.lookAt(glm::vec3(c[0], c[1], c[2]), c[3], c[4], c[5]);
    }
    app->fixedCamera_ = options.camera.has_value();
    app->pendingTeleport_ = options.teleport;
    app->walkHeading_ = options.walk;
    app->pointerOverride_ = options.pointer;
    app->pointAt_ = options.pointAt;
    app->clickAt_ = options.clickAt;
    app->dumpAvatar_ = options.dumpAvatar;
    app->dumpAt_ = options.dumpAt;
    app->frontView_ = options.frontView;
    app->say_ = options.say;
    app->occlusion_ = options.occlusion;
    app->stereoSize_ = options.stereoSize;
    app->without_ = options.without;
    app->sayAt_ = options.sayAt;
    app->screenshotPath_ = options.screenshotPath;
    app->screenshotDelay_ = options.screenshotDelay;
    app->startTicks_ = SDL_GetTicksNS();
    app->lastFrameTicks_ = app->startTicks_;

    app->http_ = net::createHttpClient({.userAgent = std::format("viewer/{}", VIEWER_VERSION)});
    // A second texture cache on the same store: each instance has one
    // results queue, and the texture browser drains its own.
    app->worldTextureCache_ =
        std::make_unique<assets::TextureCache>(*app->diskCache_, *app->jobs_, *app->j2kDecoder_);
    app->environment_ = std::make_unique<EnvironmentManager>(*app->http_);
    app->environment_->setDayPosition(options.dayTime);
    app->environment_->setSkyAsset(options.skyAsset);
    app->shadows_ = options.shadows;
    app->bandwidth_ = options.bandwidth;
    app->textures_ = std::make_unique<TextureManager>(*app->worldTextureCache_, *app->renderer_,
                                                      *app->http_, *app->jobs_);
    app->textures_->setBudget(uint64_t(options.textureMemory) << 20);
    app->meshes_ =
        std::make_unique<MeshManager>(*app->http_, *app->jobs_, *app->renderer_, *app->diskCache_);
    app->materials_ = std::make_unique<MaterialManager>(*app->http_, *app->jobs_, *app->diskCache_);
    app->terrainOverrides_ = std::make_unique<TerrainOverrides>(*app->http_);
    app->inventory_ = std::make_unique<inventory::Model>();
    app->ais_ = std::make_unique<inventory::Ais>(*app->http_, *app->inventory_);
    app->inventoryBrowser_ = std::make_unique<InventoryBrowser>(*app->ais_, *app->inventory_);
    app->currentOutfit_ =
        std::make_unique<CurrentOutfit>(*app->http_, *app->ais_, *app->inventory_);
    // The main menu (with the workers and the web, for its pages' pictures).
    app->viewerConfig_ = std::make_unique<ViewerConfig>(
        *app->http_, *app->jobs_,
        ViewerConfig::Options{options.viewerConfig, cacheRoot / "viewerconfig", VIEWER_VERSION,
                              std::string(platformName())});
    if(app->gui_)
    {
        app->boards_ =
            LobbyBoards::create(*app->gui_, *app->http_, *app->jobs_, *app->viewerConfig_);
        if(!app->boards_)
        {
            log::warn("no news or grid status boards: their pages couldn't be set up");
        }
        app->destinations_ = std::make_unique<Destinations>(*app->http_, *app->jobs_, *app->gui_);
        app->mainMenu_ = MainMenu::create(*app->gui_, *app->destinations_, app->population_,
                                          *app->inventoryBrowser_);
        if(app->mainMenu_)
        {
            app->mainMenu_->setAvatarNames(
                [app = app.get()](const Uuid& id)
                {
                    const AvatarName* name = app->avatarNames_->get(id);
                    return name                             ? name->completeName()
                           : app->avatarNames_->missing(id) ? std::string("Unknown")
                                                            : std::string();
                });
        }
        if(app->mainMenu_ && options.mainMenu)
        {
            app->mainMenu_->setOpen(true);
            if(!options.mainMenuTab.empty())
            {
                app->mainMenu_->showTab(options.mainMenuTab);
            }
        }
    }
    if(!app->mainMenu_)
    {
        log::warn("no main menu: the interface couldn't be set up");
    }
    if(app->mainMenu_)
    {
        // The main menu on a panel (first person, VR): the menu button's
        // press opens and closes it, the pointers use it.
        app->controls_.setMainMenu([app = app.get()] { app->mainMenu_->toggle(); });
        app->setPanelPage(app->mainMenu_->surface(), MainMenu::kSize);
    }
    if(app->gui_)
    {
        app->dialogPage_ = ScriptDialogPage::create(*app->gui_);
    }
    if(app->dialogPage_)
    {
        app->dialogPage_->setNames(
            [app = app.get()](const Uuid& id) -> std::string
            {
                const AvatarName* name =
                    id.isNull() || !app->avatarNames_ ? nullptr : app->avatarNames_->get(id);
                return name ? name->userName() : std::string();
            });
        app->controls_.setNoticePage(app->pageOf(app->dialogPage_->surface(),
                                                 ScriptDialogPage::kSize,
                                                 ScriptDialogPage::kMetersPerPixel));
    }
    else
    {
        log::warn("no script dialogs: the interface couldn't be set up");
    }
    app->outfit_ = std::make_unique<OutfitAttachments>(*app->http_);
    app->ims_ = std::make_unique<InstantMessages>(app->http_.get());
    app->offers_ = std::make_unique<Offers>(app->http_.get());
    app->legacyMaterials_ = std::make_unique<LegacyMaterialManager>(*app->http_);
    {
        const char* base = SDL_GetBasePath();
        app->builtInHomeScene_ = fs::path(base ? base : "") / "assets" / "home.scene";
        app->homeSceneGiven_ = !options.homeScene.empty();
        // viewerconfig's (from the last time) if it named one.
        app->homeSceneVersion_ = app->viewerConfig_->homeSceneVersion();
        app->homeScenePath_ = app->homeSceneGiven_ ? options.homeScene
                              : !app->viewerConfig_->homeScene().empty()
                                  ? app->viewerConfig_->homeScene()
                                  : app->builtInHomeScene_;
        app->loadHomeScene();
    }
    app->avatarNames_ = std::make_unique<AvatarNames>(*app->http_);
    app->accountPicture_ = std::make_unique<AccountPicture>(*app->http_);
    app->objects_ = std::make_unique<ObjectView>(*app->jobs_, *app->renderer_, app->motion_);
    app->soundAssets_ = std::make_unique<SoundAssets>(*app->http_, *app->jobs_, *app->diskCache_);
    app->sounds_ = std::make_unique<SoundManager>(audio::AudioEngine::create(), *app->soundAssets_);
    app->trees_ = std::make_unique<TreeView>(*app->renderer_);
    {
        const char* base = SDL_GetBasePath();
        const fs::path character = fs::path(base ? base : "") / "assets" / "character";
        if(auto avatar = avatar::BaseAvatar::load(character))
        {
            app->animations_ = std::make_unique<AnimationManager>(
                *app->http_, *app->jobs_, avatar->skeleton, *app->diskCache_);
            app->avatars_ =
                std::make_unique<AvatarView>(std::move(*avatar), *app->renderer_, app->motion_);
        }
        else
        {
            log::error("default avatar not loaded ({}); avatars won't be drawn", avatar.error());
        }
    }
    app->savedLogins_ =
        std::make_unique<SavedLogins>(SavedLogins::defaultListFile(), SecretStore::system());
    app->loginFlow_ = std::make_unique<LoginFlow>(*app->savedLogins_, session::queryMachineInfo(),
                                                  "viewer", VIEWER_VERSION);
    app->loginPanel_ = std::make_unique<LoginPanel>(*app->loginFlow_, options.loginUri);
    if(app->gui_)
    {
        app->loginScreen_ = LoginScreen::create(*app->gui_, *app->loginFlow_, options.loginUri);
    }
    if(!app->loginScreen_)
    {
        log::warn("no login screen: the ImGui login panel instead");
    }
    if(!options.start.empty())
    {
        app->loginPanel_->setStart(options.start);
        if(app->loginScreen_)
        {
            app->loginScreen_->setStart(options.start);
        }
    }
    if(!options.loginFile.empty())
    {
        if(auto credentials = session::readCredentialsFile(options.loginFile))
        {
            if(options.gridGiven)
            {
                credentials->loginUri = options.loginUri.empty()
                                            ? std::string(session::kAgniLoginUri)
                                            : options.loginUri;
            }
            app->handleLoginAction(app->loginScreen_ ? app->loginScreen_->logInWith(*credentials)
                                                     : app->loginPanel_->logInWith(*credentials));
        }
        else
        {
            log::error("{}", credentials.error());
        }
    }

    return app;
}

void App::loadTerrainBlendRamp()
{
    // The official viewer's terrain blend ramp (CC-BY-SA artwork, see
    // assets/LICENSE-artwork.txt). Its decoder stores rows bottom-up, so the
    // first row it samples (v = 0) is the image's last row; ours are stored
    // top-down, hence the flip.
    auto ramp = image::textureFromJ2k(*j2kDecoder_, assets_embedded::kAlphaGradient2dJ2c);
    if(!ramp)
    {
        log::warn("couldn't decode the terrain blend ramp; using the generated one");
        return;
    }
    image::flipRows(*ramp);
    renderer_->setTerrainBlendRamp(*ramp);
}

void App::handleLoginAction(LoginAction action)
{
    switch(action.kind)
    {
    case LoginAction::Kind::None:
        break;
    case LoginAction::Kind::LogIn:
        // Destroy the old session first so its requests are cancelled before
        // the new one starts.
        endSession();
        session_ = std::make_unique<session::Session>(*http_, std::move(action.parameters));
        session_->setBandwidth(bandwidth_);
        // The world thread answers handshakes once it has the region's
        // object cache.
        session_->deferHandshakeReplies();
        worldThread_ = std::make_unique<world::WorldThread>(diskCache_.get());
        world_ = worldThread_->snapshot();
        break;
    case LoginAction::Kind::LogOut:
        if(session_)
        {
            session_->requestLogout(); // update() destroys it once it's done
        }
        break;
    }
}

int App::run()
{
    while(running_)
    {
        SDL_Event event;
        while(SDL_PollEvent(&event))
        {
            // ImGui (the debug windows) first, then the main menu.
            const bool imguiConsumed = ui_->processEvent(event);
            const bool uiConsumed = imguiConsumed || loginScreenEvent(event) ||
                                    panelTypingEvent(event) || scriptDialogEvent(event) ||
                                    mainMenuEvent(event);
            const bool releasing = event.type == SDL_EVENT_MOUSE_BUTTON_UP;
            // A free cursor (first person) points; it doesn't turn the view.
            const bool pointing = event.type == SDL_EVENT_MOUSE_MOTION && cursorFree();
            if((!uiConsumed || releasing) && !pointing)
            {
                camera_.handleEvent(event);
            }
            if(!uiConsumed && !ImGui::GetIO().WantCaptureKeyboard)
            {
                agent_.handleEvent(event);
            }
            // M: first person (the official viewer's mouselook key).
            if(event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
               event.key.scancode == SDL_SCANCODE_M && !uiConsumed &&
               !ImGui::GetIO().WantCaptureKeyboard)
            {
                setFirstPerson(!camera_.firstPerson());
            }
            // The left button touches what the pointer is on. In first
            // person (and in a headset) the buttons go through the VR
            // controls each frame (updateControls).
            if(!camera_.firstPerson())
            {
                if(event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                   event.button.button == SDL_BUTTON_LEFT && !uiConsumed)
                {
                    pressPointer();
                }
                if(event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                   event.button.button == SDL_BUTTON_LEFT)
                {
                    releasePointer();
                }
            }

            // Home: the headset's origin where the head is now.
            if(headset_ && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
               event.key.scancode == SDL_SCANCODE_HOME && !uiConsumed)
            {
                headset_->recenter();
            }
            if(event.type == SDL_EVENT_QUIT)
            {
                running_ = false;
            }
            if(event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
               event.window.windowID == SDL_GetWindowID(window_.get()))
            {
                running_ = false;
            }
        }

        profile_.beginFrame();
        // Keep the session going even while minimized.
        updateSession();
        updateMainMenu();
        profile_.mark(FrameProfile::Session);

        // The headset's news (its session starting, stopping, ending).
        if(headset_)
        {
            headset_->poll();
            if(headset_->ended())
            {
                log::info("the headset's session is over; on the screen only");
                headset_.reset();
            }
        }
        const bool headsetRunning = headset_ && headset_->running();
        if(!headsetRunning && SDL_GetWindowFlags(window_.get()) & SDL_WINDOW_MINIMIZED)
        {
            SDL_Delay(10);
            continue;
        }

        textureBrowser_->update();
        textures_->update();
        profile_.mark(FrameProfile::Textures);
        const uint64_t now = SDL_GetTicksNS();
        // The keys aren't the avatar's or the camera's while a page's text
        // field has them.
        const bool keyboardFree = !ImGui::GetIO().WantCaptureKeyboard && !typingOnPage();
        updateLobby();
        if(!session_)
        {
            // Logged out: the home scene's assets (if it's shown).
            setViewerAssetUrl(homeShown() ? home_->assetUrl : std::string());
        }
        if(keyboardFree)
        {
            camera_.update(static_cast<float>(now - lastFrameTicks_) * 1e-9f);
        }
        agent_.update(static_cast<float>(now - lastFrameTicks_) * 1e-9f, keyboardFree);
        constexpr size_t kFrameHistory = 240;
        if(frameTimes_.size() >= kFrameHistory)
        {
            frameTimes_.erase(frameTimes_.begin());
        }
        frameTimes_.push_back(static_cast<float>(now - lastFrameTicks_) * 1e-6f);
        lastFrameTicks_ = now;
        logMemory(now);
        // --stereo-benchmark: what both eyes cost, every few seconds.
        // --vr: the same for the headset.
        if((stereoSize_ > 0 || headsetRunning) && now - stereoLoggedTicks_ > 5'000'000'000ull &&
           !frameTimes_.empty())
        {
            stereoLoggedTicks_ = now;
            float total = 0.0f;
            for(const float ms : frameTimes_)
            {
                total += ms;
            }
            const float average = total / static_cast<float>(frameTimes_.size());
            const FrameProfile::Times stages = profile_.average();
            float cpu = 0.0f;
            for(size_t i = 0; i < stages.size(); i++)
            {
                const bool waiting = i == FrameProfile::Acquire || i == FrameProfile::HeadsetWait ||
                                     i == FrameProfile::HeadsetEnd;
                cpu += waiting ? 0.0f : stages[i];
            }
            const auto o = objects_->stats();
            log::info("stereo {}x{} per eye: {:.1f} FPS, {:.2f} ms a frame, {:.2f} ms CPU, "
                      "{:.2f} ms waiting for the GPU; {}; prims drawn {} ({} kept), {} culled, "
                      "{} faces, {} particles from {} sources",
                      headsetRunning ? headsetSize_.x : stereoSize_,
                      headsetRunning ? headsetSize_.y : stereoSize_, 1000.0f / average, average,
                      cpu, stages[FrameProfile::Acquire], FrameProfile::describe(stages), o.drawn,
                      o.reused, o.culled, o.faces, o.particles, o.particleSources);
            // Frames that missed the headset's (late by more than a
            // quarter of one), and the slowest's stages.
            const auto late =
                std::ranges::count_if(frameTimes_, [&](float ms) { return ms > average * 1.25f; });
            log::info("  {} of the last {} frames late; slowest: {}", late, frameTimes_.size(),
                      FrameProfile::describe(profile_.worst()));
        }
        // --dump-avatar: asks for its attachments' names a while before.
        if(!dumpAvatar_.empty() && !dumped_ && world_)
        {
            const float seconds = static_cast<float>(now - startTicks_) * 1e-9f;
            const auto target = dumpTarget();
            const world::Object* avatar =
                target ? world_->object(target->region, target->localId) : nullptr;
            if(avatar && session_ && seconds >= dumpAt_ - 10.0f)
            {
                for(const auto& [key, o] : world_->objects())
                {
                    if(o.parentId == avatar->localId && o.region == avatar->region)
                    {
                        objectProperties_.get(*session_, o.region, o.localId, o.id);
                    }
                }
            }
            if(seconds >= dumpAt_)
            {
                dumped_ = true;
                if(avatar)
                {
                    dumpAvatar(*avatar);
                }
                else
                {
                    log::warn("dump: no avatar '{}' in view", dumpAvatar_);
                }
            }
        }
        // --click-at: press at the given time, let go a moment later.
        if(clickAt_ && !clickedTicks_ && static_cast<float>(now - startTicks_) * 1e-9f >= *clickAt_)
        {
            pressPointer();
            clickedTicks_ = now;
        }
        else if(clickedTicks_ && now - clickedTicks_ > 200'000'000ull && session_)
        {
            releasePointer();
        }
        // --say: typing, then saying it.
        if(!say_.empty() && session_ && said_ < 2)
        {
            const float seconds = static_cast<float>(now - startTicks_) * 1e-9f;
            if(said_ == 0 && seconds >= sayAt_ - 3.0f)
            {
                session_->say({}, 0, ChatType::StartTyping);
                said_ = 1;
            }
            else if(said_ == 1 && seconds >= sayAt_)
            {
                session_->say({}, 0, ChatType::StopTyping);
                session_->say(say_);
                said_ = 2;
            }
        }
        if(!screenshotPath_.empty() && !screenshotRequested_ &&
           static_cast<float>(now - startTicks_) * 1e-9f >= screenshotDelay_)
        {
            renderer_->requestScreenshot();
            screenshotRequested_ = true;
        }

        // The headset's next frame: waiting for it sets the pace, and the
        // scene is built for the eyes' poses it predicts.
        profile_.mark(FrameProfile::Textures);
        headsetFrame_ = headsetRunning ? headset_->beginFrame() : std::nullopt;
        if(headsetFrame_)
        {
            headsetSize_ = {headsetFrame_->width, headsetFrame_->height};
            const xr::HeadsetFrame& f = *headsetFrame_;
            const glm::vec3 look = f.head.orientation * glm::vec3(1.0f, 0.0f, 0.0f);
            vr_.headYaw = glm::degrees(std::atan2(look.y, look.x));
            // The right stick turns in steps (pushed past most of the way,
            // then let back), the left one walks.
            // Not a hand whose stick points at the radial menu or scrolls
            // the main menu.
            const std::optional<int> menuHand = controls_.menuPointer();
            auto onPage = [&](size_t hand)
            { return controls_.pageOpen() && controlsPointed_[hand].onControls; };
            const float turn = menuHand == 1 || onPage(1) ? 0.0f : f.hands[1].stick.x;
            if(!vr_.snapped && std::abs(turn) > 0.7f)
            {
                camera_.turn(turn > 0.0f ? -30.0f : 30.0f);
                vr_.snapped = true;
            }
            else if(std::abs(turn) < 0.3f)
            {
                vr_.snapped = false;
            }
            // The right stick clicked: recenter (as Home).
            if(f.hands[1].stickClick && !vr_.recentering)
            {
                headset_->recenter();
            }
            vr_.recentering = f.hands[1].stickClick;
            const glm::vec2 walk = menuHand == 0 || onPage(0) ? glm::vec2(0.0f) : f.hands[0].stick;
            agent_.setStick(walk);
            vr_.walking = glm::length(walk) > 0.5f;
        }
        else
        {
            agent_.setStick(glm::vec2(0.0f));
            vr_.placed = false;
        }
        profile_.mark(FrameProfile::HeadsetWait);
        // The scene before the swapchain: building it overlaps the GPU
        // finishing earlier frames.
        buildScene(scene_);
        ui_->beginFrame();
        drawUi();
        ui_->endFrame();
        profile_.mark(FrameProfile::Ui);

        if(headsetFrame_)
        {
            // The eyes into the headset's images, the left one in the
            // window (when it has an image free), the UI over it.
            const bool windowShown =
                !(SDL_GetWindowFlags(window_.get()) &
                  (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN | SDL_WINDOW_OCCLUDED));
            if(auto frame = renderer_->beginFrame(false, windowShown))
            {
                profile_.mark(FrameProfile::Acquire);
                xr::HeadsetFrame& h = *headsetFrame_;
                if(loginScreenShown())
                {
                    loginScreen_->render(frame->commands); // before the panel shows it
                }
                if(boards_ && homeShown())
                {
                    boards_->render(frame->commands);
                }
                else if(mainMenu_ && mainMenu_->isOpen())
                {
                    mainMenu_->render(frame->commands); // before the panel shows it
                }
                if(dialogPage_ && noticeWaiting())
                {
                    dialogPage_->render(frame->commands);
                }
                if(h.render)
                {
                    renderer_->drawEyes(*frame, scene_, eyeCameras_, h.width, h.height);
                }
                else
                {
                    renderer_->drawMirror(*frame);
                }
                if(frame->target)
                {
                    ui_->record(*frame);
                }
                profile_.mark(FrameProfile::Render);
                // Only now the headset's images (once the last frame has
                // ended: all the above was done meanwhile).
                if(h.render && headset_->acquireImages(h))
                {
                    profile_.mark(FrameProfile::HeadsetEnd);
                    renderer_->copyEyes(*frame, h.targets, h.format);
                }
            }
            renderer_->endFrame();
            profile_.mark(FrameProfile::Submit);
            headset_->endFrame(); // after the eyes' commands are submitted; returns at once
            profile_.mark(FrameProfile::HeadsetEnd);
            headsetFrame_.reset();
        }
        else if(auto frame = renderer_->beginFrame())
        {
            profile_.mark(FrameProfile::Acquire);
            if(loginScreenShown())
            {
                loginScreen_->render(frame->commands);
            }
            if(boards_ && homeShown())
            {
                boards_->render(frame->commands);
            }
            else if(mainMenu_ && mainMenu_->isOpen())
            {
                mainMenu_->render(frame->commands);
            }
            if(dialogPage_ && noticeWaiting())
            {
                dialogPage_->render(frame->commands);
            }
            if(stereoSize_ > 0)
            {
                renderer_->drawStereo(*frame, scene_, stereoSize_, 0.064f);
            }
            else
            {
                renderer_->drawScene(*frame, scene_);
            }
            // The page over the window, and over a screenshot being made.
            const gui::Surface* over = nullptr;
            glm::vec4 rect(0.0f);
            if(mainMenuOverWindow())
            {
                over = &mainMenu_->surface();
                rect = mainMenuRect();
            }
            auto drawOver = [&](const gui::Surface& page, const glm::vec4& at)
            {
                gui_->drawOver(frame->commands, frame->target, {frame->width, frame->height}, page,
                               at);
                if(SDL_GPUTexture* shot = renderer_->screenshotImage())
                {
                    gui_->drawOver(frame->commands, shot, {frame->width, frame->height}, page, at);
                }
            };
            if(over)
            {
                drawOver(*over, rect);
            }
            // What a script asks, over that.
            if(dialogOverWindow())
            {
                drawOver(dialogPage_->surface(), dialogRect());
            }
            ui_->record(*frame);
            profile_.mark(FrameProfile::Render);
            renderer_->endFrame();
        }
        else
        {
            renderer_->endFrame();
        }
        profile_.mark(FrameProfile::Submit);
        profile_.endFrame();

        if(auto shot = renderer_->takeScreenshot())
        {
            if(image::writePng(screenshotPath_, *shot))
            {
                log::info("screenshot saved to {}", screenshotPath_.string());
                if(!frameTimes_.empty())
                {
                    float total = 0.0f;
                    float worst = 0.0f;
                    for(const float ms : frameTimes_)
                    {
                        total += ms;
                        worst = std::max(worst, ms);
                    }
                    const float average = total / static_cast<float>(frameTimes_.size());
                    log::info("frames: {:.1f} FPS, {:.2f} ms average, {:.2f} ms worst (last {})",
                              1000.0f / average, average, worst, frameTimes_.size());
                    log::info("frame stages, average: {}",
                              FrameProfile::describe(profile_.average()));
                    log::info("frame stages, slowest frame: {}",
                              FrameProfile::describe(profile_.worst()));
                }
                if(world_)
                {
                    const auto o = objects_->stats();
                    const auto m = meshes_->stats();
                    const auto t = textures_->stats();
                    log::info(
                        "avatars: {}; objects: {} drawn ({} mesh), {} faces; mesh assets: {} "
                        "known, {} "
                        "levels ready ({} from disk), {} failed; textures: {} resident, {} failed",
                        avatars_ ? avatars_->drawn() : size_t{0}, o.drawn, o.meshObjects, o.faces,
                        m.meshes, m.lodsReady, m.diskHits, m.failed, t.resident, t.failed);
                    log::info("trees drawn: {}; grass drawn: {}", trees_->stats().trees,
                              trees_->stats().grass);
                    log::info("rigged meshes drawn: {}; skipped objects: {}; out of view: {}; "
                              "animated objects: {}",
                              o.palettes, o.skipped, o.culled,
                              avatars_ ? avatars_->animatedObjects() : size_t{0});
                    const auto& sd = renderer_->shadowDraws();
                    log::info("shadow casters drawn per cascade: {} {} {} {}", sd[0], sd[1], sd[2],
                              sd[3]);
                    log::info("local lights: {} in range, {} used", scene_.lights.size(),
                              renderer_->lightsUsed());
                    log::info(
                        "HUD attachments: {} faces",
                        std::ranges::count_if(scene_.objects, [](const auto& d) { return d.hud; }));
                    log::info("reflection probes: {} local, {} captured, {} in use",
                              scene_.probes.size(), renderer_->probesCaptured(),
                              renderer_->probesUsed());
                    const auto lm = legacyMaterials_->stats();
                    log::info("legacy materials: {} ready, {} loading, {} missing", lm.ready,
                              lm.loading, lm.missing);
                    {
                        const auto ms = materials_->stats();
                        size_t withMaterials = 0;
                        size_t withOverrides = 0;
                        for(const auto& [key, object] : world_->objects())
                        {
                            if(!object.materials.empty() && withMaterials < 3)
                            {
                                log::info("PBR object {} at ({:.1f}, {:.1f}, {:.1f}), {} faces",
                                          object.id.toString(), object.position.x,
                                          object.position.y, object.position.z,
                                          object.materials.size());
                            }
                            withMaterials += !object.materials.empty();
                            withOverrides += world_->materialOverrides(key) != nullptr;
                        }
                        const auto& counts = session_->messageCounts();
                        const auto gsm = counts.find("GenericStreamingMessage");
                        log::info("GenericStreamingMessage received: {}",
                                  gsm == counts.end() ? 0 : gsm->second);
                        log::info("PBR: {} objects with materials, {} with overrides; {} faces "
                                  "drawn; materials {} ready, {} loading, {} failed",
                                  withMaterials, withOverrides, o.pbrFaces, ms.ready, ms.loading,
                                  ms.failed);
                    }
                    const auto c = worldThread_->cacheStats();
                    log::info("object cache: {} hits, {} misses, {} objects stored", c.hits,
                              c.misses, c.stored);
                    if(animations_)
                    {
                        const auto a = animations_->stats();
                        log::info("animations: {} ready, {} loading, {} failed", a.ready, a.loading,
                                  a.failed);
                    }
                    for(const auto& [key, object] : world_->objects())
                    {
                        if(object.type == world::ObjectType::Avatar)
                        {
                            log::info("avatar {} at ({:.1f}, {:.1f}, {:.1f})", object.id.toString(),
                                      object.position.x, object.position.y, object.position.z);
                        }
                        else if(object.animatedMesh && object.parentId == 0)
                        {
                            log::info("animated object {} at ({:.1f}, {:.1f}, {:.1f}), {} playing",
                                      object.id.toString(), object.position.x, object.position.y,
                                      object.position.z,
                                      world_->animations(object.id)
                                          ? world_->animations(object.id)->size()
                                          : size_t{0});
                        }
                    }
                }
            }
            else
            {
                log::error("couldn't write {}", screenshotPath_.string());
            }
            running_ = false;
        }
    }
    logOutBeforeExit();
    return 0;
}

void App::updateSession()
{
    if(!session_)
    {
        return;
    }
    session_->update();

    // What the main menu wants of the map (people in places).
    if(session_->state() == session::SessionState::Connected)
    {
        const auto wanted = population_.requests(GridPopulation::Clock::now());
        for(const std::string& region : wanted.regions)
        {
            session_->requestMapRegion(region);
        }
        for(const uint64_t region : wanted.agents)
        {
            session_->requestAgentLocations(region);
        }
        // And of the parcel we're on.
        const auto parcel = parcel_.requests();
        session_->requestGroupNames(parcel.groups);
        if(parcel.estate)
        {
            session_->requestEstateCovenant();
        }
    }

    // Instant messages' requests (what came while away, chats joined).
    ims_->update(*session_);
    for(const auto& m : ims_->takeOfflineFromObjects())
    {
        objectMessage(m);
    }
    // Offers that came while away, and answers by capability.
    for(const auto& m : ims_->takeOfflineOthers())
    {
        if(!offers_->handle(m, 0))
        {
            log::info("saved message of a kind not handled yet (dialog {}) from '{}'",
                      static_cast<int>(m.MessageBlock.Dialog),
                      protocol::toString(m.MessageBlock.FromAgentName));
        }
    }
    offers_->update(*session_);

    // Events for us (the parcel we're on).
    session_->drainRegionEvents(regionEvents_);
    for(const session::RegionEvent& e : regionEvents_)
    {
        if(e.name == "ParcelProperties")
        {
            parcel_.parcelProperties(e.regionHandle, session_->agentRegion().value_or(0), e.body);
        }
        else if(e.name == "ChatterBoxInvitation" && session_->login())
        {
            // A group or ad-hoc chat's first message: joined (accepted).
            ims_->invitation(e.body, session_->login()->agentId, std::chrono::system_clock::now());
        }
    }

    // Messages go to the world thread; this frame reads its latest snapshot
    // (the previous one is released over there).
    session_->drainRegionMessages(regionMessages_);
    for(auto& m : regionMessages_)
    {
        if(m.kind == session::RegionMessage::Kind::RegionRemoved)
        {
            worldThread_->postRegionRemoved(m.regionHandle);
            nearby_.removeRegion(m.regionHandle);
        }
        else if(m.event.is<protocol::msg::CoarseLocationUpdate>())
        {
            const auto update = m.event.decode<protocol::msg::CoarseLocationUpdate>();
            if(const auto* login = session_->login(); update && login)
            {
                nearby_.update(m.regionHandle, *update, login->agentId);
            }
        }
        else if(m.event.is<protocol::msg::OnlineNotification>() ||
                m.event.is<protocol::msg::OfflineNotification>())
        {
            const bool online = m.event.is<protocol::msg::OnlineNotification>();
            std::vector<Uuid> ids;
            if(online)
            {
                if(const auto n = m.event.decode<protocol::msg::OnlineNotification>())
                {
                    for(const auto& b : n->AgentBlock)
                    {
                        ids.push_back(b.AgentID);
                    }
                }
            }
            else if(const auto n = m.event.decode<protocol::msg::OfflineNotification>())
            {
                for(const auto& b : n->AgentBlock)
                {
                    ids.push_back(b.AgentID);
                }
            }
            friends_.setOnline(ids, online);
        }
        else if(m.event.is<protocol::msg::ChangeUserRights>())
        {
            const auto change = m.event.decode<protocol::msg::ChangeUserRights>();
            if(const auto* login = session_->login(); change && login)
            {
                friends_.rightsChanged(*change, login->agentId);
            }
        }
        else if(m.event.is<protocol::msg::TerminateFriendship>())
        {
            if(const auto ended = m.event.decode<protocol::msg::TerminateFriendship>())
            {
                log::info("a friendship ended");
                friends_.remove(ended->ExBlock.OtherID);
            }
        }
        else if(m.event.is<protocol::msg::ObjectProperties>())
        {
            objectProperties_.handle(*session_, m.regionHandle, m.event);
        }
        else if(m.event.is<protocol::msg::ChatFromSimulator>())
        {
            localChat_.handle(m.event, *avatarNames_, LocalChat::Clock::now());
        }
        else if(m.event.is<protocol::msg::ImprovedInstantMessage>())
        {
            const auto im = m.event.decode<protocol::msg::ImprovedInstantMessage>();
            if(const auto* login = session_->login(); im && login)
            {
                if(offers_->handle(*im, m.regionHandle))
                {
                    // an offer (for the notice page)
                }
                else if(const auto object =
                            ims_->handle(*im, login->agentId, std::chrono::system_clock::now()))
                {
                    objectMessage(*object);
                }
            }
        }
        else if(m.event.is<protocol::msg::SoundTrigger>())
        {
            const auto t = m.event.decode<protocol::msg::SoundTrigger>();
            if(t && originHandle_)
            {
                const auto& d = t->SoundData;
                // Where, in its region (the message's handle, else the one
                // that sent it).
                const uint64_t handle = d.Handle != 0 ? d.Handle : m.regionHandle;
                sounds_->trigger(d.SoundID,
                                 regionOffset(handle, *originHandle_) +
                                     glm::vec3(d.Position.x, d.Position.y, d.Position.z),
                                 d.Gain);
            }
        }
        else if(m.event.is<protocol::msg::PreloadSound>())
        {
            if(const auto p = m.event.decode<protocol::msg::PreloadSound>())
            {
                for(const auto& b : p->DataBlock)
                {
                    sounds_->preload(b.SoundID);
                }
            }
        }
        else if(m.event.is<protocol::msg::ScriptDialog>())
        {
            if(const auto dialog = m.event.decode<protocol::msg::ScriptDialog>())
            {
                scriptDialogs_.dialog(m.regionHandle, *dialog);
            }
        }
        else if(m.event.is<protocol::msg::ScriptQuestion>())
        {
            const auto question = m.event.decode<protocol::msg::ScriptQuestion>();
            if(const auto* login = session_->login(); question && login)
            {
                const std::string self = login->name.last.empty() || login->name.last == "Resident"
                                             ? login->name.first
                                             : login->name.first + " " + login->name.last;
                scriptDialogs_.question(m.regionHandle, *question, self,
                                        static_cast<double>(SDL_GetTicksNS()) * 1e-9);
            }
            else if(!question)
            {
                log::warn("a permission request that couldn't be read");
            }
        }
        else if(m.event.is<protocol::msg::LoadURL>())
        {
            if(const auto load = m.event.decode<protocol::msg::LoadURL>())
            {
                scriptDialogs_.loadUrl(m.regionHandle, *load);
            }
        }
        else if(m.event.is<protocol::msg::MapBlockReply>())
        {
            if(const auto reply = m.event.decode<protocol::msg::MapBlockReply>())
            {
                std::vector<GridPopulation::MapBlock> blocks;
                for(const auto& block : reply->Data)
                {
                    blocks.push_back({protocol::toString(block.Name), block.X, block.Y});
                }
                population_.mapBlocks(blocks);
            }
        }
        else if(m.event.is<protocol::msg::UUIDGroupNameReply>())
        {
            if(const auto reply = m.event.decode<protocol::msg::UUIDGroupNameReply>())
            {
                for(const auto& block : reply->UUIDNameBlock)
                {
                    parcel_.groupName(block.ID, protocol::toString(block.GroupName));
                }
            }
        }
        else if(m.event.is<protocol::msg::EstateCovenantReply>())
        {
            if(const auto reply = m.event.decode<protocol::msg::EstateCovenantReply>())
            {
                parcel_.estate(protocol::toString(reply->Data.EstateName),
                               reply->Data.EstateOwnerID, reply->Data.CovenantID);
            }
        }
        else if(m.event.is<protocol::msg::MapItemReply>())
        {
            const auto reply = m.event.decode<protocol::msg::MapItemReply>();
            if(reply && reply->RequestData.ItemType == session::Session::kMapAgentLocations)
            {
                std::vector<GridPopulation::Spot> spots;
                for(const auto& block : reply->Data)
                {
                    spots.push_back({block.X, block.Y, block.Extra});
                }
                population_.agentLocations(spots, GridPopulation::Clock::now());
            }
        }
        else if(m.event.is<protocol::msg::ViewerEffect>())
        {
            if(const auto* login = session_->login())
            {
                ik_.effects.receive(m.event, login->agentId);
            }
        }
        else
        {
            worldThread_->post(m.regionHandle, std::move(m.event));
        }
    }
    // What the world thread needs the regions told (object caches).
    worldThread_->drainRequests(worldRequests_);
    for(const world::WorldRequest& r : worldRequests_)
    {
        if(r.kind == world::WorldRequest::Kind::HandshakeReply)
        {
            session_->replyToHandshake(r.region, r.haveCache);
        }
        else
        {
            objectRequests_.clear();
            for(const auto& o : r.objects)
            {
                objectRequests_.emplace_back(o.localId, static_cast<uint8_t>(o.miss));
            }
            session_->requestObjects(r.region, objectRequests_);
        }
    }
    if(auto latest = worldThread_->snapshot(); latest != world_)
    {
        worldThread_->retire(std::exchange(world_, std::move(latest)));
    }

    requestMissingAppearances();

    // Where moving objects are this frame.
    motion_.update(world_.get(), session_.get(), std::chrono::steady_clock::now());

    // Arrivals move the world origin to the agent's region, and the camera
    // with it.
    if(session_->arrivals() != seenArrivals_)
    {
        seenArrivals_ = session_->arrivals();
        handleArrival(session_->lastArrival());
    }

    // The wheel asking for first or third person.
    if(const auto request = camera_.takeModeRequest())
    {
        setFirstPerson(*request);
    }
    // --first-person: once we're in the world.
    if(wantFirstPerson_ && agentKey_)
    {
        setFirstPerson(true);
        wantFirstPerson_ = !camera_.firstPerson();
    }
    // First person: the camera at our eyes, the body facing where it looks.
    if(camera_.firstPerson() && world_ && originHandle_ && avatars_ && agentKey_)
    {
        if(const world::Object* me = world_->object(agentKey_->region, agentKey_->localId))
        {
            // In a headset, steady: the headset moves the view itself.
            const bool steady = headset_ && headset_->running();
            if(auto eye = avatars_->eyePosition(*me, *world_, *originHandle_, steady))
            {
                if(steady)
                {
                    // The eyes ahead of the pelvis along the headset's
                    // heading (not the body's: its turning mustn't move
                    // the view), as they were when it was centered.
                    const float yaw = glm::radians(camera_.yaw());
                    *eye +=
                        glm::vec3(std::cos(yaw), std::sin(yaw), 0.0f) * avatars_->eyesForward(*me);
                }
                camera_.setEye(*eye);
            }
        }
        if(headset_ && headset_->running())
        {
            // The body turns once the head is far round from it (keeping it
            // within 45 degrees), and faces where the head does to walk.
            const float head = camera_.yaw() + vr_.headYaw;
            if(!vr_.bodyYaw || vr_.walking)
            {
                vr_.bodyYaw = head;
            }
            const float off = std::remainder(head - *vr_.bodyYaw, 360.0f);
            if(std::abs(off) > 45.0f)
            {
                *vr_.bodyYaw += off - std::copysign(45.0f, off);
            }
            *vr_.bodyYaw = std::remainder(*vr_.bodyYaw, 360.0f);
            agent_.setYaw(*vr_.bodyYaw);
        }
        else
        {
            agent_.setYaw(camera_.yaw());
        }
        // Kept captured: something (the window losing focus, say) may have
        // let go of it.
        // A free cursor (Tab held, or the radial menu open) let go of.
        const bool capture = !cursorFree();
        if(SDL_GetWindowRelativeMouseMode(window_.get()) != capture &&
           SDL_GetKeyboardFocus() == window_.get())
        {
            SDL_SetWindowRelativeMouseMode(window_.get(), capture);
        }
    }

    // The camera follows our avatar: it moves as the avatar does.
    if(world_ && originHandle_)
    {
        if(const auto global = agentGlobalPosition())
        {
            if(lastAgentGlobal_ && !fixedCamera_)
            {
                const glm::dvec3 moved = *global - *lastAgentGlobal_;
                if(glm::length(moved) < 50.0) // larger jumps are teleports: handleArrival
                {
                    camera_.shift(glm::vec3(moved));
                }
            }
            lastAgentGlobal_ = global;
        }
    }

    // --teleport, a few seconds after logging in.
    if(!pendingTeleport_.empty() && seenArrivals_ > 0 &&
       session_->state() == session::SessionState::Connected &&
       SDL_GetTicksNS() - arrivedTicks_ > 5'000'000'000ull)
    {
        if(pendingTeleport_ == "home")
        {
            session_->teleportHome();
        }
        else if(const auto location = session::parseLocation(pendingTeleport_))
        {
            session_->teleportTo(*location);
        }
        pendingTeleport_.clear();
    }

    // Tell the region where our camera is, in the agent's region's
    // coordinates, and how we want to move.
    if(originHandle_ && session_->agentRegion())
    {
        const render::Camera c = camera_.camera();
        const glm::vec3 at = glm::normalize(c.target - c.position);
        const glm::vec3 left = glm::normalize(glm::cross(glm::vec3(0, 0, 1), at));
        const glm::vec3 up = glm::cross(at, left);
        const glm::vec3 position =
            c.position + regionOffset(*originHandle_, *session_->agentRegion());
        session::Session::CameraView view;
        view.position = {position.x, position.y, position.z};
        view.atAxis = {at.x, at.y, at.z};
        view.leftAxis = {left.x, left.y, left.z};
        view.upAxis = {up.x, up.y, up.z};
        view.farDistance = 128.0f;
        session_->setCamera(view);
        session_->setControls(agent_.controlFlags(), agent_.bodyRotation());
    }
    // The sky: the main region's environment (once its capabilities are in).
    {
        const session::Region* main = session_->mainRegion();
        std::string_view url;
        if(main)
        {
            if(const auto it = main->capabilities.find("ExtEnvironment");
               it != main->capabilities.end())
            {
                url = it->second;
            }
        }
        // The agent's parcel, once its region says which (the main region's).
        std::optional<EnvironmentManager::ParcelKey> parcel;
        if(const auto& p = parcel_.parcel(); p && main && parcel_.region() == main->handle)
        {
            parcel = EnvironmentManager::ParcelKey{p->localId, p->environmentVersion};
        }
        environment_->update(main ? std::optional<uint64_t>(main->handle) : std::nullopt, url,
                             parcel);
    }
    std::string viewerAsset(session_->capability("ViewerAsset").value_or(""));
    if(viewerAsset.empty() && homeShown())
    {
        viewerAsset = home_->assetUrl; // the lobby's, until the region's is in
    }
    textures_->setViewerAssetUrl(viewerAsset);
    environment_->setViewerAssetUrl(viewerAsset);
    soundAssets_->setViewerAssetUrl(viewerAsset);
    if(avatars_)
    {
        const auto* login = session_->login();
        avatars_->setAppearanceServiceUrl(
            login ? login->reply["agent_appearance_service"].asString() : std::string());
    }
    meshes_->setViewerAssetUrl(viewerAsset);
    materials_->setViewerAssetUrl(viewerAsset);
    legacyMaterials_->setCapabilityUrl(
        std::string(session_->capability("RenderMaterials").value_or("")));
    avatarNames_->setCapabilityUrl(
        std::string(session_->capability("GetDisplayNames").value_or("")));
    currentOutfit_->setAppearanceCapability(
        std::string(session_->capability("UpdateAvatarAppearance").value_or("")));
    // PBR terrain overrides: each region's, once its capabilities are in.
    for(const auto& [circuit, region] : session_->regions())
    {
        if(const auto it = region->capabilities.find("ModifyRegion");
           it != region->capabilities.end())
        {
            terrainOverrides_->request(region->handle, it->second);
        }
    }
    terrainOverrides_->update();
    updateOutfit();
    if(animations_)
    {
        animations_->setViewerAssetUrl(viewerAsset);
    }

    if(session_->state() == session::SessionState::LoggedOut)
    {
        endSession();
    }
}

void App::handleArrival(const session::Arrival& arrival)
{
    arrivedTicks_ = SDL_GetTicksNS();
    if(arrival.kind == session::Arrival::Kind::Crossing && originHandle_)
    {
        // The same world from a new origin: the camera keeps its place.
        camera_.shift(regionOffset(*originHandle_, arrival.region));
        originHandle_ = arrival.region;
        return;
    }
    originHandle_ = arrival.region;
    lastAgentGlobal_.reset();
    const glm::vec2 look(arrival.lookAt.x, arrival.lookAt.y);
    if(glm::length(look) > 0.01f)
    {
        agent_.setYaw(glm::degrees(std::atan2(look.y, look.x)));
    }
    if(walkHeading_ && pendingTeleport_.empty())
    {
        agent_.setYaw(*walkHeading_);
        agent_.setAutoWalk(true);
    }
    if(fixedCamera_ && arrival.kind == session::Arrival::Kind::Login)
    {
        return;
    }
    // Behind the avatar, looking where it faces (--front-view: close in
    // front, looking at it).
    const auto& p = arrival.position;
    if(frontView_)
    {
        camera_.lookAt(glm::vec3(p.x, p.y, p.z + 0.4f), 2.2f, agent_.yaw() + 180.0f, 8.0f);
        return;
    }
    camera_.lookAt(glm::vec3(p.x, p.y, p.z + 0.3f), 5.0f, agent_.yaw(), 12.0f);
}

std::optional<glm::dvec3> App::agentGlobalPosition()
{
    const session::LoginSuccess* login = session_->login();
    if(!login)
    {
        return std::nullopt;
    }
    const world::Object* self =
        agentKey_ ? world_->object(agentKey_->region, agentKey_->localId) : nullptr;
    if(self && (self->id != login->agentId || self->region != session_->agentRegion()))
    {
        self = nullptr;
    }
    if(!self && world_->objectsRevision() != agentSearchRevision_)
    {
        // Look for it again (new region, or not seen yet). During a crossing
        // both regions may have it for a moment: prefer the agent's region.
        agentSearchRevision_ = world_->objectsRevision();
        agentKey_.reset();
        for(const auto& [key, object] : world_->objects())
        {
            if(object.type == world::ObjectType::Avatar && object.id == login->agentId)
            {
                agentKey_ = key;
                self = &object;
                if(key.region == session_->agentRegion())
                {
                    break;
                }
            }
        }
    }
    if(!self)
    {
        return std::nullopt;
    }
    glm::vec3 local = motion_.pose(*self).position;
    // Sitting: relative to what it sits on, up through the seat's link
    // parents.
    const world::Object* o = self;
    for(int depth = 0; depth < 16 && o->parentId != 0; depth++)
    {
        const world::Object* parent = world_->object(o->region, o->parentId);
        if(!parent)
        {
            break;
        }
        const world::Pose pose = motion_.pose(*parent);
        local = pose.position + pose.rotation * local;
        o = parent;
    }
    return glm::dvec3(static_cast<double>(self->region >> 32) + local.x,
                      static_cast<double>(self->region & 0xFFFFFFFFu) + local.y, local.z);
}

namespace
{

// The process's resident set (Linux; nothing elsewhere).
std::optional<uint64_t> residentBytes()
{
#ifdef __linux__
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if(!f)
    {
        return std::nullopt;
    }
    unsigned long long size = 0;
    unsigned long long resident = 0;
    const int read = std::fscanf(f, "%llu %llu", &size, &resident);
    std::fclose(f);
    if(read != 2)
    {
        return std::nullopt;
    }
    return resident * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
#else
    return std::nullopt;
#endif
}

} // namespace

void App::logMemory(uint64_t now)
{
    constexpr uint64_t kInterval = 30'000'000'000ull;
    if(now - lastMemoryLog_ < kInterval)
    {
        return;
    }
    lastMemoryLog_ = now;
    constexpr double kMiB = 1.0 / (1024.0 * 1024.0);
    const auto rss = residentBytes();
    const auto t = textures_->stats();
    const auto m = meshes_->stats();
    log::info("memory: resident {:.0f} MiB; textures {} resident ({:.0f} of {:.0f} MiB GPU, "
              "1/{} size, {} evicted; {:.0f} MiB CPU), {} loading, {} queued; mesh pool {:.0f} "
              "MiB; meshes {} ({} levels); prim shapes {}; objects {}",
              rss ? static_cast<double>(*rss) * kMiB : -1.0, t.resident,
              static_cast<double>(t.gpuBytes) * kMiB, static_cast<double>(t.budget) * kMiB,
              1 << t.bias, t.evicted, static_cast<double>(t.cpuBytes) * kMiB, t.loading, t.queued,
              static_cast<double>(renderer_->meshPoolBytes()) * kMiB, m.meshes, m.lodsReady,
              objects_ ? objects_->stats().meshes : size_t{0},
              world_ ? world_->objects().size() : size_t{0});
}

void App::requestMissingAppearances()
{
    // Regions send an avatar's appearance as it comes into view, but not
    // always (not ours after login, for one); the official viewer asks again
    // for avatars left with default parameters. A moment's grace first, for
    // the one on its way.
    constexpr auto kGrace = std::chrono::seconds(3);
    constexpr auto kRetry = std::chrono::seconds(30);
    const auto now = std::chrono::steady_clock::now();
    if(now < nextAppearanceCheck_)
    {
        return;
    }
    nextAppearanceCheck_ = now + std::chrono::seconds(1);
    for(const auto& [key, object] : world_->objects())
    {
        if(object.type != world::ObjectType::Avatar)
        {
            continue;
        }
        const world::World::Appearance* appearance = world_->appearance(object.id);
        if(appearance && !appearance->visualParams.empty())
        {
            appearanceDue_.erase(object.id);
            continue;
        }
        const auto [due, added] = appearanceDue_.try_emplace(object.id, now + kGrace);
        if(!added && now >= due->second)
        {
            log::info("asking for the appearance of avatar {}", object.id.toString());
            session_->requestAvatarAppearance(object.id);
            due->second = now + kRetry;
        }
    }
}

void App::updateOutfit()
{
    const auto* login = session_ ? session_->login() : nullptr;
    if(!login || !world_)
    {
        return;
    }
    const auto descendents = session_->capability("FetchInventoryDescendents2");
    const auto items = session_->capability("FetchInventory2");
    ais_->setCapability(std::string(session_->capability("InventoryAPIv3").value_or("")));
    ais_->update();
    // (The capabilities come together, once the seed capability answers.)
    if(!outfit_->started() && (ais_->available() || (descendents && items)))
    {
        const Uuid root = login->reply["inventory-root"][0]["folder_id"].asUuid();
        outfit_->start(*ais_, *inventory_, std::string(descendents.value_or("")),
                       std::string(items.value_or("")), login->agentId, root);
    }
    // What we wear now: our avatar's attachments' items (and their
    // objects), looked at twice a second.
    const double now = static_cast<double>(SDL_GetTicksNS()) * 1e-9;
    const bool look = now - wornAt_ >= 0.5;
    std::optional<double> arrived;
    if(agentKey_)
    {
        if(const world::Object* me = world_->object(agentKey_->region, agentKey_->localId))
        {
            if(!selfSeenAt_)
            {
                selfSeenAt_ = now;
            }
            arrived = now - *selfSeenAt_;
            if(look)
            {
                wornAt_ = now;
                worn_.clear();
                wornObjects_.clear();
                for(const auto& [key, object] : world_->objects())
                {
                    if(key.region != agentKey_->region || object.parentId != me->localId ||
                       object.attachItemId.isNull())
                    {
                        continue;
                    }
                    if(!worn_.insert(object.attachItemId).second && outfit_->busy())
                    {
                        log::warn("outfit: {} is attached twice", object.attachItemId.toString());
                    }
                    wornObjects_[object.attachItemId].push_back(key.localId);
                }
                currentOutfit_->update(worn_, now);
            }
        }
    }
    // An outfit put on: its attachments on (five a frame, as the login's),
    // the others off.
    const auto changes = currentOutfit_->takeAttachmentChanges(worn_);
    for(const Uuid& id : changes.attach)
    {
        if(const inventory::Item* item = inventory_->item(id))
        {
            outfitAttachQueue_.push_back(OutfitAttachments::toAttach(*item));
        }
    }
    std::vector<uint32_t> off;
    for(const Uuid& id : changes.detach)
    {
        if(const auto objects = wornObjects_.find(id); objects != wornObjects_.end())
        {
            off.insert(off.end(), objects->second.begin(), objects->second.end());
        }
        outfit_->forget(id);
    }
    profilePicture_ = {};
    session_->detach(off);
    if(!outfitAttachQueue_.empty())
    {
        const size_t n = std::min(outfitAttachQueue_.size(), OutfitAttachments::kPerRequest);
        session_->attachFromInventory(std::span(outfitAttachQueue_.data(), n));
        outfitAttachQueue_.erase(outfitAttachQueue_.begin(),
                                 outfitAttachQueue_.begin() + static_cast<std::ptrdiff_t>(n));
    }
    if(!outfit_->busy())
    {
        return;
    }
    const auto wear = outfit_->update(now, arrived, worn_);
    if(!wear.empty())
    {
        session_->attachFromInventory(wear);
    }
}

void App::endSession()
{
    accountPicture_->clear();
    population_.clear(); // the next login may be another grid
    parcel_.clear();
    inventoryBrowser_->clear();
    currentOutfit_->clear();
    wornObjects_.clear();
    outfitAttachQueue_.clear();
    setFirstPerson(false);
    appearanceDue_.clear();
    objectProperties_ = {};
    hoverKey_.reset();
    hoverInfo_ = nullptr;
    textures_->setViewerAssetUrl({});
    meshes_->setViewerAssetUrl({});
    materials_->setViewerAssetUrl({});
    legacyMaterials_->setCapabilityUrl({});
    avatarNames_ = std::make_unique<AvatarNames>(*http_); // the next login may be another grid
    localChat_.clear();
    scriptDialogs_.clear();
    keyboardForDialog_.reset();
    keyboardForChat_.reset();
    ims_->clear();
    offers_->clear();
    sounds_->clear();
    soundAssets_->clear();
    friends_.clear();
    friendsLoaded_ = false;
    nearby_.clear();
    offerAnswers_.clear();
    if(animations_)
    {
        animations_->setViewerAssetUrl({});
    }
    environment_->update(std::nullopt, {});
    motion_.update(nullptr, nullptr, std::chrono::steady_clock::now());
    session_.reset();
    world_.reset();
    worldThread_.reset();
    terrainView_.clear();
    terrainOverrides_->clear();
    ais_->cancel();
    ais_->setCapability({});
    outfit_->clear();
    inventory_->clear();
    selfSeenAt_.reset();
    worn_.clear();
    wornAt_ = -1e9;
    ik_ = Ik{};
    trees_->clear();
    if(avatars_)
    {
        avatars_->clear();
    }
    originHandle_.reset();
    seenArrivals_ = 0;
    agentKey_.reset();
    agentSearchRevision_ = ~0ull;
    lastAgentGlobal_.reset();
}

void App::buildScene(render::Scene& scene)
{
    // Before we arrive (logged out, or logging in): the home scene stands
    // where the world will be, its region the origin.
    const bool home = homeShown();
    std::shared_ptr<const world::World> sessionWorld;
    if(home)
    {
        sessionWorld = std::exchange(world_, homeWorld_);
        originHandle_ = kHomeRegion;
    }
    buildSceneOf(scene, home);
    if(home)
    {
        world_ = std::move(sessionWorld);
        originHandle_.reset();
    }
}

void App::buildSceneOf(render::Scene& scene, bool home)
{
    // Kept from frame to frame, so its lists keep their capacity.
    scene.objects.clear();
    scene.lights.clear();
    scene.labels.clear();
    scene.panels.clear();
    scene.pointerLines.clear();
    scene.particles.clear();
    // What a recent frame hid behind what (occlusion culling).
    scene.occlusion = occlusion_ ? renderer_->occlusion() : nullptr;
    scene.terrain.clear();
    scene.probes.clear();
    scene.worldOrigin = {};
    scene.camera = camera_.camera();
    if(headsetFrame_ && headsetFrame_->render)
    {
        placeEyes(scene.camera, *headsetFrame_);
    }
    const auto wallClock = std::chrono::system_clock::now().time_since_epoch();
    // Which sky track: by our avatar's height (the camera's until it's here).
    float altitude = scene.camera.position.z;
    if(session_ && world_)
    {
        if(const auto global = agentGlobalPosition())
        {
            altitude = static_cast<float>(global->z);
        }
    }
    environment_->apply(
        scene.lighting,
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::milliseconds>(wallClock).count()) /
            1000.0,
        altitude);
    scene.lighting.atmosphere.clouds = textures_->get(environment_->cloudTexture());
    scene.lighting.atmosphere.sunTexture = textures_->get(environment_->sunTexture(), 512.0f);
    scene.lighting.atmosphere.moonTexture = textures_->get(environment_->moonTexture(), 512.0f);
    // Shadows from whichever light is up (none once it's down).
    scene.shadows.enabled = shadows_ && scene.lighting.sunDirection.z > 0.02f &&
                            scene.lighting.sunColor != glm::vec3(0.0f);
    scene.shadows.towardLight = scene.lighting.sunDirection;
    scene.shadows.distance = 128.0f;
    scene.reflections.probeFace = renderer_->nextProbeFace();
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window_.get(), &width, &height);
    float viewAspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    int viewHeight = height;
    if(headsetFrame_ && headsetFrame_->render)
    {
        viewAspect = static_cast<float>(headsetFrame_->width) /
                     static_cast<float>(std::max(headsetFrame_->height, 1u));
        viewHeight = static_cast<int>(headsetFrame_->height);
    }
    else if(stereoSize_ > 0)
    {
        // A headset's eye: square, about 90 degrees each way.
        scene.camera.verticalFov = glm::radians(90.0f);
        viewAspect = 1.0f;
        viewHeight = static_cast<int>(stereoSize_);
    }
    textures_->setView(static_cast<uint32_t>(std::max(viewHeight, 1)), scene.camera.verticalFov);
    scene.frustum = render::Frustum::fromCamera(scene.camera, viewAspect);
    // The HUD image is as big as the view, so HUDs lay out as on screen.
    scene.hud.aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    scene.hud.pixelHeight = static_cast<float>(std::max(height, 1));
    if(avatars_)
    {
        avatars_->setHudAspect(scene.hud.aspect);
    }
    // What the pointer was on (the renderer's answer to an earlier frame),
    // and where it points now.
    if(world_ && objects_)
    {
        const auto before = pointer_.hover();
        pointer_.update(*renderer_, *world_, *objects_);
        // Held on what it touched: where on it the pointer is now.
        if(session_ && originHandle_ && session_->agentRegion())
        {
            pointer_.drag(*session_, *originHandle_, *session_->agentRegion(), SDL_GetTicksNS());
        }
        const auto& after = pointer_.hover();
        if((pointerOverride_ || !pointAt_.isNull()) &&
           (before.has_value() != after.has_value() ||
            (after && (after->key != before->key || after->face != before->face))))
        {
            if(const world::Object* o =
                   after ? world_->object(after->key.region, after->key.localId) : nullptr)
            {
                const world::Object& root = ObjectView::linkRoot(*o, *world_);
                log::info("pointer on {} face {} ({}{}touchable, click action {}; flags {:#x}, "
                          "root {} flags {:#x}) at {:.2f} {:.2f} {:.2f}",
                          o->id.toString(), after->face, after->hud ? "HUD, " : "",
                          after->touchable ? "" : "not ", after->clickAction, o->updateFlags,
                          root.id.toString(), root.updateFlags, after->hit.position.x,
                          after->hit.position.y, after->hit.position.z);
            }
            else
            {
                log::info("pointer on nothing");
            }
        }
    }
    updateControls(scene);
    const auto ray = pointerRay(scene);
    pointer_.aim(scene, ray);
    updateIk(scene, ray);
    // Resting on something clickable for a moment: its properties (touch
    // text, name), selecting it to get them. Not physical objects: selecting
    // stops them.
    {
        const auto& hover = pointer_.hover();
        const bool actionable =
            hover && (hover->touchable || hover->clickAction != world::ClickAction::Touch);
        const uint64_t now = SDL_GetTicksNS();
        if(!actionable || !hoverKey_ || *hoverKey_ != hover->key)
        {
            hoverKey_ = actionable ? std::optional(hover->key) : std::nullopt;
            hoverSinceTicks_ = now;
            hoverInfo_ = nullptr;
        }
        const world::Object* o =
            actionable && world_ ? world_->object(hover->key.region, hover->key.localId) : nullptr;
        if(o && session_ && now - hoverSinceTicks_ > 250'000'000ull &&
           !(o->updateFlags & world::kFlagUsePhysics))
        {
            hoverInfo_ = objectProperties_.get(*session_, o->region, o->localId, o->id);
            if(hoverInfo_ && (pointerOverride_ || !pointAt_.isNull()) && !loggedHoverInfo_)
            {
                loggedHoverInfo_ = true;
                log::info("pointer on '{}': touch text '{}', sit text '{}', description '{}'",
                          hoverInfo_->name, hoverInfo_->touchName, hoverInfo_->sitName,
                          hoverInfo_->description);
            }
        }
    }
    // Outline what a click would do something to.
    const auto& hover = pointer_.hover();
    scene.highlight = hover && !hover->hud &&
                              (hover->touchable || hover->clickAction != world::ClickAction::Touch)
                          ? hover->hit.pickId
                          : 0;
    addHoverLabel(scene);
    addCenterDot(scene);
    if(world_ && originHandle_)
    {
        terrainView_.update(*world_, *renderer_);
        terrainTextures_.update(*textures_, *renderer_);
        const auto placeholder = sceneAssets_->terrainBlinnPhong();
        terrainView_.addTo(scene, *originHandle_,
                           [&](uint64_t handle) -> TerrainView::Material
                           {
                               if(pbrTerrain_)
                               {
                                   // A shader test: procedural PBR layers
                                   // everywhere.
                                   return sceneAssets_->terrainPbr();
                               }
                               return terrainTextures_.material(
                                   *world_->terrain(handle), *textures_, *materials_,
                                   terrainOverrides_->overrides(handle), placeholder);
                           });
        const float seconds = static_cast<float>(SDL_GetTicksNS() - startTicks_) * 1e-9f;
        profile_.mark(FrameProfile::Terrain);
        addWater(scene, seconds);
        objects_->update();
        meshes_->update();
        materials_->update();
        legacyMaterials_->update();
        avatarNames_->update();
        if(animations_)
        {
            animations_->update();
        }
        profile_.mark(FrameProfile::Loading);
        // Before anything is culled: the occlusion map is only used at the
        // origin it was drawn at.
        scene.worldOrigin = glm::dvec2(static_cast<double>(*originHandle_ >> 32),
                                       static_cast<double>(*originHandle_ & 0xFFFFFFFFu));
        if(avatars_)
        {
            const double now = static_cast<double>(SDL_GetTicksNS() - startTicks_) * 1e-9;
            avatars_->addTo(scene, *world_, *originHandle_, *textures_, *animations_, *meshes_, now,
                            128.0f);
        }
        profile_.mark(FrameProfile::Avatars);
        placeHud(scene);
        if(const auto target = dumpTarget())
        {
            objects_->inspectWorn(target);
        }
        else
        {
            objects_->inspectWorn(developer_ && showWornMesh_ ? agentKey_ : std::nullopt);
        }
        objects_->addTo(scene, *world_, *originHandle_, *textures_, *meshes_, *materials_,
                        *legacyMaterials_, avatars_.get(), 128.0f);
        addFloatingText(scene, *world_, *objects_, *originHandle_, avatars_.get());
        updateSounds(scene);
        if(avatars_)
        {
            const auto* login = session_ ? session_->login() : nullptr;
            addNameTags(scene, *world_, *avatars_, *avatarNames_, localChat_,
                        login ? login->agentId : Uuid{}, *originHandle_, LocalChat::Clock::now());
        }
        trees_->addTo(scene, *world_, *originHandle_, *textures_, 128.0f);
        // Automatic reflection probes where objects are dense, at least 2 m
        // over the ground (scene positions are from the origin region's corner).
        const uint64_t origin = *originHandle_;
        const auto groundAt = [&](glm::vec2 p)
        {
            const double gx = static_cast<double>(origin >> 32) + static_cast<double>(p.x);
            const double gy = static_cast<double>(origin & 0xFFFFFFFFu) + static_cast<double>(p.y);
            constexpr double kWidth = world::kRegionWidth;
            const double rx = std::floor(gx / kWidth) * kWidth;
            const double ry = std::floor(gy / kWidth) * kWidth;
            const world::RegionTerrain* terrain =
                world_->terrain(static_cast<uint64_t>(rx) << 32 | static_cast<uint64_t>(ry));
            return terrain
                       ? terrain->heightAt(static_cast<float>(gx - rx), static_cast<float>(gy - ry))
                       : 0.0f;
        };
        auto automatic = placeAutomaticProbes(objects_->probeObjects(), groundAt, origin);
        scene.probes.insert(scene.probes.end(), automatic.begin(), automatic.end());
        profile_.mark(FrameProfile::Objects);
    }
    if(lobby_ && !home)
    {
        sceneAssets_->addFloor(scene, glm::vec2(kLobbyStart), 60.0f);
    }
    if(home && !boardsPlaced_) // the boards are the VR controls' to draw
    {
        // The side panels' places, without their pages: cards as big as
        // the login panel, facing as their markers do.
        auto placeholder = [&](const std::optional<world::HomeMarker>& marker, const char* text)
        {
            if(!marker)
            {
                return;
            }
            render::Panel panel;
            panel.frame =
                panelFacing(marker->position, marker->position + markerForward(marker->rotation));
            panel.size = glm::vec2(LoginScreen::kSize);
            panel.metersPerPixel = 0.00125f;
            panel.boxes.push_back(
                {glm::vec2(0.0f), panel.size, glm::vec4(0.086f, 0.11f, 0.165f, 0.97f), 24.0f});
            panel.texts.push_back({text, panel.size * 0.5f, 40.0f,
                                   glm::vec4(0.72f, 0.76f, 0.84f, 1.0f), panel.size.x - 80.0f});
            scene.panels.push_back(std::move(panel));
        };
        placeholder(home_->news, "News placeholder");
        placeholder(home_->gridStatus, "Grid status placeholder");
    }
    if(showTestScene_ || (!originHandle_ && !home))
    {
        // Logged in: next to the camera's target, so it's easy to find.
        const glm::vec3 origin =
            originHandle_ ? camera_.target() + glm::vec3(0.0f, 30.0f, 0.0f) : glm::vec3(0.0f);
        sceneAssets_->addTestScene(scene, origin);
    }

    // --without: passes left out, to measure what they cost.
    auto without = [&](std::string_view pass)
    { return std::ranges::find(without_, pass) != without_.end(); };
    if(!without_.empty())
    {
        scene.shadows.enabled = scene.shadows.enabled && !without("shadows");
        if(without("lights"))
        {
            scene.lights.clear();
        }
        if(without("probes"))
        {
            scene.reflections.enabled = false;
            scene.probes.clear();
        }
        scene.glow = !without("glow");
        scene.dynamicExposure = !without("exposure");
        scene.clusters = !without("clusters");
        objects_->setParticles(!without("particles")); // from the next frame
        if(without("particles"))
        {
            scene.particles.clear();
        }
        if(without("water"))
        {
            scene.water.enabled = false;
        }
    }
}

void App::addWater(render::Scene& scene, float seconds)
{
    render::Water& water = scene.water;
    water.patches.clear();
    water.enabled = false;
    if(!world_ || !originHandle_)
    {
        return;
    }
    constexpr int64_t kRegion = 256;
    constexpr int kReach = 10; // regions around the camera's that get water
    const int64_t originX = static_cast<int64_t>(*originHandle_ >> 32);
    const int64_t originY = static_cast<int64_t>(*originHandle_ & 0xFFFFFFFFu);
    auto cornerOf = [&](int64_t x, int64_t y)
    { return glm::vec2(static_cast<float>(x - originX), static_cast<float>(y - originY)); };
    // Each known region's water, at its height; the camera's region's
    // height (or the origin's) for the fog and the water beyond.
    const glm::vec3 camera = scene.camera.position;
    const int64_t cameraX =
        static_cast<int64_t>(std::floor((static_cast<double>(originX) + camera.x) / kRegion)) *
        kRegion;
    const int64_t cameraY =
        static_cast<int64_t>(std::floor((static_cast<double>(originY) + camera.y) / kRegion)) *
        kRegion;
    std::optional<float> cameraHeight;
    std::optional<float> originHeight;
    std::unordered_set<uint64_t> known;
    for(const auto& [handle, terrain] : world_->terrain())
    {
        if(!terrain.hasComposition)
        {
            continue;
        }
        const auto x = static_cast<int64_t>(handle >> 32);
        const auto y = static_cast<int64_t>(handle & 0xFFFFFFFFu);
        const float height = terrain.composition.waterHeight;
        known.insert(handle);
        water.patches.push_back({cornerOf(x, y), static_cast<float>(kRegion), height});
        if(x == cameraX && y == cameraY)
        {
            cameraHeight = height;
        }
        if(handle == *originHandle_)
        {
            originHeight = height;
        }
    }
    if(water.patches.empty())
    {
        return;
    }
    water.enabled = true;
    water.height = cameraHeight.value_or(originHeight.value_or(water.patches.front().height));
    for(int i = -kReach; i <= kReach; i++)
    {
        for(int j = -kReach; j <= kReach; j++)
        {
            const int64_t x = cameraX + i * kRegion;
            const int64_t y = cameraY + j * kRegion;
            if(x >= 0 && y >= 0 &&
               !known.contains(static_cast<uint64_t>(x) << 32 | static_cast<uint64_t>(y)))
            {
                water.patches.push_back(
                    {cornerOf(x, y), static_cast<float>(kRegion), water.height});
            }
        }
    }

    const environment::WaterSettings& w = environment_->water();
    water.fogColor = w.fogColor;
    water.fogDensity = w.fogDensityFor(false);
    water.fogDensityBelow = w.fogDensityFor(true);
    water.fresnelScale = w.fresnelScale;
    water.fresnelOffset = w.fresnelOffset;
    water.roughness = std::max(0.0f, w.blurMultiplier) * 2.0f;
    water.scaleAbove = w.scaleAbove;
    water.scaleBelow = w.scaleBelow;
    water.normalScale = w.normalScale;
    water.wave1 = w.wave1;
    water.wave2 = w.wave2;
    water.normalMap = textures_->getLinear(w.normalMap, 512.0f);
    water.nextNormalMap = w.nextNormalMap == w.normalMap
                              ? water.normalMap
                              : textures_->getLinear(w.nextNormalMap, 512.0f);
    water.normalBlend = w.normalBlend;
    water.time = seconds * 0.5f; // as the official phase_time
}

std::optional<PointerController::Ray> App::pointerRay(const render::Scene& scene) const
{
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(window_.get(), &width, &height);
    if(width <= 0 || height <= 0)
    {
        return std::nullopt;
    }
    // --point-at: from the camera straight at the object.
    if(!pointAt_.isNull() && world_ && objects_ && originHandle_)
    {
        for(const auto& [key, object] : world_->objects())
        {
            if(object.id != pointAt_)
            {
                continue;
            }
            const auto frame =
                objects_->objectFrame(object, *world_, *originHandle_, avatars_.get());
            if(!frame)
            {
                return std::nullopt;
            }
            PointerController::Ray ray;
            ray.origin = scene.camera.position;
            ray.direction = glm::normalize(glm::vec3((*frame)[3]) - ray.origin);
            return ray;
        }
        return std::nullopt;
    }
    // On the VR controls, it isn't on the world.
    if(camera_.firstPerson() && controlsPointed_[1].onControls)
    {
        return std::nullopt;
    }
    const render::Hud& hud = scene.hud;
    const float unit = 0.5f / std::max(hud.zoom, 0.01f);
    // Where a ray meets the HUD when it's a panel in the scene.
    auto onHudPanel = [&](PointerController::Ray& ray)
    {
        if(hud.placement == render::Hud::Placement::Screen)
        {
            return;
        }
        const glm::mat4 toPanel = glm::inverse(hud.panel);
        const glm::vec3 o(toPanel * glm::vec4(ray.origin, 1.0f));
        const glm::vec3 d(toPanel * glm::vec4(ray.direction, 0.0f));
        if(std::abs(d.x) > 1e-6f)
        {
            const glm::vec3 p = o + d * (-o.x / d.x);
            const float half = 0.5f * hud.panelHeight;
            if(-o.x / d.x > 0.0f && std::abs(p.y) <= half * hud.aspect && std::abs(p.z) <= half)
            {
                ray.hudPoint = glm::vec2(p.y, p.z) / hud.panelHeight * (2.0f * unit);
            }
        }
    };
    // In a headset: the right controller's ray.
    if(vr_.placed && headsetFrame_ && headsetFrame_->render)
    {
        if(!vrAim_)
        {
            return std::nullopt;
        }
        PointerController::Ray ray = *vrAim_;
        onHudPanel(ray);
        return ray;
    }
    float x = 0.0f;
    float y = 0.0f;
    if(camera_.firstPerson() && cursorFree())
    {
        // The cursor, let go of to point with.
        if(ImGui::GetIO().WantCaptureMouse || SDL_GetMouseFocus() != window_.get())
        {
            return std::nullopt;
        }
        SDL_GetMouseState(&x, &y);
    }
    else if(camera_.firstPerson())
    {
        // The center dot, in the middle.
        x = static_cast<float>(width) * 0.5f;
        y = static_cast<float>(height) * 0.5f;
    }
    else if(pointerOverride_)
    {
        x = (*pointerOverride_)[0] * static_cast<float>(width);
        y = (*pointerOverride_)[1] * static_cast<float>(height);
    }
    else
    {
        if(ImGui::GetIO().WantCaptureMouse || camera_.dragging() ||
           SDL_GetMouseFocus() != window_.get())
        {
            return std::nullopt;
        }
        SDL_GetMouseState(&x, &y);
        // Not through the main menu or a script's dialog over the window.
        const glm::vec2 pixel = glm::vec2(x, y) * SDL_GetWindowPixelDensity(window_.get());
        auto within = [&](const glm::vec4& rect)
        {
            return pixel.x >= rect.x && pixel.y >= rect.y && pixel.x < rect.x + rect.z &&
                   pixel.y < rect.y + rect.w;
        };
        if((mainMenuOverWindow() && within(mainMenuRect())) ||
           (dialogOverWindow() && within(dialogRect())))
        {
            return std::nullopt;
        }
    }
    // Through the view's projection, back from the window position.
    const glm::vec2 ndc(2.0f * x / static_cast<float>(width) - 1.0f,
                        1.0f - 2.0f * y / static_cast<float>(height));
    const render::Camera& camera = scene.camera;
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const glm::mat4 inverse = glm::inverse(camera.projection(aspect) * camera.view());
    const glm::vec4 far = inverse * glm::vec4(ndc, 1.0f, 1.0f);
    PointerController::Ray ray;
    ray.origin = camera.position;
    ray.direction = glm::normalize(glm::vec3(far) / far.w - camera.position);
    // Where it meets the HUD: over the view, straight from the window
    // position; on a panel, where the ray crosses it.
    if(hud.placement == render::Hud::Placement::Screen)
    {
        ray.hudPoint = glm::vec2(-ndc.x * unit * hud.aspect, ndc.y * unit);
    }
    onHudPanel(ray);
    return ray;
}

void App::placeEyes(render::Camera& camera, const xr::HeadsetFrame& frame)
{
    // The headset's origin where the camera is (the avatar's eyes in first
    // person), facing the camera's heading; its pitch is the head's own.
    const render::Camera base = camera;
    const glm::quat heading = glm::angleAxis(glm::radians(camera_.yaw()), glm::vec3(0, 0, 1));
    vr_.placed = true;
    vr_.base = base.position;
    vr_.heading = heading;
    const float scale = vrScale(frame);
    if(std::abs(scale - vr_.loggedScale) > 0.05f)
    {
        vr_.loggedScale = scale;
        log::info("VR: movements scaled by {:.2f} (the avatar's eye height over the wearer's)",
                  scale);
    }
    vr_.scale = scale;
    auto place = [&](const xr::Pose& pose)
    {
        render::Camera c = base;
        const glm::quat orientation = heading * pose.orientation;
        c.position = base.position + heading * (pose.position * scale);
        c.target = c.position + orientation * glm::vec3(1.0f, 0.0f, 0.0f);
        c.up = orientation * glm::vec3(0.0f, 0.0f, 1.0f);
        return c;
    };
    for(size_t e = 0; e < 2; e++)
    {
        eyeCameras_[e] = place(frame.eyes[e].pose);
        eyeCameras_[e].edges = frame.eyes[e].tangents;
    }
    // The scene's camera, which what both eyes share is done from (culling,
    // shadows, level of detail): the head, seeing all both eyes do.
    camera = place(frame.head);
    const glm::vec4& l = frame.eyes[0].tangents;
    const glm::vec4& r = frame.eyes[1].tangents;
    const glm::vec4 edges(std::min(l.x, r.x), std::max(l.y, r.y), std::min(l.z, r.z),
                          std::max(l.w, r.w));
    camera.edges = edges;
    camera.verticalFov = std::atan(edges.w) - std::atan(edges.z);
}

float App::vrScale(const xr::HeadsetFrame& frame) const
{
    // The wearer's eyes as told, or as measured once they're plausibly
    // someone's (not a headset on the desk).
    constexpr float kLowestEyes = 1.0f;
    if(!vrScaling_)
    {
        return 1.0f;
    }
    const std::optional<float> wearer = vrEyeHeight_ ? vrEyeHeight_ : frame.eyeHeight;
    if(!wearer || *wearer < (vrEyeHeight_ ? 0.1f : kLowestEyes) || !world_ || !agentKey_ ||
       !avatars_)
    {
        return 1.0f;
    }
    const world::Object* me = world_->object(agentKey_->region, agentKey_->localId);
    const std::optional<float> avatar = me ? avatars_->eyeHeight(*me) : std::nullopt;
    if(!avatar)
    {
        return vr_.scale;
    }
    return std::clamp(*avatar / *wearer, 0.25f, 4.0f);
}

void App::setFirstPerson(bool on)
{
    const auto* login = session_ ? session_->login() : nullptr;
    if(on && (!login || !agentKey_ || !avatars_))
    {
        return; // not logged in: nobody's eyes to look from
    }
    if(on == camera_.firstPerson())
    {
        return;
    }
    if(on)
    {
        // Facing the way the avatar does.
        camera_.setFirstPerson(true);
        camera_.lookAt(camera_.target(), 1.0f, agent_.yaw(), 0.0f);
    }
    else
    {
        // Behind the avatar again, the way it faces.
        camera_.setFirstPerson(false);
        const render::Camera c = camera_.camera();
        const glm::vec3 eye = c.position;
        camera_.lookAt(eye - glm::vec3(0.0f, 0.0f, 0.3f), 3.0f, agent_.yaw(), 12.0f);
    }
    agent_.setFirstPerson(on);
    agent_.setMouselookButton(false);
    avatars_->setFirstPerson(on ? std::optional(login->agentId) : std::nullopt);
    // The UI mustn't capture and release the mouse meanwhile (on X11 that
    // would let go of the pointer's grab, freeing it to the screen's edges).
    ui_->setMouseCapture(!on);
    if(!SDL_SetWindowRelativeMouseMode(window_.get(), on))
    {
        log::warn("first person: relative mouse mode failed: {}", SDL_GetError());
    }
    releasePointer();
}

void App::pressPointer()
{
    if(session_ && world_ && objects_ && originHandle_ && session_->agentRegion())
    {
        pointer_.press(*session_, *world_, *objects_, avatars_.get(), *originHandle_,
                       *session_->agentRegion());
    }
    // Pressing on something in the world: the hand reaches for it.
    if(const auto& hover = pointer_.hover(); hover && !hover->hud)
    {
        ik_.reachPoint = hover->hit.position;
        ik_.reaching = true;
        ik_.pressed = hover->key;
        ik_.pressDistance = glm::length(hover->hit.position - ik_.rayOrigin);
    }
}

MenuItem App::radialMenu()
{
    // The radial menu, top level first. An item is an action
    // (MenuItem::run) or a submenu (MenuItem::submenu); .when() greys it
    // out unless it holds, .named() works its label out when shown,
    // .staying() keeps the menu open after it. More than
    // RadialMenu::kSlices to a level page.
    return MenuItem::submenu(
        "",
        {
            MenuItem::run("Fly", [this] { agent_.setFlying(!agent_.flying()); })
                .named([this] { return std::string(agent_.flying() ? "Land" : "Fly"); })
                .when([this] { return !sitting(); }),
            MenuItem::run("Stand up", [this] { agent_.standUp(); })
                .when([this] { return sitting(); }),
            MenuItem::run("Chat",
                          [this]
                          {
                              keyboardForDialog_.reset();
                              keyboardForChat_.reset(); // local chat
                              controls_.openKeyboard();
                          }),
            MenuItem::submenu(
                "VR",
                {
                    MenuItem::run("Recenter",
                                  [this]
                                  {
                                      if(headset_)
                                      {
                                          headset_->recenter();
                                      }
                                  })
                        .when([this] { return headset_ && headset_->running(); }),
                    MenuItem::run("Avatar scale", [this] { vrScaling_ = !vrScaling_; })
                        .named(
                            [this]
                            {
                                return std::string(vrScaling_ ? "Avatar scale: on"
                                                              : "Avatar scale: off");
                            })
                        .staying(),
                }),
        });
}

bool App::mainMenuOverWindow() const
{
    return mainMenu_ && mainMenu_->isOpen() && !camera_.firstPerson() &&
           !(headset_ && headset_->running()) && !loginScreenShown();
}

glm::vec4 App::mainMenuRect() const
{
    return pageRect(MainMenu::kSize, 0.92f);
}

glm::vec4 App::pageRect(glm::ivec2 page, float fill) const
{
    // In the middle of the window: at the display's scale, or smaller to
    // fit.
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window_.get(), &width, &height);
    const glm::vec2 window(static_cast<float>(width), static_cast<float>(height));
    const glm::vec2 size(page);
    const float scale = std::min({SDL_GetWindowDisplayScale(window_.get()),
                                  window.x * fill / size.x, window.y * fill / size.y});
    const glm::vec2 fitted = glm::floor(size * scale);
    const glm::vec2 topLeft = glm::floor((window - fitted) * 0.5f);
    return glm::vec4(topLeft, fitted);
}

void App::setPanelPage(gui::Surface& surface, glm::ivec2 size, float metersPerPixel)
{
    controls_.setPage(pageOf(surface, size, metersPerPixel));
}

VrControls::Page App::pageOf(gui::Surface& surface, glm::ivec2 size, float metersPerPixel)
{
    VrControls::Page page;
    page.size = glm::vec2(size);
    page.metersPerPixel = metersPerPixel;
    page.image = surface.texture();
    page.point = [&surface](std::optional<glm::vec2> pixel)
    {
        if(pixel)
        {
            surface.pointerMove(*pixel);
        }
        else
        {
            surface.pointerLeave();
        }
    };
    page.button = [&surface](bool down) { surface.pointerButton(0, down); };
    page.scroll = [&surface](glm::vec2 lines) { surface.scroll(lines); };
    page.cursor = [&surface] { return surface.cursor(); };
    return page;
}

void App::placeBoards()
{
    // The news and grid status standing at their markers, facing as they
    // do, while the home scene shows.
    const bool wanted = boards_ && homeShown() && home_->news && home_->gridStatus;
    if(wanted == boardsPlaced_ && !boardsMoved_)
    {
        return;
    }
    boardsPlaced_ = wanted;
    boardsMoved_ = false;
    std::vector<VrControls::StandingPage> pages;
    if(wanted)
    {
        auto standing = [&](gui::Surface& surface, const world::HomeMarker& m)
        {
            return VrControls::StandingPage{
                pageOf(surface, LobbyBoards::kSize, LobbyBoards::kMetersPerPixel), m.position,
                m.position + markerForward(m.rotation)};
        };
        pages.push_back(standing(boards_->newsSurface(), *home_->news));
        pages.push_back(standing(boards_->statusSurface(), *home_->gridStatus));
    }
    controls_.setStandingPages(std::move(pages));
}

void App::updateLobby()
{
    const bool lobby = loginScreenShown();
    if(lobby == lobby_)
    {
        return;
    }
    lobby_ = lobby;
    camera_.setWalking(lobby);
    if(lobby)
    {
        // Standing at the start, at eye height, looking at the panel.
        camera_.lookAt(lobbyStart(), 1.0f, lobbyYaw(), 8.0f);
        const auto [panel, facing] = loginPanelPlace();
        log::info("lobby: eyes at {:.2f} {:.2f} {:.2f} facing {:.0f} degrees; login panel at "
                  "{:.2f} {:.2f} {:.2f} facing {:.2f} {:.2f}",
                  lobbyStart().x, lobbyStart().y, lobbyStart().z, lobbyYaw(), panel.x, panel.y,
                  panel.z, facing.x - panel.x, facing.y - panel.y);
        lobbyTicks_ = SDL_GetTicksNS();
    }
}

void App::loadHomeScene()
{
    home_.reset();
    homeWorld_.reset();
    std::ifstream in(homeScenePath_, std::ios::binary);
    if(!in)
    {
        log::info("no home scene at {}: the lobby is the test scene", homeScenePath_.string());
        return;
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
    auto scene = world::loadHomeScene(bytes, kHomeRegion);
    if(!scene)
    {
        log::error("the home scene {} couldn't be read: {}", homeScenePath_.string(),
                   scene.error());
        return;
    }
    auto world = std::make_shared<world::World>();
    for(world::WorldUpdate& update : scene->updates)
    {
        world->apply(std::move(update));
    }
    scene->updates.clear();
    world->standAlone();
    for(auto& [id, material] : scene->legacyMaterials)
    {
        legacyMaterials_->put(id, material);
    }
    materials_->put(world::kHomeBlankMaterial, assets::Material{});
    log::info("home scene {}: {} objects, assets from {}", homeScenePath_.string(),
              world->objectCount(), scene->assetUrl);
    home_ = std::move(*scene);
    homeWorld_ = std::move(world);
}

glm::vec3 App::lobbyStart() const
{
    if(!home_ || (!home_->landing && !home_->floor))
    {
        return kLobbyStart;
    }
    const glm::vec3 at = home_->landing ? home_->landing->position : glm::vec3(128.0f);
    const float floor = home_->floor.value_or(at.z);
    return {at.x, at.y, floor + kLobbyEyeHeight};
}

float App::lobbyYaw() const
{
    if(!home_ || !home_->landing)
    {
        return 90.0f;
    }
    const glm::vec3 forward = markerForward(home_->landing->rotation);
    return glm::degrees(std::atan2(forward.y, forward.x));
}

std::pair<glm::vec3, glm::vec3> App::loginPanelPlace() const
{
    if(home_ && home_->login)
    {
        const world::HomeMarker& m = *home_->login;
        return {m.position, m.position + markerForward(m.rotation)};
    }
    const glm::vec3 start = lobbyStart();
    const float yaw = glm::radians(lobbyYaw());
    const glm::vec3 ahead(std::cos(yaw), std::sin(yaw), 0.0f);
    return {start + ahead * 1.4f + glm::vec3(0.0f, 0.0f, -0.3f), start};
}

void App::setViewerAssetUrl(const std::string& url)
{
    textures_->setViewerAssetUrl(url);
    meshes_->setViewerAssetUrl(url);
    materials_->setViewerAssetUrl(url);
    if(animations_)
    {
        animations_->setViewerAssetUrl(url);
    }
}

void App::walkLobby(const VrControls::Input& input, const glm::vec3& headForward)
{
    const uint64_t now = SDL_GetTicksNS();
    const float seconds = std::min(static_cast<float>(now - lobbyTicks_) * 1e-9f, 0.1f);
    lobbyTicks_ = now;
    glm::vec3 forward(headForward.x, headForward.y, 0.0f);
    if(glm::length(forward) < 1e-3f)
    {
        return;
    }
    forward = glm::normalize(forward);
    const glm::vec3 right = glm::cross(forward, glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::vec2 walk = input.pointers[0].stick;
    if(glm::length(walk) > 0.15f)
    {
        camera_.shift((forward * walk.y + right * walk.x) * 1.6f * seconds);
    }
    const float turn = input.pointers[1].stick.x;
    if(!lobbyTurned_ && std::abs(turn) > 0.7f)
    {
        camera_.turn(turn > 0.0f ? -30.0f : 30.0f);
        lobbyTurned_ = true;
    }
    else if(std::abs(turn) < 0.3f)
    {
        lobbyTurned_ = false;
    }
}

bool App::loginScreenShown() const
{
    return loginScreen_ && LoginScreen::wanted(session_.get());
}

bool App::loginScreenEvent(const SDL_Event& event)
{
    if(!loginScreenShown())
    {
        return false;
    }
    if(event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
       event.key.scancode == SDL_SCANCODE_F5 && !ImGui::GetIO().WantCaptureKeyboard)
    {
        gui_->forgetStyles();
        loginScreen_->reload();
        if(boards_)
        {
            boards_->reload();
        }
        log::info("the login screen's documents read again");
        // And the home scene (after converting it again), the panels placed
        // anew from its markers.
        loadHomeScene();
        pageIsLogin_ = false;
        boardsMoved_ = true;
        return true;
    }
    // The wheel over one of the lobby's pages (the mouse is the right
    // pointer) scrolls it; the wheel turned away from us scrolls up.
    if(event.type == SDL_EVENT_MOUSE_WHEEL &&
       controls_.scrollPointed(1, glm::vec2(event.wheel.x, -event.wheel.y)))
    {
        return true;
    }
    // Typing into a field on its panel (pointing at it is VrControls').
    gui::Surface& surface = loginScreen_->surface();
    if(surface.typing() && !ImGui::GetIO().WantCaptureKeyboard)
    {
        if(event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP)
        {
            surface.key(event.key.key, event.key.mod, event.type == SDL_EVENT_KEY_DOWN);
            return true;
        }
        if(event.type == SDL_EVENT_TEXT_INPUT)
        {
            surface.text(event.text.text);
            return true;
        }
    }
    return false;
}

bool App::mainMenuEvent(const SDL_Event& event)
{
    if(!mainMenu_ || loginScreenShown())
    {
        return false;
    }
    const bool keyboardFree = !ImGui::GetIO().WantCaptureKeyboard;
    if(event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && keyboardFree)
    {
        // Escape opens and closes it (as other worlds' main menus); F5
        // reads its documents again (after editing them: --ui-dir).
        if(event.key.scancode == SDL_SCANCODE_ESCAPE)
        {
            mainMenu_->toggle();
            return true;
        }
        if(event.key.scancode == SDL_SCANCODE_F5)
        {
            gui_->forgetStyles();
            mainMenu_->reload();
            if(dialogPage_)
            {
                dialogPage_->reload();
            }
            log::info("the interface's documents read again");
            return true;
        }
    }
    if(!mainMenuOverWindow())
    {
        return false;
    }
    return pageEvent(mainMenu_->surface(), mainMenuRect(), MainMenu::kSize, mainMenuPressed_,
                     event);
}

bool App::dialogOverWindow() const
{
    return dialogPage_ && noticeWaiting() && !camera_.firstPerson() &&
           !(headset_ && headset_->running()) && !loginScreenShown();
}

glm::vec4 App::dialogRect() const
{
    // The window's top right: at the display's scale, or smaller to fit.
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window_.get(), &width, &height);
    const glm::vec2 window(static_cast<float>(width), static_cast<float>(height));
    const glm::vec2 size(ScriptDialogPage::kSize);
    const float scale = std::min({SDL_GetWindowDisplayScale(window_.get()),
                                  window.x * 0.45f / size.x, window.y * 0.7f / size.y});
    const glm::vec2 fitted = glm::floor(size * scale);
    const float margin = std::floor(16.0f * scale);
    return glm::vec4(window.x - fitted.x - margin, margin, fitted);
}

gui::Surface* App::panelTyping() const
{
    // Pages on the VR controls' panels (first person, a headset), the
    // window's keyboard typing into one's text field.
    if(dialogPage_ && controls_.noticeOpen() && dialogPage_->surface().typing())
    {
        return &dialogPage_->surface();
    }
    if(mainMenu_ && controls_.pageOpen() && !pageIsLogin_ && mainMenu_->surface().typing())
    {
        return &mainMenu_->surface();
    }
    return nullptr;
}

bool App::typingOnPage() const
{
    return (lobby_ && loginScreen_ && loginScreen_->surface().typing()) ||
           (mainMenu_ && mainMenu_->isOpen() && mainMenu_->surface().typing()) ||
           (dialogPage_ && noticeWaiting() && dialogPage_->surface().typing());
}

bool App::panelTypingEvent(const SDL_Event& event)
{
    gui::Surface* page = panelTyping();
    if(!page || ImGui::GetIO().WantCaptureKeyboard)
    {
        return false;
    }
    // Escape still closes the menu (mainMenuEvent).
    if((event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) &&
       event.key.scancode != SDL_SCANCODE_ESCAPE)
    {
        page->key(event.key.key, event.key.mod, event.type == SDL_EVENT_KEY_DOWN);
        return true;
    }
    if(event.type == SDL_EVENT_TEXT_INPUT)
    {
        page->text(event.text.text);
        return true;
    }
    return false;
}

bool App::scriptDialogEvent(const SDL_Event& event)
{
    if(!dialogOverWindow())
    {
        dialogPressed_ = false;
        return false;
    }
    return pageEvent(dialogPage_->surface(), dialogRect(), ScriptDialogPage::kSize, dialogPressed_,
                     event);
}

void App::sayIn(const std::string& conversation, std::string_view typed)
{
    const auto* login = session_ ? session_->login() : nullptr;
    const auto session = Uuid::parse(conversation);
    if(conversation == ChatView::kNearby || !session || !login)
    {
        say(typed);
        return;
    }
    const std::string name =
        login->name.last.empty() ? login->name.first : login->name.first + " " + login->name.last;
    if(const auto out =
           ims_->send(*session, typed, login->agentId, name, std::chrono::system_clock::now()))
    {
        session_->sendInstantMessage(out->to, out->session, out->dialog, out->text);
    }
}

void App::objectMessage(const InstantMessages::FromObject& m)
{
    localChat_.handle(ChatSource::Object, ChatType::Owner, 1, m.object, m.owner, m.name, m.text,
                      nullptr, LocalChat::Clock::now());
}

void App::say(std::string_view typed)
{
    // "/5 text": on channel 5 (as the official chat bar).
    if(const auto input = parseChatInput(typed); input && session_)
    {
        session_->say(input->text, input->channel);
    }
}

bool App::noticeWaiting() const
{
    return scriptDialogs_.front() || (offers_ && offers_->front());
}

void App::updateScriptDialogs()
{
    updateOfferAnswers();
    if(!dialogPage_)
    {
        return;
    }
    const bool headset = headset_ && headset_->running();
    const bool shown = noticeWaiting() && !loginScreenShown();
    const bool onPanel = shown && (camera_.firstPerson() || headset);
    if(shown)
    {
        dialogPage_->update(scriptDialogs_, *offers_, headset, headset,
                            camera_.firstPerson() && !headset && !cursorFree());
    }
    if(onPanel != controls_.noticeOpen())
    {
        onPanel ? controls_.openNotice() : controls_.closeNotice();
    }

    const auto action = dialogPage_->takeAction();
    if(action && action->offer)
    {
        // An offer's answer (if it's still the one shown).
        const Offer* offer = offers_->front();
        if(!scriptDialogs_.front() && offer && offer->serial == action->serial)
        {
            action->kind == ScriptDialogPage::Action::Kind::Accept ? offers_->accept()
                                                                   : offers_->decline();
            updateOfferAnswers();
        }
        return;
    }

    // What was clicked: the answer to the script.
    const ScriptNotice* notice = scriptDialogs_.front();
    if(!action || !notice || notice->serial != action->serial)
    {
        return; // nothing, or about one that's gone
    }
    auto sendReply = [&](const std::optional<ScriptDialogs::DialogReply>& r)
    {
        if(r && session_)
        {
            session_->replyToScriptDialog(r->region, r->object, r->channel, r->index, r->label);
        }
    };
    auto sendAnswer = [&](const std::optional<ScriptDialogs::PermissionReply>& r)
    {
        if(r && session_)
        {
            session_->answerScriptPermissions(r->region, r->task, r->item, r->questions);
        }
    };
    using Kind = ScriptDialogPage::Action::Kind;
    switch(action->kind)
    {
    case Kind::Choose:
        sendReply(scriptDialogs_.choose(action->button));
        break;
    case Kind::Send:
        sendReply(scriptDialogs_.reply(action->text));
        break;
    case Kind::Allow:
    case Kind::Deny:
        sendAnswer(scriptDialogs_.answer(action->kind == Kind::Allow));
        break;
    case Kind::Open:
        if(const auto url = scriptDialogs_.open())
        {
            log::info("opening {} in the web browser (offered by a script)", *url);
            if(!SDL_OpenURL(url->c_str()))
            {
                log::warn("the web browser couldn't be opened: {}", SDL_GetError());
            }
        }
        break;
    case Kind::Ignore:
        sendAnswer(scriptDialogs_.dismiss());
        break;
    case Kind::Keyboard:
        keyboardForDialog_ = action->serial;
        controls_.openKeyboard();
        break;
    case Kind::Accept:
    case Kind::Decline:
        break; // offers' (above)
    }
}

std::optional<Uuid> App::systemFolder(int type, double waited)
{
    // Asking for the root's contents (the Inventory tab does too): the
    // system folders.
    const std::optional<Uuid> root = inventoryBrowser_ ? inventoryBrowser_->root() : std::nullopt;
    if(root)
    {
        if(type >= 0)
        {
            if(const inventory::Folder* folder = inventory_->folderOfType(type))
            {
                return folder->id;
            }
        }
        return root;
    }
    if(waited < 15.0)
    {
        return std::nullopt;
    }
    const auto* login = session_ ? session_->login() : nullptr;
    return login ? login->reply["inventory-root"][0]["folder_id"].asUuid() : Uuid();
}

bool App::carryOut(const OfferAnswer& a, double waited)
{
    if(!session_ || !session_->login())
    {
        return true; // logged out: gone
    }
    // What needs a folder first, all or nothing.
    std::optional<Uuid> imFolder;
    if(a.im && a.im->folderType)
    {
        imFolder = systemFolder(*a.im->folderType, waited);
        if(!imFolder)
        {
            return false;
        }
    }
    std::optional<Uuid> callingCards;
    if((a.friendship && *a.friendship) || a.offerFriendshipTo)
    {
        callingCards = systemFolder(inventory::kCallingCardFolder, waited);
        if(!callingCards)
        {
            return false;
        }
    }
    std::optional<Uuid> trash;
    if(a.trash)
    {
        trash = systemFolder(inventory::kTrash, waited);
        if(!trash)
        {
            return false;
        }
    }

    if(a.im)
    {
        std::vector<uint8_t> bucket;
        if(imFolder)
        {
            bucket.assign(imFolder->bytes.begin(), imFolder->bytes.end());
        }
        session_->sendInstantMessage(a.im->to, a.im->id, a.im->dialog, "", bucket);
    }
    if(a.lure)
    {
        session_->teleportViaLure(*a.lure, a.godlike);
    }
    if(a.offerTeleportTo)
    {
        // "Join me in Region" and where (the region fills in the lure).
        std::string text = "Join me";
        const auto handle = session_->agentRegion();
        if(const session::Region* region = handle ? session_->regionByHandle(*handle) : nullptr;
           region && !region->name.empty())
        {
            const protocol::Vector3 p = session_->agentPosition();
            std::string escaped;
            for(const char c : region->name)
            {
                escaped += c == ' ' ? std::string("%20") : std::string(1, c);
            }
            text += std::format(" in {}\r\nhttps://maps.secondlife.com/secondlife/{}/{}/{}/{}",
                                region->name, escaped, static_cast<int>(p.x), static_cast<int>(p.y),
                                static_cast<int>(p.z));
        }
        session_->offerTeleport(*a.offerTeleportTo, text);
    }
    if(a.friendship)
    {
        session_->answerFriendship(a.transaction, *a.friendship, callingCards.value_or(Uuid()));
    }
    if(a.offerFriendshipTo)
    {
        // LLAvatarActions::requestFriendship: our calling card folder as
        // the message's id.
        session_->sendInstantMessage(*a.offerFriendshipTo, callingCards.value_or(Uuid()),
                                     ImDialog::FriendshipOffered, "Would you be my friend?");
    }
    if(a.trash && trash && *trash != Uuid())
    {
        session_->moveInventoryItem(*a.trash, *trash);
        if(const inventory::Item* item = inventory_->item(*a.trash))
        {
            inventory::Item moved = *item;
            moved.parent = *trash;
            inventory_->put(std::move(moved));
        }
    }
    if(a.url)
    {
        log::info("opening {} in the web browser (offered by the grid)", *a.url);
        if(!SDL_OpenURL(a.url->c_str()))
        {
            log::warn("the web browser couldn't be opened: {}", SDL_GetError());
        }
    }
    return true;
}

void App::updateOfferAnswers()
{
    if(!offers_)
    {
        return;
    }
    const double now = static_cast<double>(SDL_GetTicksNS()) * 1e-9;
    for(OfferAnswer& a : offers_->takeAnswers())
    {
        offerAnswers_.push_back({std::move(a), now});
    }
    // Friends: the login's list once, then friendships formed.
    if(const auto* login = session_ ? session_->login() : nullptr; login && !friendsLoaded_)
    {
        friends_.load(login->reply["buddy-list"]);
        friendsLoaded_ = true;
        log::info("{} friends", friends_.all().size());
    }
    for(const Uuid& id : offers_->takeFriendships())
    {
        friends_.form(id);
    }
    std::erase_if(offerAnswers_,
                  [&](const PendingAnswer& p) { return carryOut(p.answer, now - p.since); });
}

void App::updateSounds(const render::Scene& scene)
{
    soundAssets_->update();
    if(!world_ || !originHandle_)
    {
        return;
    }
    const auto locate = [&](const world::ObjectKey& key) -> std::optional<SoundManager::Source>
    {
        const world::Object* o = world_->object(key.region, key.localId);
        if(!o)
        {
            return std::nullopt;
        }
        SoundManager::Source s;
        s.object = o->id;
        s.sound = o->sound;
        s.events = o->soundEvents;
        s.hud = objects_->onHud(*o, *world_, avatars_.get());
        if(!s.hud)
        {
            const auto frame = objects_->objectFrame(*o, *world_, *originHandle_, avatars_.get());
            if(!frame)
            {
                return std::nullopt; // its parent isn't here yet
            }
            s.position = glm::vec3((*frame)[3]);
        }
        return s;
    };
    const render::Camera& c = scene.camera;
    sounds_->update(objects_->soundSources(), locate, c.position, c.forward(), c.up);

    const uint64_t origin = *originHandle_;
    const glm::dvec3 corner(static_cast<double>(origin >> 32),
                            static_cast<double>(origin & 0xFFFFFFFFu), 0.0);
    // A scene position's region and where in it.
    auto regionAt = [&](const glm::vec3& p)
    {
        const glm::dvec3 g = corner + glm::dvec3(p);
        constexpr double kWidth = world::kRegionWidth;
        const double rx = std::floor(g.x / kWidth) * kWidth;
        const double ry = std::floor(g.y / kWidth) * kWidth;
        return std::pair(static_cast<uint64_t>(rx) << 32 | static_cast<uint64_t>(ry),
                         glm::vec2(static_cast<float>(g.x - rx), static_cast<float>(g.y - ry)));
    };

    // Footsteps: on the highest of the land and the objects under a foot.
    if(avatars_)
    {
        const AvatarView::GroundBelow ground = [&](const glm::vec3& top,
                                                   float depth) -> std::optional<AvatarView::Ground>
        {
            std::optional<AvatarView::Ground> best;
            const auto [handle, local] = regionAt(top);
            if(const world::RegionTerrain* terrain = world_->terrain(handle))
            {
                const float land = terrain->heightAt(local.x, local.y);
                if(land <= top.z && land >= top.z - depth)
                {
                    best = AvatarView::Ground{land, std::nullopt};
                }
            }
            if(const auto hit = objects_->groundBelow(corner + glm::dvec3(top), depth))
            {
                const auto height = static_cast<float>(hit->height);
                if(!best || height > best->height)
                {
                    const world::Object* o = world_->object(hit->prim.region, hit->prim.localId);
                    best = AvatarView::Ground{height, static_cast<uint8_t>(o ? o->material : 0)};
                }
            }
            return best;
        };
        for(const AvatarView::Footstep& step : avatars_->footsteps(*world_, origin, ground))
        {
            sounds_->footstep(step.position, step.material);
        }
    }

    // Wind: the region's where we are, less our own motion.
    const uint64_t ticks = SDL_GetTicksNS();
    const float seconds = lastSoundTicks_
                              ? std::min(static_cast<float>(ticks - lastSoundTicks_) * 1e-9f, 0.25f)
                              : 0.0f;
    lastSoundTicks_ = ticks;
    glm::vec3 weather(0.0f);
    glm::vec3 velocity(0.0f);
    if(agentKey_)
    {
        if(const world::Object* me = world_->object(agentKey_->region, agentKey_->localId))
        {
            velocity = me->velocity;
            if(const world::WindField* wind = world_->wind(agentKey_->region);
               wind && me->parentId == 0)
            {
                weather = world::windVelocity(*wind, glm::vec2(me->position),
                                              static_cast<float>(world::kRegionWidth));
            }
        }
    }
    sounds_->updateWind(weather, velocity, agent_.flying(), seconds);
}

void App::peopleAction(const PeopleView::Action& action)
{
    const auto* login = session_->login();
    if(!login)
    {
        return;
    }
    using Kind = PeopleView::Action::Kind;
    switch(action.kind)
    {
    case Kind::Message:
    {
        const Uuid session =
            ims_->start(login->agentId, action.who, action.name, std::chrono::system_clock::now());
        mainMenu_->chat().open(session.toString());
        mainMenu_->showTab("chat");
        break;
    }
    case Kind::OfferTeleport:
    {
        log::info("offering '{}' a teleport", action.name);
        OfferAnswer a;
        a.offerTeleportTo = action.who;
        offerAnswers_.push_back({std::move(a), static_cast<double>(SDL_GetTicksNS()) * 1e-9});
        break;
    }
    case Kind::RequestTeleport:
        // LLAvatarActions::teleportRequest: IM_TELEPORT_REQUEST, no id.
        log::info("asking '{}' for a teleport", action.name);
        session_->sendInstantMessage(action.who, Uuid(), ImDialog::TeleportRequest,
                                     "Can I teleport to you?");
        break;
    case Kind::OfferFriendship:
    {
        log::info("offering '{}' friendship", action.name);
        OfferAnswer a;
        a.offerFriendshipTo = action.who;
        offerAnswers_.push_back({std::move(a), static_cast<double>(SDL_GetTicksNS()) * 1e-9});
        break;
    }
    case Kind::EndFriendship:
    {
        log::info("ending the friendship with '{}'", action.name);
        session_->endFriendship(action.who);
        friends_.remove(action.who);
        break;
    }
    case Kind::SetRights:
        session_->grantRights(action.who, action.rights);
        friends_.setRightsGiven(action.who, action.rights);
        break;
    }
}

bool App::pageEvent(gui::Surface& surface, const glm::vec4& rect, glm::ivec2 size, bool& pressed,
                    const SDL_Event& event)
{
    const bool keyboardFree = !ImGui::GetIO().WantCaptureKeyboard;
    // Typing into a field on it.
    if(surface.typing() && keyboardFree)
    {
        if(event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP)
        {
            surface.key(event.key.key, event.key.mod, event.type == SDL_EVENT_KEY_DOWN);
            return true;
        }
        if(event.type == SDL_EVENT_TEXT_INPUT)
        {
            surface.text(event.text.text);
            return true;
        }
    }
    // The mouse: window coordinates to the page's pixels.
    const float density = SDL_GetWindowPixelDensity(window_.get());
    auto toPage = [&](float x, float y, bool& inside)
    {
        const glm::vec2 pixel = glm::vec2(x, y) * density;
        const glm::vec2 local = (pixel - glm::vec2(rect.x, rect.y)) / glm::vec2(rect.z, rect.w);
        inside = local.x >= 0.0f && local.y >= 0.0f && local.x < 1.0f && local.y < 1.0f;
        return local * glm::vec2(size);
    };
    bool inside = false;
    switch(event.type)
    {
    case SDL_EVENT_MOUSE_MOTION:
    {
        const glm::vec2 at = toPage(event.motion.x, event.motion.y, inside);
        if(inside || pressed)
        {
            surface.pointerMove(at);
        }
        else
        {
            surface.pointerLeave();
        }
        return inside || pressed;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    {
        const glm::vec2 at = toPage(event.button.x, event.button.y, inside);
        if(!inside)
        {
            return false;
        }
        surface.pointerMove(at);
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT ? 1
                                                                     : 2;
        surface.pointerButton(button, true);
        pressed = true;
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
    {
        if(!pressed)
        {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT ? 1
                                                                     : 2;
        surface.pointerButton(button, false);
        pressed = (SDL_GetMouseState(nullptr, nullptr) &
                   (SDL_BUTTON_LMASK | SDL_BUTTON_RMASK | SDL_BUTTON_MMASK)) != 0;
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
    {
        toPage(event.wheel.mouse_x, event.wheel.mouse_y, inside);
        if(!inside)
        {
            return false;
        }
        // The wheel turned away from us scrolls up.
        surface.scroll(glm::vec2(event.wheel.x, -event.wheel.y));
        return true;
    }
    default:
        return false;
    }
}

namespace
{

// The places the resident wants to see ("g", "gm" or "gma"), from the
// login's agent_region_access (PG, M or A: the preference), else
// agent_access_max (what they may see).
std::string maturityPreference(const session::LoginSuccess& login)
{
    std::string access = login.reply["agent_region_access"].asString();
    if(access.empty())
    {
        access = login.reply["agent_access_max"].asString();
    }
    return access.starts_with('A') ? "gma" : access.starts_with('M') ? "gm" : "g";
}

} // namespace

void App::updateMainMenu()
{
    if(!mainMenu_ || !mainMenu_->isOpen())
            const AvatarName* own = avatarNames_->get(login->agentId);
            status.displayName = own && !own->displayName.empty() ? own->displayName
                                 : login->name.last.empty() || login->name.last == "Resident"
                                     ? login->name.first
                                     : login->name.first + " " + login->name.last;
            status.picture = profilePicture_;
            status.balance = session_->balance();
    {
        return;
    }
    MainMenu::Status status;
    status.viewerFps = ImGui::GetIO().Framerate;
    if(session_)
    {
        if(const session::Region* main = session_->mainRegion())
        {
            status.region = main->name;
            if(main->stats)
            {
                status.regionFps = main->stats->fps;
                status.timeDilation = main->stats->timeDilation;
            }
            const protocol::Vector3 at = session_->agentPosition();
            status.position = std::format("{:.0f}, {:.0f}, {:.0f}", static_cast<double>(at.x),
                                          static_cast<double>(at.y), static_cast<double>(at.z));
        }
        if(const session::LoginSuccess* login = session_->login())
        {
            status.name = login->name.first;
            status.maturity = maturityPreference(*login);
        }
        status.inWorld = session_->state() == session::SessionState::Connected;
        // The parcel we're on, and the names of who has it.
        const auto avatar = [&](const Uuid& id)
        {
            const AvatarName* name = id.isNull() ? nullptr : avatarNames_->get(id);
            return name ? name->completeName() : std::string();
        };
        if(const auto& parcel = parcel_.parcel())
        {
            status.parcel = &*parcel;
            status.parcelOwner =
                parcel->groupOwned ? parcel_.group(parcel->owner) : avatar(parcel->owner);
            status.parcelGroup = parcel_.group(parcel->group);
        }
        if(const auto& estate = parcel_.estate())
        {
            status.estate = &*estate;
            status.estateOwner = avatar(estate->owner);
        }
        status.worn = &worn_;
        status.changing = [this](const Uuid& item) { return currentOutfit_->pending(item); };
        status.wearing = [this](const Uuid& item) { return currentOutfit_->wearing(item); };
        status.baseOutfit = currentOutfit_->baseOutfit();
        status.outfitChanging = currentOutfit_->outfitPending() || !outfitAttachQueue_.empty();
    }
    destinations_->update();
    status.chat = &localChat_;
    status.ims = ims_.get();
    if(const auto* login = session_ ? session_->login() : nullptr)
    {
        status.self = login->agentId;
    }
    status.headset = headset_ && headset_->running();
    status.friends = &friends_;
    status.nearby = &nearby_;
    status.names = [this](const Uuid& id) { return avatarNames_->get(id); };
    if(session_ && session_->state() == session::SessionState::Connected)
    {
        if(const auto handle = session_->agentRegion())
        {
            const protocol::Vector3 p = session_->agentPosition();
            status.here = globalPosition(*handle, glm::vec3(p.x, p.y, p.z));
        }
    }
    mainMenu_->update(status);
    if(const auto action = mainMenu_->people().takeAction(); action && session_)
    {
        peopleAction(*action);
    }
    // Attaching from inventory, or taking off: the COF follows once the
    // simulator has done it (CurrentOutfit).
    if(const auto action = mainMenu_->takeInventoryAction(); action && session_)
    {
        const double now = static_cast<double>(SDL_GetTicksNS()) * 1e-9;
        const inventory::Item* item = inventory_->item(action->item);
        if(action->kind == MainMenu::InventoryAction::Kind::Attach && item &&
           item->type == inventory::kObject)
        {
            log::info("attaching '{}' from inventory", item->name);
            const OutfitAttachments::Item attach = OutfitAttachments::toAttach(*item);
            session_->attachFromInventory(std::span(&attach, 1));
            currentOutfit_->attaching(item->id, now);
        }
        else if((action->kind == MainMenu::InventoryAction::Kind::Wear ||
                 action->kind == MainMenu::InventoryAction::Kind::Add) &&
                item && CurrentOutfit::isWearable(*item))
        {
            log::info("wearing '{}'", item->name);
            currentOutfit_->wear(item->id, action->kind == MainMenu::InventoryAction::Kind::Add
                                               ? CurrentOutfit::How::Add
                                               : CurrentOutfit::How::Replace);
        }
        else if(action->kind == MainMenu::InventoryAction::Kind::WearOutfit)
        {
            // Only once what's in it is known (or everything would come off).
            const inventory::Folder* folder = inventory_->folder(action->item);
            if(folder &&
               inventoryBrowser_->openLinks(folder->id) == InventoryBrowser::State::Loaded)
            {
                log::info("wearing the outfit '{}'", folder->name);
                currentOutfit_->wearOutfit(folder->id);
            }
        }
        else if(action->kind == MainMenu::InventoryAction::Kind::TakeOff && item)
        {
            log::info("taking off '{}'", item->name);
            currentOutfit_->takeOff(item->id);
        }
        else if(action->kind == MainMenu::InventoryAction::Kind::Detach)
        {
            if(const auto objects = wornObjects_.find(action->item); objects != wornObjects_.end())
            {
                log::info("taking off '{}'", item ? item->name : action->item.toString());
                session_->detach(objects->second);
                currentOutfit_->detaching(action->item, now);
                outfit_->forget(action->item); // not put back by a later outfit pass
            }
        }
    }
    // Chat typed on the Chat tab, or the VR keyboard asked for to type it.
    if(const auto said = mainMenu_->chat().takeSay(); said && session_)
    {
        sayIn(said->conversation, said->text);
    }
    if(const auto into = mainMenu_->chat().takeKeyboard())
    {
        keyboardForDialog_.reset();
        keyboardForChat_ = *into;
        controls_.openKeyboard();
    }
    if(mainMenu_->takeLogOut() && session_)
    {
        mainMenu_->setOpen(false);
        handleLoginAction({LoginAction::Kind::LogOut, {}});
    }
    if(const auto where = mainMenu_->takeTeleport(); where && session_)
    {
        if(const auto location = session::parseLocation(*where))
        {
            log::info("teleporting to {} (from Destinations)", location->region);
            session_->teleportTo(*location);
            mainMenu_->setOpen(false);
        }
    }
}

bool App::cursorFree() const
{
    if(!camera_.firstPerson() || (headset_ && headset_->running()))
    {
        return false;
    }
    const bool* keys = SDL_GetKeyboardState(nullptr);
    const bool tab = keys[SDL_SCANCODE_TAB] && !ImGui::GetIO().WantCaptureKeyboard;
    return tab || controls_.menuPointer().has_value() || controls_.pageOpen();
}

bool App::sitting() const
{
    if(!world_ || !agentKey_)
    {
        return false;
    }
    const world::Object* me = world_->object(agentKey_->region, agentKey_->localId);
    return me && me->parentId != 0;
}

void App::updateControls(render::Scene& scene)
{
    VrControls::Input input;
    input.eye = scene.camera.position;
    input.forward = glm::normalize(scene.camera.target - scene.camera.position);
    // In the lobby the panels stand in the world: a fixed anchor.
    input.anchor = lobby_ ? glm::vec3(0.0f) : vr_.placed ? vr_.base : scene.camera.position;
    input.now = static_cast<double>(SDL_GetTicks()) / 1000.0;
    vrAim_.reset();
    const bool headset = vr_.placed && headsetFrame_ && headsetFrame_->render;
    if(headset)
    {
        // The controllers: their aim rays, triggers (pressed past most of
        // the way, let go past half), menu buttons (B, Y; a Vive's menu
        // button) and sticks; the menu opens at the hand and follows it.
        for(size_t h = 0; h < 2; h++)
        {
            const xr::HeadsetFrame::Hand& hand = headsetFrame_->hands[h];
            VrControls::Pointer& p = input.pointers[h];
            if(!hand.tracked)
            {
                vrTrigger_[h] = false;
                continue;
            }
            const xr::Pose aim = vrToScene(hand.aim);
            p.active = true;
            p.origin = aim.position;
            p.direction = glm::normalize(aim.orientation * glm::vec3(1.0f, 0.0f, 0.0f));
            vrTrigger_[h] = hand.trigger > (vrTrigger_[h] ? 0.4f : 0.7f);
            p.button = vrTrigger_[h];
            p.menu = hand.secondary;
            p.stick = hand.stick;
            p.menuAt = vrToScene(hand.grip).position + glm::vec3(0.0f, 0.0f, 0.12f);
        }
        if(input.pointers[1].active)
        {
            vrAim_ = PointerController::Ray{input.pointers[1].origin, input.pointers[1].direction,
                                            std::nullopt};
        }
        if(lobby_)
        {
            walkLobby(input, input.forward);
        }
    }
    else if(camera_.firstPerson() || lobby_)
    {
        // The mouse: through the center dot, or the cursor while it's free
        // (always, in the lobby); the left button presses, the right one
        // (held) opens the menu (in the lobby it looks around instead).
        int width = 0;
        int height = 0;
        SDL_GetWindowSize(window_.get(), &width, &height);
        float x = 0.0f;
        float y = 0.0f;
        SDL_MouseButtonFlags buttons = SDL_GetMouseState(&x, &y);
        if(!cursorFree() && !lobby_)
        {
            x = static_cast<float>(width) * 0.5f;
            y = static_cast<float>(height) * 0.5f;
        }
        // --pointer (and --click-at's press): the mouse's stand-in, for
        // trying the pages without one.
        if(pointerOverride_)
        {
            x = (*pointerOverride_)[0] * static_cast<float>(width);
            y = (*pointerOverride_)[1] * static_cast<float>(height);
            const bool clicking =
                clickedTicks_ && SDL_GetTicksNS() - clickedTicks_ <= 200'000'000ull;
            buttons = clicking ? SDL_BUTTON_LMASK : 0;
        }
        const bool overUi = (cursorFree() || lobby_) && ImGui::GetIO().WantCaptureMouse;
        const bool focused = SDL_GetKeyboardFocus() == window_.get() || pointerOverride_;
        if(width > 0 && height > 0 && focused && !overUi)
        {
            const glm::vec2 ndc(2.0f * x / static_cast<float>(width) - 1.0f,
                                1.0f - 2.0f * y / static_cast<float>(height));
            const float aspect = static_cast<float>(width) / static_cast<float>(height);
            const glm::mat4 inverse =
                glm::inverse(scene.camera.projection(aspect) * scene.camera.view());
            const glm::vec4 far = inverse * glm::vec4(ndc, 1.0f, 1.0f);
            VrControls::Pointer& p = input.pointers[1];
            p.active = true;
            p.origin = scene.camera.position;
            p.direction = glm::normalize(glm::vec3(far) / far.w - scene.camera.position);
            p.button = (buttons & SDL_BUTTON_LMASK) != 0;
            p.menu = !lobby_ && (buttons & SDL_BUTTON_RMASK) != 0;
            p.active = !(lobby_ && camera_.dragging());
        }
    }
    controlsPointed_ = controls_.update(input);
    // The main menu on a panel while it's open in first person or VR (placed
    // from where the eyes are now); over the window otherwise.
    // The login screen instead, on the same panel, in a headset until
    // we're in the world.
    const bool loginOnPanel = lobby_;
    if(loginOnPanel != pageIsLogin_ && (loginOnPanel || mainMenu_))
    {
        pageIsLogin_ = loginOnPanel;
        if(loginOnPanel)
        {
            // Where the scene's marker says (or in front of where the
            // lobby starts, facing it), 90 cm across.
            setPanelPage(loginScreen_->surface(), LoginScreen::kSize, 0.00125f);
            const auto [center, facing] = loginPanelPlace();
            controls_.openPageAt(center, facing, glm::vec3(0.0f));
        }
        else
        {
            controls_.closePage();
            setPanelPage(mainMenu_->surface(), MainMenu::kSize);
        }
    }
    const bool menuOnPanel =
        loginOnPanel || (mainMenu_ && mainMenu_->isOpen() && !loginScreenShown() &&
                         (camera_.firstPerson() || (headset_ && headset_->running())));
    if(menuOnPanel && !controls_.pageOpen())
    {
        controls_.openPage();
    }
    else if(!menuOnPanel && controls_.pageOpen())
    {
        controls_.closePage();
    }

    // The right pointer's presses not on the controls: the world's. On the
    // desktop with the center dot, the mouselook button scripts take.
    const VrControls::Pointed& right = controlsPointed_[1];
    if(right.pressed)
    {
        if(headset || cursorFree())
        {
            primaryPress();
        }
        else if(camera_.firstPerson())
        {
            agent_.setMouselookButton(true);
            mouselookPressed_ = true;
        }
    }
    if(right.released)
    {
        if(mouselookPressed_)
        {
            agent_.setMouselookButton(false);
            mouselookPressed_ = false;
        }
        else
        {
            primaryRelease();
        }
    }
    // Held on something that isn't touched: sit on it.
    constexpr uint64_t kSitHold = 600'000'000ull;
    if(sitHold_ && SDL_GetTicksNS() - sitHold_->sinceTicks > kSitHold)
    {
        const auto& hover = pointer_.hover();
        if(hover && !hover->hud && hover->key == sitHold_->key)
        {
            sitOn(*hover);
        }
        sitHold_.reset();
    }

    std::array<std::optional<glm::vec3>, VrControls::kPointers> worldHits{};
    if(const auto& hover = pointer_.hover(); hover && !hover->hud && headset)
    {
        worldHits[1] = hover->hit.position;
    }
    controls_.addTo(scene, headset, worldHits);
}

void App::primaryPress()
{
    const auto& hover = pointer_.hover();
    if(hover && !hover->hud && hover->clickAction == world::ClickAction::Sit && !sitting())
    {
        sitOn(*hover);
        return;
    }
    pressPointer();
    if(hover && !hover->hud && !hover->touchable && !sitting())
    {
        sitHold_ = SitHold{hover->key, SDL_GetTicksNS()};
    }
}

void App::primaryRelease()
{
    sitHold_.reset();
    releasePointer();
}

void App::sitOn(const PointerController::Hover& hover)
{
    if(!session_ || !world_ || !objects_ || !originHandle_)
    {
        return;
    }
    const world::Object* object = world_->object(hover.key.region, hover.key.localId);
    if(!object || object->type != world::ObjectType::Prim)
    {
        return;
    }
    // Where on it, in its frame (as the official viewer's "Sit Here").
    glm::vec3 offset(0.0f);
    if(const auto frame = objects_->objectFrame(*object, *world_, *originHandle_, avatars_.get()))
    {
        const glm::mat3 rotation(*frame);
        offset = glm::transpose(rotation) * (hover.hit.position - glm::vec3((*frame)[3]));
    }
    log::info("sitting on {}", object->id.toString());
    session_->requestSit(object->region, object->id, {offset.x, offset.y, offset.z});
    releasePointer();
}

void App::releasePointer()
{
    if(session_)
    {
        pointer_.release(*session_);
    }
    ik_.reaching = false; // eases back to the animation
    ik_.pressed.reset();
}

void App::updateIk(const render::Scene& scene, const std::optional<PointerController::Ray>& ray)
{
    const auto* login = session_ ? session_->login() : nullptr;
    if(!avatars_ || !login)
    {
        return;
    }
    const uint64_t now = SDL_GetTicksNS();
    const float seconds =
        ik_.lastTicks == 0 ? 0.0f : std::min(static_cast<float>(now - ik_.lastTicks) * 1e-9f, 0.1f);
    ik_.lastTicks = now;
    // How far towards a value in `seconds`, by half-life.
    auto towards = [&](float halfLife) { return 1.0f - std::exp2(-seconds / halfLife); };

    if(vr_.placed && headsetFrame_ && headsetFrame_->render)
    {
        updateVrIk(scene, login->agentId);
        updateOthersIk(login->agentId, seconds);
        sendAttention(login->agentId, true, static_cast<double>(now) * 1e-9,
                      scene.camera.target - scene.camera.position);
        return;
    }
    // The head: at what the pointer's on, or along its ray; following it
    // with the official look-at's lag (HEAD_LOOKAT_LAG_HALF_LIFE).
    std::optional<glm::vec3> look;
    if(const auto& hover = pointer_.hover(); hover && !hover->hud && ray)
    {
        look = hover->hit.position;
    }
    else if(ray)
    {
        look = ray->origin + ray->direction * 10.0f;
    }
    if(look)
    {
        ik_.lookPoint =
            ik_.lookWeight <= 0.0f ? *look : glm::mix(ik_.lookPoint, *look, towards(0.15f));
    }
    ik_.lookWeight = glm::mix(ik_.lookWeight, look ? 1.0f : 0.0f, towards(0.15f));

    // The left hand (as the official editing motion, lleditingmotion.cpp):
    // reaching while pressing (following the drag: on what
    // was pressed, or along the pointer's ray as far as it was), easing
    // back after.
    if(ray)
    {
        ik_.rayOrigin = ray->origin;
    }
    if(ik_.reaching && ray)
    {
        const auto& hover = pointer_.hover();
        ik_.reachPoint = hover && !hover->hud && ik_.pressed && hover->key == *ik_.pressed
                             ? hover->hit.position
                             : ray->origin + ray->direction * ik_.pressDistance;
    }
    ik_.handWeight = glm::mix(ik_.handWeight, ik_.reaching ? 1.0f : 0.0f, towards(0.08f));

    avatar::IkGoals goals;
    if(ik_.lookWeight > 0.001f)
    {
        goals.lookAt = ik_.lookPoint;
        goals.lookAtWeight = ik_.lookWeight;
    }
    if(ik_.reachPoint && ik_.handWeight > 0.001f)
    {
        goals.leftHand = avatar::IkGoals::Effector{*ik_.reachPoint, std::nullopt, ik_.handWeight};
    }
    const bool any = goals.lookAt || goals.leftHand;
    avatars_->setIk(login->agentId, any ? std::optional(goals) : std::nullopt);
    updateOthersIk(login->agentId, seconds);
    sendAttention(login->agentId, look.has_value(), static_cast<double>(now) * 1e-9,
                  scene.camera.target - scene.camera.position);
}

void App::updateVrIk(const render::Scene& scene, const Uuid& self)
{
    const world::Object* me =
        world_ && agentKey_ ? world_->object(agentKey_->region, agentKey_->localId) : nullptr;
    if(!me)
    {
        return;
    }
    const avatar::Skeleton& skeleton = avatars_->base().skeleton;
    avatar::IkGoals goals;

    // The head where the headset is (its eyes on the headset's), the spine
    // reaching for it.
    const render::Camera& c = scene.camera;
    const glm::vec3 forward = glm::normalize(c.target - c.position);
    const glm::vec3 up = glm::normalize(c.up);
    const glm::quat head =
        glm::quat_cast(glm::mat3(forward, glm::normalize(glm::cross(up, forward)), up));
    if(const auto eyes = avatars_->eyesFromHead(*me))
    {
        goals.chains.push_back(
            {skeleton.find("mHead"), 4, {c.position - head * *eyes, head, 1.0f}});
    }

    // The hands where the controllers are: the wrist a little behind the
    // grip, the hand turned from the grip's axes (it holds along +X, palm
    // inwards) to the wrist's (fingers out along +Y on the left, -Y on the
    // right, palm down).
    constexpr float kWristBehindGrip = 0.08f;
    static const std::array<glm::quat, 2> kGripToWrist = {
        glm::quat_cast(glm::mat3(glm::vec3(0, 0, 1), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0))),
        glm::quat_cast(glm::mat3(glm::vec3(0, 0, 1), glm::vec3(-1, 0, 0), glm::vec3(0, -1, 0)))};
    const std::array<int, 2> wrists = {skeleton.find("mWristLeft"), skeleton.find("mWristRight")};
    for(size_t h = 0; h < 2; h++)
    {
        const xr::HeadsetFrame::Hand& hand = headsetFrame_->hands[h];
        if(!hand.tracked || wrists[h] < 0)
        {
            continue;
        }
        const xr::Pose grip = vrToScene(hand.grip);
        const glm::quat wrist = glm::normalize(grip.orientation * kGripToWrist[h]);
        const glm::vec3 fingers = wrist * glm::vec3(0.0f, h == 0 ? 1.0f : -1.0f, 0.0f);
        goals.chains.push_back(
            {wrists[h],
             3,
             {grip.position - fingers * (kWristBehindGrip * vr_.scale), wrist, 1.0f}});
    }
    avatars_->setIk(self, goals.chains.empty() ? std::nullopt : std::optional(goals));

    // Send motion here: (boneid, position, rotation)
}

std::optional<glm::vec3> App::attentionPoint(const ViewerEffects::Target& target, bool look)
{
    const glm::dvec3 corner(static_cast<double>(*originHandle_ >> 32),
                            static_cast<double>(*originHandle_ & 0xFFFFFFFFu), 0.0);
    const glm::vec3 offset(static_cast<float>(target.position[0]),
                           static_cast<float>(target.position[1]),
                           static_cast<float>(target.position[2]));
    if(target.object.isNull())
    {
        // Global.
        return glm::vec3(glm::dvec3(target.position[0], target.position[1], target.position[2]) -
                         corner);
    }
    // What it's at: found once, then checked.
    const world::Object* object = nullptr;
    if(const auto it = ik_.targetKeys.find(target.object); it != ik_.targetKeys.end())
    {
        object = world_->object(it->second.region, it->second.localId);
        object = object && object->id == target.object ? object : nullptr;
    }
    if(!object)
    {
        for(const auto& [key, o] : world_->objects())
        {
            if(o.id == target.object)
            {
                ik_.targetKeys[target.object] = key;
                object = &o;
                break;
            }
        }
    }
    if(!object)
    {
        return std::nullopt;
    }
    if(object->type == world::ObjectType::Avatar)
    {
        // An avatar: its head, the offset in world axes for free looks and
        // mouselook, turned by the avatar otherwise (llhudeffectlookat.cpp,
        // llhudeffectpointat.cpp calcTargetPosition).
        const auto head = avatars_->jointPosition(*object, *world_, "mHead", *originHandle_);
        if(!head)
        {
            return std::nullopt;
        }
        const bool worldAxes = look && (target.type == ViewerEffects::kLookFreeLook ||
                                        target.type == ViewerEffects::kLookMouselook);
        const glm::quat turn =
            worldAxes
                ? glm::quat(1.0f, 0.0f, 0.0f, 0.0f)
                : glm::quat_cast(glm::mat3(avatars_->rootFrame(*object, *world_, *originHandle_)));
        return *head + turn * offset;
    }
    const auto frame = objects_->objectFrame(*object, *world_, *originHandle_, avatars_.get());
    if(!frame || objects_->onHud(*object, *world_, avatars_.get()))
    {
        return std::nullopt;
    }
    return glm::vec3((*frame)[3]) + glm::quat_cast(glm::mat3(*frame)) * offset;
}

void App::updateOthersIk(const Uuid& self, float seconds)
{
    if(!world_ || !originHandle_ || !objects_ || !avatars_)
    {
        return;
    }
    auto towards = [&](float halfLife) { return 1.0f - std::exp2(-seconds / halfLife); };
    // The avatars here, by id.
    std::unordered_map<Uuid, const world::Object*> here;
    for(const world::ObjectKey& key : world_->avatarKeys())
    {
        if(const world::Object* o = world_->object(key.region, key.localId))
        {
            here[o->id] = o;
        }
    }
    const auto& received = ik_.effects.received();
    for(const auto& [id, attention] : received)
    {
        if(id != self && here.contains(id))
        {
            ik_.others.try_emplace(id);
        }
    }
    for(auto it = ik_.others.begin(); it != ik_.others.end();)
    {
        const Uuid& id = it->first;
        App::Ik::Other& o = it->second;
        const auto r = received.find(id);
        std::optional<glm::vec3> look;
        std::optional<glm::vec3> point;
        if(r != received.end() && here.contains(id))
        {
            if(r->second.look)
            {
                look = attentionPoint(*r->second.look, true);
            }
            if(r->second.point)
            {
                point = attentionPoint(*r->second.point, false);
            }
        }
        if(look)
        {
            o.lookPoint =
                o.lookWeight <= 0.0f ? *look : glm::mix(o.lookPoint, *look, towards(0.15f));
        }
        o.lookWeight = glm::mix(o.lookWeight, look ? 1.0f : 0.0f, towards(0.15f));
        if(point)
        {
            o.pointPoint =
                o.pointWeight <= 0.0f ? *point : glm::mix(o.pointPoint, *point, towards(0.08f));
        }
        o.pointWeight = glm::mix(o.pointWeight, point ? 1.0f : 0.0f, towards(0.08f));
        avatar::IkGoals goals;
        if(o.lookWeight > 0.001f)
        {
            goals.lookAt = o.lookPoint;
            goals.lookAtWeight = o.lookWeight;
        }
        if(o.pointWeight > 0.001f)
        {
            // The official editing motion points with the left arm.
            goals.leftHand = avatar::IkGoals::Effector{o.pointPoint, std::nullopt, o.pointWeight};
        }
        if(!goals.lookAt && !goals.leftHand && !look && !point)
        {
            avatars_->setIk(id, std::nullopt);
            it = ik_.others.erase(it);
            continue;
        }
        avatars_->setIk(id, goals);
        ++it;
    }
    // Avatars gone: their attention forgotten.
    std::vector<Uuid> gone;
    for(const auto& [id, attention] : received)
    {
        if(!here.contains(id))
        {
            gone.push_back(id);
        }
    }
    for(const Uuid& id : gone)
    {
        ik_.effects.forget(id);
    }
}

void App::sendAttention(const Uuid& self, bool looking, double seconds,
                        const glm::vec3& cameraForward)
{
    if(!session_ || !world_ || !originHandle_ || !objects_)
    {
        return;
    }
    const glm::dvec3 corner(static_cast<double>(*originHandle_ >> 32),
                            static_cast<double>(*originHandle_ & 0xFFFFFFFFu), 0.0);
    auto global = [&](const glm::vec3& p)
    {
        const glm::dvec3 g = corner + glm::dvec3(p);
        return std::array<double, 3>{g.x, g.y, g.z};
    };
    std::optional<ViewerEffects::Target> lookAt;
    std::optional<ViewerEffects::Target> pointAt;
    if(ik_.reaching && ik_.reachPoint)
    {
        // Pressing: pointing at it and looking at it (as lltoolgrab.cpp):
        // the point in the object's own frame, so it moves with it; for
        // anything else (an avatar), where it is.
        ViewerEffects::Target target;
        target.position = global(*ik_.reachPoint);
        const world::Object* object =
            ik_.pressed ? world_->object(ik_.pressed->region, ik_.pressed->localId) : nullptr;
        if(object && object->type == world::ObjectType::Prim)
        {
            if(const auto frame =
                   objects_->objectFrame(*object, *world_, *originHandle_, avatars_.get()))
            {
                const glm::vec3 local = glm::inverse(glm::quat_cast(glm::mat3(*frame))) *
                                        (*ik_.reachPoint - glm::vec3((*frame)[3]));
                target.object = object->id;
                target.position = {local.x, local.y, local.z};
            }
        }
        target.type = ViewerEffects::kPointGrab;
        pointAt = target;
        target.type = ViewerEffects::kLookSelect;
        lookAt = target;
    }
    else if(const world::Object* me = avatars_ && agentKey_
                                          ? world_->object(agentKey_->region, agentKey_->localId)
                                          : nullptr)
    {
        constexpr int kSkull = 2;
        const auto head = avatars_->attachmentFrame(*me, *world_, kSkull, *originHandle_);
        const glm::vec3 offset = head ? ik_.lookPoint - glm::vec3((*head)[3]) : glm::vec3(0.0f);
        if(camera_.firstPerson() && glm::length(cameraForward) > 1e-4f)
        {
            // First person: a mouselook look, the camera's direction in
            // world axes (as llagentcamera.cpp updateLookAt).
            const glm::vec3 d = glm::normalize(cameraForward);
            lookAt = ViewerEffects::Target{ViewerEffects::kLookMouselook, self, {d.x, d.y, d.z}};
        }
        else if(looking && ik_.lookWeight > 0.5f && glm::length(offset) > 0.01f)
        {
            // The cursor: a free look at ourselves, an offset from our head
            // in world axes; only its direction, 2 m long, as the official
            // one (it doesn't tell where we're looking further off).
            const glm::vec3 d = glm::normalize(offset) * 2.0f;
            lookAt = ViewerEffects::Target{ViewerEffects::kLookFreeLook, self, {d.x, d.y, d.z}};
        }
        else if(glm::length(cameraForward) > 1e-4f)
        {
            // No cursor in the view: idle, looking 2 m ahead the way the
            // camera does, an offset in our avatar's own frame (as
            // llagentcamera.cpp updateLookAt with the mouse still).
            const glm::quat body =
                glm::quat_cast(glm::mat3(avatars_->rootFrame(*me, *world_, *originHandle_)));
            const glm::vec3 d = glm::inverse(body) * (glm::normalize(cameraForward) * 2.0f);
            lookAt = ViewerEffects::Target{ViewerEffects::kLookIdle, self, {d.x, d.y, d.z}};
        }
    }
    ik_.effects.update([&](const session::Session::AttentionEffect& e)
                       { session_->sendAttention(e); }, lookAt, pointAt, seconds);
}

void App::placeHud(render::Scene& scene)
{
    render::Hud& hud = scene.hud;
    hud.placement = render::Hud::Placement::World;
    // A panel's frame: faced from -X, +Y left, +Z up (as HUD space).
    if(hudPlacement_ == HudPlacement::Forearm && avatars_ && agentKey_)
    {
        const world::Object* self = world_->object(agentKey_->region, agentKey_->localId);
        constexpr int kLeftForearm = 21; // "L Forearm", on mElbowLeft
        const auto forearm =
            self ? avatars_->attachmentFrame(*self, *world_, kLeftForearm, *originHandle_)
                 : std::nullopt;
        if(forearm)
        {
            // On top of the forearm, facing up, like a watch.
            hud.panel = *forearm * glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.06f, 0.06f)) *
                        glm::rotate(glm::mat4(1.0f), glm::half_pi<float>(), glm::vec3(0, 1, 0));
            hud.panelHeight = 0.2f;
            return;
        }
    }
    if(hudPlacement_ != HudPlacement::Screen)
    {
        // In front of the camera, facing it (where a headset would show it).
        const render::Camera& camera = scene.camera;
        const glm::vec3 forward = glm::normalize(camera.target - camera.position);
        const glm::vec3 left = glm::normalize(glm::cross(camera.up, forward));
        const glm::vec3 up = glm::cross(forward, left);
        hud.panel = glm::mat4(glm::vec4(forward, 0.0f), glm::vec4(left, 0.0f), glm::vec4(up, 0.0f),
                              glm::vec4(camera.position + forward, 1.0f));
        hud.panelHeight = 0.5f;
        return;
    }
    hud.placement = render::Hud::Placement::Screen;
}

void App::logOutBeforeExit()
{
    if(!session_)
    {
        return;
    }
    // Tell the region we're leaving, so the avatar doesn't linger until the
    // circuit times out. update() gives up on its own after a few seconds.
    session_->requestLogout();
    while(session_ && session_->state() == session::SessionState::LoggingOut)
    {
        SDL_Delay(10);
        updateSession();
    }
    endSession();
}

// Top-right corner: frames per second and frame times over the last second
// (average and worst), with a graph of recent frames.
void App::drawFpsOverlay()
{
    if(frameTimes_.empty())
    {
        return;
    }
    float total = 0.0f;
    float worst = 0.0f;
    size_t count = 0;
    for(auto it = frameTimes_.rbegin(); it != frameTimes_.rend() && total < 1000.0f; ++it)
    {
        total += *it;
        worst = std::max(worst, *it);
        count++;
    }
    const float average = total / static_cast<float>(count);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    constexpr float kMargin = 10.0f;
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - kMargin, viewport->WorkPos.y + kMargin),
        ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::SetNextWindowBgAlpha(0.5f);
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if(ImGui::Begin("##fps", nullptr, kFlags))
    {
        ImGui::Text("%.0f FPS", static_cast<double>(average > 0.0f ? 1000.0f / average : 0.0f));
        ImGui::Text("%.2f ms avg, %.2f ms worst", static_cast<double>(average),
                    static_cast<double>(worst));
        ImGui::PlotLines("##frames", frameTimes_.data(), static_cast<int>(frameTimes_.size()), 0,
                         nullptr, 0.0f, std::max(33.3f, worst), ImVec2(200.0f, 40.0f));
        // Where the main thread's time goes (averages, stages over 0.05 ms).
        const FrameProfile::Times stages = profile_.average();
        for(size_t i = 0; i < FrameProfile::kStageCount; i++)
        {
            if(stages[i] >= 0.05f)
            {
                ImGui::Text("%-9s %5.2f ms", FrameProfile::kNames[i],
                            static_cast<double>(stages[i]));
            }
        }
    }
    ImGui::End();
}

// What was decided for the rigged mesh worn by the avatar inspected
// (ObjectView::wornReports), as text: for finding why some draws wrong.
std::string App::wornMeshText()
{
    const auto shortId = [](const Uuid& id) { return id.toString().substr(0, 8); };
    const auto modeName = [](assets::DiffuseAlphaMode mode) -> std::string_view
    {
        switch(mode)
        {
        case assets::DiffuseAlphaMode::None:
            return "none";
        case assets::DiffuseAlphaMode::Blend:
            return "blend";
        case assets::DiffuseAlphaMode::Mask:
            return "mask";
        case assets::DiffuseAlphaMode::Emissive:
            return "emissive";
        case assets::DiffuseAlphaMode::Default:
            return "default";
        }
        return "?";
    };
    std::string text;
    for(const ObjectView::RiggedReport& r : objects_->wornReports())
    {
        std::string name = "?";
        const world::Object* root =
            world_ ? world_->object(r.root.region, r.root.localId) : nullptr;
        if(root && session_)
        {
            if(const auto* info =
                   objectProperties_.get(*session_, root->region, root->localId, root->id))
            {
                name = info->name;
            }
        }
        text += std::format("'{}' (attached {}, prim {}) mesh {} {}, detail {}\n", name,
                            r.root.localId, r.key.localId, shortId(r.mesh),
                            r.animated ? "animesh (own skeleton)" : "rigged (wearer's skeleton)",
                            r.detail);
        text += std::format("  determinants: frame {:+.3f}, bind shape {:+.3f}; mirrored joints "
                            "{} of {}\n",
                            r.rootDeterminant, r.bindShapeDeterminant, r.mirroredJoints, r.joints);
        if(r.faces.empty())
        {
            text += "  (mesh not loaded)\n";
        }
        for(const ObjectView::FaceReport& f : r.faces)
        {
            text += std::format("  face {}: ", f.face);
            if(!f.skipped.empty())
            {
                text += std::format("not drawn ({})\n", f.skipped);
                continue;
            }
            const std::string drawn = f.blend ? std::string("BLENDED")
                                      : f.alphaCutoff >= 0.0f
                                          ? std::format("MASKED at {:.2f}", f.alphaCutoff)
                                      : f.opaque ? std::string("OPAQUE (alpha ignored)")
                                                 : std::string("OPAQUE");
            text += std::format(
                "{} {}{}; texture {} alpha {}; material {} {}{}; tint alpha {:.2f}, glow {:.2f}\n",
                f.pbr ? "PBR" : "legacy", drawn, f.doubleSided ? ", double sided" : "",
                shortId(f.texture),
                f.textureAlpha ? image::alphaModeName(*f.textureAlpha) : "not loaded",
                f.legacy.isNull() ? std::string("none") : shortId(f.legacy), f.legacyState,
                f.alphaMode
                    ? std::format(", mode {} cutoff {:.2f}", modeName(*f.alphaMode), f.maskCutoff)
                    : std::string(),
                static_cast<double>(f.tintAlpha), static_cast<double>(f.glow));
        }
    }
    return text;
}

void App::drawWornMeshWindow()
{
    if(!ImGui::Begin("Worn mesh", &showWornMesh_))
    {
        ImGui::End();
        return;
    }
    std::string text = wornMeshText();
    if(text.empty())
    {
        text = agentKey_ ? "No rigged mesh worn (or none in view)." : "Not in world.";
    }
    if(ImGui::Button("Copy"))
    {
        ImGui::SetClipboardText(text.c_str());
    }
    ImGui::SameLine();
    if(ImGui::Button("Log"))
    {
        log::info("worn mesh:\n{}", text);
    }
    ImGui::Separator();
    ImGui::TextUnformatted(text.c_str());
    ImGui::End();
}

std::optional<world::ObjectKey> App::dumpTarget() const
{
    if(dumpAvatar_.empty() || dumped_ || !world_)
    {
        return std::nullopt;
    }
    if(dumpAvatar_ == "self")
    {
        return agentKey_;
    }
    const auto id = Uuid::parse(dumpAvatar_);
    auto lower = [](std::string t)
    {
        std::ranges::transform(t, t.begin(), [](unsigned char c) { return std::tolower(c); });
        return t;
    };
    const std::string wanted = lower(dumpAvatar_);
    for(const auto& [key, o] : world_->objects())
    {
        if(o.type != world::ObjectType::Avatar)
        {
            continue;
        }
        if(id ? o.id == *id
              : lower(o.firstName + " " + o.lastName).find(wanted) != std::string::npos)
        {
            return key;
        }
    }
    return std::nullopt;
}

void App::dumpAvatar(const world::Object& avatar)
{
    const world::World& world = *world_;
    std::string text =
        std::format("avatar {} {} ({})\n", avatar.firstName, avatar.lastName, avatar.id.toString());
    auto faceText = [&](const world::TextureFace& f)
    {
        std::string t = f.texture.toString();
        if(avatars_)
        {
            if(const auto bake = avatars_->bakeId(world, avatar, f.texture))
            {
                t += " (bake -> " + bake->toString() + ")";
            }
        }
        t += std::format(" color {:.3f} {:.3f} {:.3f} {:.3f}", f.color.r, f.color.g, f.color.b,
                         f.color.a);
        if(f.fullbright())
        {
            t += " fullbright";
        }
        if(f.shiny() || f.bump())
        {
            t += std::format(" shiny {} bump {}", f.shiny(), f.bump());
        }
        if(f.glow > 0.0f)
        {
            t += std::format(" glow {:.2f}", f.glow);
        }
        if(!f.material.isNull())
        {
            t += " legacy material " + f.material.toString();
        }
        if(f.repeats != glm::vec2(1.0f) || f.offset != glm::vec2(0.0f) || f.rotation != 0.0f)
        {
            t += std::format(" repeats {:.3f} {:.3f} offset {:.3f} {:.3f} rotation {:.3f}",
                             f.repeats.x, f.repeats.y, f.offset.x, f.offset.y, f.rotation);
        }
        if(f.mediaTexGen)
        {
            t += std::format(" mediaTexGen {:#x}", f.mediaTexGen);
        }
        return t;
    };

    // The appearance: its bakes (texture entry), visual parameters, hover.
    if(const world::World::Appearance* a = world.appearance(avatar.id))
    {
        text += std::format("appearance: {} visual params, hover {:.3f}\n", a->visualParams.size(),
                            a->hover.z);
        for(size_t i = 0; i < a->faces.size(); i++)
        {
            if(!a->faces[i].texture.isNull())
            {
                text += std::format("  slot {}: {}\n", i, faceText(a->faces[i]));
            }
        }
        std::string params;
        for(const uint8_t v : a->visualParams)
        {
            params += std::format("{} ", v);
        }
        text += "  visual params: " + params + "\n";
    }
    else
    {
        text += "no appearance yet\n";
    }
    if(const auto* playing = world.animations(avatar.id))
    {
        text += "animations:";
        for(const auto& p : *playing)
        {
            text += " " + p.id.toString();
        }
        text += "\n";
    }

    // Its attachments, prim by prim, as received.
    std::vector<const world::Object*> roots;
    for(const auto& [key, o] : world.objects())
    {
        if(o.region == avatar.region && o.parentId == avatar.localId)
        {
            roots.push_back(&o);
        }
    }
    for(const world::Object* root : roots)
    {
        std::string name = "?";
        if(session_)
        {
            if(const auto* info =
                   objectProperties_.get(*session_, root->region, root->localId, root->id))
            {
                name = info->name;
            }
        }
        const int point = ((root->state & 0xF0) >> 4) | ((root->state & 0x0F) << 4);
        text += std::format("attachment '{}' at point {} (local {})\n", name, point, root->localId);
        std::vector<const world::Object*> prims{root};
        for(const auto& [key, o] : world.objects())
        {
            if(o.region == root->region && o.parentId == root->localId)
            {
                prims.push_back(&o);
            }
        }
        for(const world::Object* o : prims)
        {
            const bool mesh = (o->sculptType & world::SculptType::Mask) == world::SculptType::Mesh;
            text += std::format("  prim {}{}: {} {}, scale {:.3f} {:.3f} {:.3f}{}\n", o->localId,
                                o == root ? " (root)" : "", mesh ? "mesh" : "prim",
                                mesh ? o->sculptTexture.toString() : std::string(), o->scale.x,
                                o->scale.y, o->scale.z, o->animatedMesh ? ", animesh" : "");
            const world::FaceOverrides* overrides =
                world.materialOverrides({o->region, o->localId});
            // Faces past the last that differs from the one after it all
            // look alike: up to 8 for mesh.
            const size_t faces = mesh ? 8 : 9;
            for(size_t i = 0; i < faces; i++)
            {
                std::string line = faceText(o->faces[i]);
                for(const auto& [face, material] : o->materials)
                {
                    if(face == i)
                    {
                        line += " PBR " + material.toString();
                    }
                }
                if(overrides)
                {
                    for(const auto& [face, ov] : *overrides)
                    {
                        if(face != i)
                        {
                            continue;
                        }
                        line += " override:";
                        if(ov.baseColor)
                        {
                            line += std::format(" bc {:.3f} {:.3f} {:.3f} {:.3f}", ov.baseColor->r,
                                                ov.baseColor->g, ov.baseColor->b, ov.baseColor->a);
                        }
                        for(size_t t = 0; t < ov.textures.size(); t++)
                        {
                            if(ov.textures[t])
                            {
                                line += std::format(" tex{} {}", t, ov.textures[t]->toString());
                            }
                        }
                        if(ov.alphaMode)
                        {
                            line += std::format(" am {}", static_cast<int>(*ov.alphaMode));
                        }
                        if(ov.metallic)
                        {
                            line += std::format(" mf {:.2f}", *ov.metallic);
                        }
                        if(ov.roughness)
                        {
                            line += std::format(" rf {:.2f}", *ov.roughness);
                        }
                    }
                }
                text += std::format("    face {}: {}\n", i, line);
            }
        }
    }
    text += "as drawn:\n" + wornMeshText();
    log::info("dump:\n{}", text);
}

void App::drawEnvironmentWindow()
{
    if(ImGui::Begin("Environment"))
    {
        const EnvironmentManager::Status& s = environment_->status();
        ImGui::Text("%s%s", s.name.c_str(), s.fromRegion ? "" : " (none from the region)");
        if(s.dayLength > 0.0)
        {
            ImGui::Text("Day: %.0f%% through, %.1f hours long", s.position * 100.0,
                        s.dayLength / 3600.0);
        }
        const float up = std::clamp(s.sunDirection.z, -1.0f, 1.0f);
        ImGui::Text("Sun %.0f degrees up", static_cast<double>(glm::degrees(std::asin(up))));
        bool fixed = environment_->dayPosition().has_value();
        if(ImGui::Checkbox("Fixed time of day", &fixed))
        {
            environment_->setDayPosition(
                fixed ? std::optional<float>(static_cast<float>(s.position)) : std::nullopt);
        }
        ImGui::Checkbox("Sun shadows", &shadows_);
        int placement = static_cast<int>(hudPlacement_);
        if(ImGui::Combo("HUDs", &placement,
                        "Over the screen\0On the left forearm\0Floating in front\0"))
        {
            hudPlacement_ = static_cast<HudPlacement>(placement);
        }
        float exposure = environment_->exposure();
        if(ImGui::SliderFloat("Exposure", &exposure, 0.1f, 4.0f, "%.2f",
                              ImGuiSliderFlags_Logarithmic))
        {
            environment_->setExposure(exposure);
        }
        if(fixed)
        {
            float position = *environment_->dayPosition();
            if(ImGui::SliderFloat("Time of day", &position, 0.0f, 1.0f))
            {
                environment_->setDayPosition(position);
            }
        }
    }
    ImGui::End();
}

void App::drawTeleportWindow()
{
    if(!session_ || session_->state() != session::SessionState::Connected)
    {
        return;
    }
    if(ImGui::Begin("Teleport"))
    {
        if(const session::Region* main = session_->mainRegion())
        {
            const auto p = session_->agentPosition();
            ImGui::Text("In %s (grid %u, %u)%s", main->name.c_str(), main->gridX(), main->gridY(),
                        session_->agentRegion() == main->handle ? "" : ", arriving");
            if(const auto global = agentGlobalPosition())
            {
                ImGui::Text("Avatar at %.1f, %.1f, %.1f", std::fmod(global->x, 256.0),
                            std::fmod(global->y, 256.0), global->z);
            }
            else
            {
                ImGui::Text("Arrived at %.1f, %.1f, %.1f", static_cast<double>(p.x),
                            static_cast<double>(p.y), static_cast<double>(p.z));
            }
        }
        const session::TeleportStatus& status = session_->teleport();
        const bool idle = status.state == session::TeleportState::None;
        ImGui::BeginDisabled(!idle);
        ImGui::SetNextItemWidth(-80.0f);
        const bool entered = ImGui::InputTextWithHint(
            "##where", "Region/x/y/z or secondlife:// link", teleportInput_.data(),
            teleportInput_.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if(ImGui::Button("Teleport") || entered)
        {
            if(const auto location = session::parseLocation(teleportInput_.data()))
            {
                session_->teleportTo(*location);
            }
        }
        if(ImGui::Button("Home"))
        {
            session_->teleportHome();
        }
        ImGui::EndDisabled();
        if(!idle)
        {
            ImGui::Text("%s%s%s", std::string(session::teleportStateName(status.state)).c_str(),
                        status.message.empty() ? "" : ": ", status.message.c_str());
        }
        else if(!status.failure.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", status.failure.c_str());
        }
    }
    ImGui::End();
}

void App::addHoverLabel(render::Scene& scene) const
{
    // What a click on what the pointer's on does, and its name: a bubble
    // by where the pointer meets it, the same size on screen at any
    // distance, so it shows in first person and in a headset (in each
    // eye) as on the desktop. On a HUD attachment, on the HUD.
    const auto& hover = pointer_.hover();
    if(!hover || !(hover->touchable || hover->clickAction != world::ClickAction::Touch))
    {
        return;
    }
    static constexpr const char* kActions[] = {"Touch", "Sit",  "Buy",        "Pay",
                                               "Open",  "Play", "Open media", "Zoom"};
    if(hover->clickAction >= std::size(kActions))
    {
        return;
    }
    // The script's own words for touching or sitting (llSetTouchText,
    // llSetSitText), once its properties are in.
    std::string action = kActions[hover->clickAction];
    if(hoverInfo_ && hover->clickAction == world::ClickAction::Touch &&
       !hoverInfo_->touchName.empty())
    {
        action = hoverInfo_->touchName;
    }
    if(hoverInfo_ && hover->clickAction == world::ClickAction::Sit && !hoverInfo_->sitName.empty())
    {
        action = hoverInfo_->sitName;
    }
    render::Label label;
    label.style = render::Label::Style::NameTag;
    label.hud = hover->hud;
    label.position = hover->hit.position;
    label.overAll = true; // not hidden by what's in front of the point
    label.lines.push_back({std::move(action), glm::vec4(1.0f), 10.0f});
    if(hoverInfo_ && !hoverInfo_->name.empty())
    {
        label.lines.push_back({hoverInfo_->name, glm::vec4(0.75f, 0.78f, 0.84f, 1.0f), 9.0f});
    }
    // The interface's panels' color: seen against a night sky too.
    label.background = glm::vec4(0.086f, 0.11f, 0.165f, 0.9f);
    label.raise = 14.0f;
    scene.labels.push_back(std::move(label));
}

void App::addCenterDot(render::Scene& scene) const
{
    // First person on a screen, the mouse captured: clicks and the look-at
    // go through the middle of the view, marked by a small dot (a pointer
    // line of no length, all dot), white in a dark ring to show on light
    // and dark alike. Not a crosshair: this is a place to be, not a shooter.
    if(!camera_.firstPerson() || cursorFree() || (headset_ && headset_->running()))
    {
        return;
    }
    const glm::vec3 forward = scene.camera.target - scene.camera.position;
    if(glm::length(forward) < 1e-6f)
    {
        return;
    }
    render::PointerLine dot;
    dot.from = dot.to = scene.camera.position + glm::normalize(forward);
    dot.color = glm::vec4(0.0f, 0.0f, 0.0f, 0.45f);
    dot.dot = 8.0f;
    scene.pointerLines.push_back(dot);
    dot.color = glm::vec4(1.0f, 1.0f, 1.0f, 0.9f);
    dot.dot = 5.0f;
    scene.pointerLines.push_back(dot);
}

namespace
{

// A CSS cursor name as the system's cursor (the usual one for those it
// has none like).
SDL_SystemCursor systemCursor(std::string_view css)
{
    static constexpr std::pair<std::string_view, SDL_SystemCursor> kNames[] = {
        {"pointer", SDL_SYSTEM_CURSOR_POINTER},
        {"text", SDL_SYSTEM_CURSOR_TEXT},
        {"move", SDL_SYSTEM_CURSOR_MOVE},
        {"not-allowed", SDL_SYSTEM_CURSOR_NOT_ALLOWED},
        {"wait", SDL_SYSTEM_CURSOR_WAIT},
        {"progress", SDL_SYSTEM_CURSOR_PROGRESS},
        {"crosshair", SDL_SYSTEM_CURSOR_CROSSHAIR},
        {"ew-resize", SDL_SYSTEM_CURSOR_EW_RESIZE},
        {"col-resize", SDL_SYSTEM_CURSOR_EW_RESIZE},
        {"ns-resize", SDL_SYSTEM_CURSOR_NS_RESIZE},
        {"row-resize", SDL_SYSTEM_CURSOR_NS_RESIZE},
        {"nwse-resize", SDL_SYSTEM_CURSOR_NWSE_RESIZE},
        {"nesw-resize", SDL_SYSTEM_CURSOR_NESW_RESIZE},
    };
    for(const auto& [name, cursor] : kNames)
    {
        if(name == css)
        {
            return cursor;
        }
    }
    return SDL_SYSTEM_CURSOR_DEFAULT;
}

} // namespace

void App::updateCursor()
{
    auto show = [&](SDL_SystemCursor wanted)
    {
        auto& cursor = cursors_[static_cast<size_t>(wanted)];
        if(!cursor)
        {
            cursor.reset(SDL_CreateSystemCursor(wanted));
        }
        if(cursor)
        {
            SDL_SetCursor(cursor.get());
        }
    };
    // Over ImGui's windows (developer), it picks its own cursor. It sets
    // one only when it changes from the last it set: the arrow, as when it
    // last had it.
    ImGuiIO& io = ImGui::GetIO();
    if(io.WantCaptureMouse)
    {
        if(cursorShown_)
        {
            show(SDL_SYSTEM_CURSOR_DEFAULT);
            cursorShown_.reset();
        }
                                    profilePicture_ = image;
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        return;
    }
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    // The page under the mouse (the main menu over the window, or a panel
    // the mouse points at), else a hand over what a click does something
    // to (what it does is a label by it: addHoverLabel).
    std::string name = dialogOverWindow() ? dialogPage_->surface().cursor() : std::string();
    if(name.empty() && mainMenuOverWindow())
    {
        name = mainMenu_->surface().cursor();
    }
    if(name.empty())
    {
        name = controls_.cursorPointed(1);
    }
    if(const auto& hover = pointer_.hover();
       name.empty() && hover &&
       (hover->touchable || hover->clickAction != world::ClickAction::Touch))
    {
        name = "pointer";
    }
    const SDL_SystemCursor wanted = systemCursor(name);
    if(cursorShown_ == wanted)
    {
        return;
    }
    show(wanted);
    cursorShown_ = wanted;
}

void App::drawUi()
{
    if(developer_)
    {
        ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);
    }

    updateCursor();

    // viewerconfig: its answers, and the home scene it names (unless one
    // was given) once it's here.
    viewerConfig_->update(std::chrono::steady_clock::now(), lobby_);
    if(!homeSceneGiven_ && viewerConfig_->homeSceneVersion() != homeSceneVersion_)
    {
        homeSceneVersion_ = viewerConfig_->homeSceneVersion();
        homeScenePath_ =
            viewerConfig_->homeScene().empty() ? builtInHomeScene_ : viewerConfig_->homeScene();
        loadHomeScene();
        pageIsLogin_ = false; // the login panel placed anew
        boardsMoved_ = true;  // and the boards
    }
    placeBoards();
    updateScriptDialogs();
    if(boards_ && homeShown())
    {
        boards_->setHeadset(headset_ && headset_->running());
        boards_->update();
        // A link clicked: the system's web browser (the viewer has none).
        if(const auto link = boards_->takeLink())
        {
            log::info("opening {} in the web browser", *link);
            if(!SDL_OpenURL(link->c_str()))
            {
                log::warn("the web browser couldn't be opened: {}", SDL_GetError());
            }
        }
    }

    // Logging in: the login screen's (the ImGui panel's without one).
    loginFlow_->observe(session_.get());
    if(!loginScreen_)
    {
        handleLoginAction(loginPanel_->draw(session_.get()));
    }
    else if(loginScreenShown())
    {
        handleLoginAction(loginScreen_->update(session_.get()));
    }
    // Our profile picture, for the account on the login screen.
    if(const auto* login = session_ ? session_->login() : nullptr)
    {
        accountPicture_->update(login->agentId, session_->capability("AgentProfile").value_or(""),
                                [this](const Uuid& image)
                                {
                                    if(const auto account = loginFlow_->account())
                                    {
                                        savedLogins_->setImage(*account, image);
                                    }
                                });
    }
    if(developer_)
    {
        drawDeveloperWindows();
        if(loginScreen_)
        {
            handleLoginAction(loginPanel_->drawSessionWindow(session_.get()));
        }
    }
}

void App::drawDeveloperWindows()
{
    if(ImGui::Begin("Stats"))
    {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::Text("%.1f FPS (%.2f ms)", static_cast<double>(io.Framerate),
                    static_cast<double>(1000.0f / io.Framerate));
        ImGui::Text("GPU driver: %s", renderer_->driverName());
        ImGui::Text("Worker threads: %u (%zu jobs queued)", jobs_->threadCount(),
                    jobs_->pendingCount());
        ImGui::Checkbox("FPS overlay", &showFps_);
        ImGui::Checkbox("Material test scene", &showTestScene_);
        ImGui::Checkbox("PBR terrain shader", &pbrTerrain_);
        ImGui::Checkbox("ImGui demo window", &showDemoWindow_);
        ImGui::Checkbox("Worn mesh inspector", &showWornMesh_);
        // What the pointer is on, and why a click would (or wouldn't) touch it.
        if(const auto& hover = pointer_.hover(); hover && world_)
        {
            if(const world::Object* o = world_->object(hover->key.region, hover->key.localId))
            {
                const world::Object& root = ObjectView::linkRoot(*o, *world_);
                ImGui::Text("Pointer: %s face %d%s", o->id.toString().c_str(), hover->face,
                            hover->hud ? " (HUD)" : "");
                ImGui::Text("  flags %#x, root %s flags %#x: %s, click action %u", o->updateFlags,
                            root.id.toString().c_str(), root.updateFlags,
                            hover->touchable ? "touchable" : "not touchable",
                            static_cast<unsigned>(hover->clickAction));
            }
        }
        else
        {
            ImGui::Text("Pointer: nothing");
        }
        const glm::vec3 target = camera_.target();
        ImGui::Text("Camera target %.1f, %.1f, %.1f", static_cast<double>(target.x),
                    static_cast<double>(target.y), static_cast<double>(target.z));
        if(world_)
        {
            ImGui::Text("Terrain regions: %zu (%zu texture sets ready)", world_->terrain().size(),
                        terrainTextures_.readyCount());
        }
        // Counting thousands of textures and meshes isn't free: twice a
        // second is plenty for reading.
        if(const uint64_t ticks = SDL_GetTicksNS(); ticks - uiStatsTicks_ > 500'000'000ull)
        {
            uiStatsTicks_ = ticks;
            uiTextureStats_ = textures_->stats();
            uiMeshStats_ = meshes_->stats();
        }
        const auto& t = uiTextureStats_;
        ImGui::Text("Textures: %zu resident, %zu loading, %zu downloading, %zu queued, %zu failed",
                    t.resident, t.loading, t.downloading, t.queued, t.failed);
        ImGui::Text("Downloaded: %.1f MB", static_cast<double>(t.bytesDownloaded) / 1e6);
        if(world_)
        {
            const auto o = objects_->stats();
            ImGui::Text("Objects: %zu known, %zu prims drawn (%zu faces), %zu not drawable yet",
                        world_->objects().size(), o.drawn, o.faces, o.skipped);
            ImGui::Text("Prim meshes: %zu cached, %zu generating", o.meshes, o.pending);
            ImGui::Text("Particles: %zu from %zu sources", o.particles, o.particleSources);
            const auto c = worldThread_->cacheStats();
            ImGui::Text("Object cache: %llu hits, %llu misses",
                        static_cast<unsigned long long>(c.hits),
                        static_cast<unsigned long long>(c.misses));
            const auto& m = uiMeshStats_;
            ImGui::Text("Mesh assets: %zu known, %zu levels ready, %zu downloading, %zu decoding, "
                        "%zu failed (%.1f MB); %zu mesh objects drawn",
                        m.meshes, m.lodsReady, m.downloading, m.decoding, m.failed,
                        static_cast<double>(m.bytesDownloaded) / 1e6, o.meshObjects);
        }
        ImGui::TextDisabled("Right-drag: look, wheel: zoom, middle-drag: pan, WASD/QE: move");
        ImGui::TextDisabled("Arrows: walk and turn (Shift: sideways), PgUp/PgDn: up/down, "
                            "Home: fly");
    }
    ImGui::End();

    if(showFps_)
    {
        drawFpsOverlay();
    }
    textureBrowser_->draw();
    drawTeleportWindow();
    drawEnvironmentWindow();
    if(showWornMesh_)
    {
        drawWornMeshWindow();
    }
    if(showDemoWindow_)
    {
        ImGui::ShowDemoWindow(&showDemoWindow_);
    }
}

} // namespace viewer
