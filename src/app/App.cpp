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

void printUsage()
{
    std::println(stderr,
                 "usage: viewer [--cache-dir PATH] [--import-textures DIR] [--grid LOGIN_URI] "
                 "[--login-file PATH] [--no-vsync]");
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
    app->textureCache_ = std::make_unique<assets::TextureCache>(cacheRoot / "textures", *app->jobs_,
                                                                *app->j2kDecoder_);
    log::info("{} worker threads; texture cache at {}", app->jobs_->threadCount(),
              app->textureCache_->root().string());

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
    app->screenshotPath_ = options.screenshotPath;
    app->screenshotDelay_ = options.screenshotDelay;
    app->startTicks_ = SDL_GetTicksNS();
    app->lastFrameTicks_ = app->startTicks_;

    app->http_ = net::createHttpClient({.userAgent = std::format("viewer/{}", VIEWER_VERSION)});
    // A second cache instance on the same directory: each instance has one
    // results queue, and the texture browser drains its own.
    app->worldTextureCache_ = std::make_unique<assets::TextureCache>(
        cacheRoot / "textures", *app->jobs_, *app->j2kDecoder_);
    app->textures_ =
        std::make_unique<TextureManager>(*app->worldTextureCache_, *app->renderer_, *app->http_);
    app->meshes_ = std::make_unique<MeshManager>(*app->http_, *app->jobs_, *app->renderer_,
                                                 cacheRoot / "meshes");
    app->objects_ = std::make_unique<ObjectView>(*app->jobs_, *app->renderer_);
    {
        const char* base = SDL_GetBasePath();
        const fs::path character = fs::path(base ? base : "") / "assets" / "character";
        if(auto avatar = avatar::BaseAvatar::load(character))
        {
            app->animations_ = std::make_unique<AnimationManager>(
                *app->http_, *app->jobs_, avatar->skeleton, cacheRoot / "animations");
            app->avatars_ = std::make_unique<AvatarView>(std::move(*avatar), *app->renderer_);
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
        worldThread_ = std::make_unique<world::WorldThread>();
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

        // Keep the session going even while minimized.
        updateSession();

        if(SDL_GetWindowFlags(window_.get()) & SDL_WINDOW_MINIMIZED)
        {
            SDL_Delay(10);
            continue;
        }

        textureBrowser_->update();
        textures_->update();
        const uint64_t now = SDL_GetTicksNS();
        if(!ImGui::GetIO().WantCaptureKeyboard)
        {
            camera_.update(static_cast<float>(now - lastFrameTicks_) * 1e-9f);
        }
        constexpr size_t kFrameHistory = 240;
        if(frameTimes_.size() >= kFrameHistory)
        {
            frameTimes_.erase(frameTimes_.begin());
        }
        frameTimes_.push_back(static_cast<float>(now - lastFrameTicks_) * 1e-6f);
        lastFrameTicks_ = now;
        if(!screenshotPath_.empty() && !screenshotRequested_ &&
           static_cast<float>(now - startTicks_) * 1e-9f >= screenshotDelay_)
        {
            renderer_->requestScreenshot();
            screenshotRequested_ = true;
        }

        ui_->beginFrame();
        drawUi();
        ui_->endFrame();

        if(auto frame = renderer_->beginFrame())
        {
            renderer_->drawScene(*frame, buildScene());
            ui_->record(*frame);
        }
        renderer_->endFrame();

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
                    log::info("rigged meshes drawn: {}; skipped objects: {}", o.palettes,
                              o.skipped);
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
    if(auto latest = worldThread_->snapshot(); latest != world_)
    {
        worldThread_->retire(std::exchange(world_, std::move(latest)));
    }

    // Once we know where the agent is, put the world origin at its region
    // and the camera behind it.
    if(!originHandle_ && session_->agentRegion())
    {
        originHandle_ = session_->agentRegion();
        const auto p = session_->agentPosition();
        if(!fixedCamera_)
        {
            camera_.lookAt(glm::vec3(p.x, p.y, p.z + 0.3f), 5.0f, 90.0f, 12.0f);
        }
    }

    // Tell the region where our camera is (its coordinates are the origin
    // region's, which is where the agent arrived).
    if(originHandle_)
    {
        const render::Camera c = camera_.camera();
        const glm::vec3 at = glm::normalize(c.target - c.position);
        const glm::vec3 left = glm::normalize(glm::cross(glm::vec3(0, 0, 1), at));
        const glm::vec3 up = glm::cross(at, left);
        session::Session::CameraView view;
        view.position = {c.position.x, c.position.y, c.position.z};
        view.atAxis = {at.x, at.y, at.z};
        view.leftAxis = {left.x, left.y, left.z};
        view.upAxis = {up.x, up.y, up.z};
        view.farDistance = 128.0f;
        session_->setCamera(view);
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
    if(animations_)
    {
        animations_->setViewerAssetUrl(viewerAsset);
    }

    if(session_->state() == session::SessionState::LoggedOut)
    {
        endSession();
    }
}

void App::endSession()
{
    textures_->setViewerAssetUrl({});
    meshes_->setViewerAssetUrl({});
    if(animations_)
    {
        animations_->setViewerAssetUrl({});
    }
    session_.reset();
    world_.reset();
    worldThread_.reset();
    terrainView_.clear();
    originHandle_.reset();
}

render::Scene App::buildScene()
{
    render::Scene scene;
    scene.camera = camera_.camera();
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
        if(avatars_)
        {
            animations_->update();
            const double now = static_cast<double>(SDL_GetTicksNS() - startTicks_) * 1e-9;
            avatars_->addTo(scene, *world_, *originHandle_, *textures_, *animations_, *meshes_, now,
                            128.0f);
        }
        objects_->addTo(scene, *world_, *originHandle_, *textures_, *meshes_, avatars_.get(),
                        128.0f);
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
    return scene;
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
            const auto m = meshes_->stats();
            ImGui::Text("Mesh assets: %zu known, %zu levels ready, %zu downloading, %zu decoding, "
                        "%zu failed (%.1f MB); %zu mesh objects drawn",
                        m.meshes, m.lodsReady, m.downloading, m.decoding, m.failed,
                        static_cast<double>(m.bytesDownloaded) / 1e6, o.meshObjects);
        }
        ImGui::TextDisabled("Right-drag: look, wheel: zoom, middle-drag: pan, WASD/QE: move");
    }
    ImGui::End();

    if(showFps_)
    {
        drawFpsOverlay();
    }
    textureBrowser_->draw();
    handleLoginAction(loginPanel_->draw(session_.get()));

    if(showDemoWindow_)
    {
        ImGui::ShowDemoWindow(&showDemoWindow_);
    }
}

} // namespace viewer
