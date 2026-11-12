#include "App.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/image/Png.hpp"
#include "viewer/image/TextureProcessing.hpp"

#include "EmbeddedAssets.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>
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
                 "[--walk DEGREES] [--day-time 0..1] [--no-shadows] [--texture-memory MIB]");
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
        else if(arg == "--day-time" && hasValue)
        {
            i++;
            options.dayTime = std::strtof(args[i], nullptr);
        }
        else if(arg == "--grid" && hasValue)
        {
            i++;
            options.loginUri = args[i];
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
    app->renderer_ = render::Renderer::create(app->window_.get(), renderOptions);
    if(!app->renderer_)
    {
        return nullptr;
    }

    app->ui_ = ui::ImGuiLayer::create(app->window_.get(), *app->renderer_);
    if(!app->ui_)
    {
        return nullptr;
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
    app->showTestScene_ = options.testScene;
    app->pbrTerrain_ = options.pbrTerrain;
    app->camera_.lookAt(glm::vec3(0.0f, -6.0f, 1.0f), 34.0f, 90.0f, 30.0f);
    if(options.camera)
    {
        const auto& c = *options.camera;
        app->camera_.lookAt(glm::vec3(c[0], c[1], c[2]), c[3], c[4], c[5]);
    }
    app->fixedCamera_ = options.camera.has_value();
    app->pendingTeleport_ = options.teleport;
    app->walkHeading_ = options.walk;
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
    app->shadows_ = options.shadows;
    app->textures_ = std::make_unique<TextureManager>(*app->worldTextureCache_, *app->renderer_,
                                                      *app->http_, *app->jobs_);
    app->textures_->setBudget(uint64_t(options.textureMemory) << 20);
    app->meshes_ =
        std::make_unique<MeshManager>(*app->http_, *app->jobs_, *app->renderer_, *app->diskCache_);
    app->materials_ = std::make_unique<MaterialManager>(*app->http_, *app->jobs_, *app->diskCache_);
    app->objects_ = std::make_unique<ObjectView>(*app->jobs_, *app->renderer_, app->motion_);
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
    app->loginPanel_ = std::make_unique<LoginPanel>(session::queryMachineInfo(), "viewer",
                                                    VIEWER_VERSION, options.loginUri);
    if(!options.loginFile.empty())
    {
        app->loginPanel_->setStart(options.start);
        if(auto credentials = session::readCredentialsFile(options.loginFile))
        {
            app->handleLoginAction(app->loginPanel_->logInWith(*credentials));
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
            const bool uiConsumed = ui_->processEvent(event);
            const bool releasing = event.type == SDL_EVENT_MOUSE_BUTTON_UP;
            if(!uiConsumed || releasing)
            {
                camera_.handleEvent(event);
            }
            if(!uiConsumed && !ImGui::GetIO().WantCaptureKeyboard)
            {
                agent_.handleEvent(event);
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
        profile_.mark(FrameProfile::Session);

        if(SDL_GetWindowFlags(window_.get()) & SDL_WINDOW_MINIMIZED)
        {
            SDL_Delay(10);
            continue;
        }

        textureBrowser_->update();
        textures_->update();
        profile_.mark(FrameProfile::Textures);
        const uint64_t now = SDL_GetTicksNS();
        const bool keyboardFree = !ImGui::GetIO().WantCaptureKeyboard;
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
        if(!screenshotPath_.empty() && !screenshotRequested_ &&
           static_cast<float>(now - startTicks_) * 1e-9f >= screenshotDelay_)
        {
            renderer_->requestScreenshot();
            screenshotRequested_ = true;
        }

        // The scene before the swapchain: building it overlaps the GPU
        // finishing earlier frames.
        buildScene(scene_);
        ui_->beginFrame();
        drawUi();
        ui_->endFrame();
        profile_.mark(FrameProfile::Ui);

        if(auto frame = renderer_->beginFrame())
        {
            profile_.mark(FrameProfile::Acquire);
            renderer_->drawScene(*frame, scene_);
            ui_->record(*frame);
            profile_.mark(FrameProfile::Render);
        }
        renderer_->endFrame();
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

    // Messages go to the world thread; this frame reads its latest snapshot
    // (the previous one is released over there).
    session_->drainRegionMessages(regionMessages_);
    for(auto& m : regionMessages_)
    {
        if(m.kind == session::RegionMessage::Kind::RegionRemoved)
        {
            worldThread_->postRegionRemoved(m.regionHandle);
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
        environment_->update(main ? std::optional<uint64_t>(main->handle) : std::nullopt, url);
    }
    const std::string viewerAsset(session_->capability("ViewerAsset").value_or(""));
    textures_->setViewerAssetUrl(viewerAsset);
    if(avatars_)
    {
        const auto* login = session_->login();
        avatars_->setAppearanceServiceUrl(
            login ? login->reply["agent_appearance_service"].asString() : std::string());
    }
    meshes_->setViewerAssetUrl(viewerAsset);
    materials_->setViewerAssetUrl(viewerAsset);
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
    // Behind the avatar, looking where it faces.
    const auto& p = arrival.position;
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
    if(self->parentId != 0)
    {
        // Sitting: relative to what it sits on.
        if(const world::Object* seat = world_->object(self->region, self->parentId))
        {
            const world::Pose seatPose = motion_.pose(*seat);
            local = seatPose.position + seatPose.rotation * local;
        }
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

void App::endSession()
{
    appearanceDue_.clear();
    textures_->setViewerAssetUrl({});
    meshes_->setViewerAssetUrl({});
    materials_->setViewerAssetUrl({});
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
    // Kept from frame to frame, so its lists keep their capacity.
    scene.objects.clear();
    scene.lights.clear();
    scene.terrain.clear();
    scene.worldOrigin = {};
    scene.camera = camera_.camera();
    const auto wallClock = std::chrono::system_clock::now().time_since_epoch();
    environment_->apply(
        scene.lighting,
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::milliseconds>(wallClock).count()) /
            1000.0,
        scene.camera.position.z);
    scene.lighting.atmosphere.clouds = textures_->get(environment_->cloudTexture());
    // Shadows from whichever light is up (none once it's down).
    scene.shadows.enabled = shadows_ && scene.lighting.sunDirection.z > 0.02f &&
                            scene.lighting.sunColor != glm::vec3(0.0f);
    scene.shadows.towardLight = scene.lighting.sunDirection;
    scene.shadows.distance = 128.0f;
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window_.get(), &width, &height);
    textures_->setView(static_cast<uint32_t>(std::max(height, 1)), scene.camera.verticalFov);
    scene.frustum = render::Frustum::fromCamera(
        scene.camera, height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f);
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
                                   // PBR terrain materials from the grid aren't
                                   // supported yet: procedural stand-ins.
                                   return sceneAssets_->terrainPbr();
                               }
                               return terrainTextures_.material(*world_->terrain(handle),
                                                                *textures_, placeholder);
                           });
        const float seconds = static_cast<float>(SDL_GetTicksNS() - startTicks_) * 1e-9f;
        profile_.mark(FrameProfile::Terrain);
        for(const auto& [handle, terrain] : world_->terrain())
        {
            if(!terrain.hasComposition)
            {
                continue;
            }
            const glm::vec2 corner(
                static_cast<float>(static_cast<int64_t>(handle >> 32) -
                                   static_cast<int64_t>(*originHandle_ >> 32)),
                static_cast<float>(static_cast<int64_t>(handle & 0xFFFFFFFFu) -
                                   static_cast<int64_t>(*originHandle_ & 0xFFFFFFFFu)));
            sceneAssets_->addWater(scene, corner, terrain.composition.waterHeight, seconds);
        }
        objects_->update();
        meshes_->update();
        materials_->update();
        if(animations_)
        {
            animations_->update();
        }
        profile_.mark(FrameProfile::Loading);
        if(avatars_)
        {
            const double now = static_cast<double>(SDL_GetTicksNS() - startTicks_) * 1e-9;
            avatars_->addTo(scene, *world_, *originHandle_, *textures_, *animations_, *meshes_, now,
                            128.0f);
        }
        profile_.mark(FrameProfile::Avatars);
        objects_->addTo(scene, *world_, *originHandle_, *textures_, *meshes_, *materials_,
                        avatars_.get(), 128.0f);
        trees_->addTo(scene, *world_, *originHandle_, *textures_, 128.0f);
        profile_.mark(FrameProfile::Objects);
        scene.worldOrigin = glm::dvec2(static_cast<double>(*originHandle_ >> 32),
                                       static_cast<double>(*originHandle_ & 0xFFFFFFFFu));
    }
    if(showTestScene_ || !originHandle_)
    {
        // Logged in: next to the camera's target, so it's easy to find.
        const glm::vec3 origin =
            originHandle_ ? camera_.target() + glm::vec3(0.0f, 30.0f, 0.0f) : glm::vec3(0.0f);
        sceneAssets_->addTestScene(scene, origin);
    }
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

void App::drawUi()
{
    ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);

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
        const glm::vec3 target = camera_.target();
        ImGui::Text("Camera target %.1f, %.1f, %.1f", static_cast<double>(target.x),
                    static_cast<double>(target.y), static_cast<double>(target.z));
        if(world_)
        {
            ImGui::Text("Terrain regions: %zu (%zu texture sets ready)", world_->terrain().size(),
                        terrainTextures_.readyCount());
        }
        const auto t = textures_->stats();
        ImGui::Text("Textures: %zu resident, %zu loading, %zu downloading, %zu queued, %zu failed",
                    t.resident, t.loading, t.downloading, t.queued, t.failed);
        ImGui::Text("Downloaded: %.1f MB", static_cast<double>(t.bytesDownloaded) / 1e6);
        if(world_)
        {
            const auto o = objects_->stats();
            ImGui::Text("Objects: %zu known, %zu prims drawn (%zu faces), %zu not drawable yet",
                        world_->objects().size(), o.drawn, o.faces, o.skipped);
            ImGui::Text("Prim meshes: %zu cached, %zu generating", o.meshes, o.pending);
            const auto c = worldThread_->cacheStats();
            ImGui::Text("Object cache: %llu hits, %llu misses",
                        static_cast<unsigned long long>(c.hits),
                        static_cast<unsigned long long>(c.misses));
            const auto m = meshes_->stats();
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
    handleLoginAction(loginPanel_->draw(session_.get()));

    if(showDemoWindow_)
    {
        ImGui::ShowDemoWindow(&showDemoWindow_);
    }
}

} // namespace viewer
