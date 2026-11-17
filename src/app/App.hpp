#pragma once

#include "AgentController.hpp"
#include "AnimationManager.hpp"
#include "AvatarNames.hpp"
#include "AvatarView.hpp"
#include "CameraController.hpp"
#include "EnvironmentManager.hpp"
#include "FrameProfile.hpp"
#include "LegacyMaterialManager.hpp"
#include "LocalChat.hpp"
#include "LoginPanel.hpp"
#include "MainMenu.hpp"
#include "MaterialManager.hpp"
#include "ObjectProperties.hpp"
#include "ObjectView.hpp"
#include "OutfitAttachments.hpp"
#include "PointerController.hpp"
#include "SceneAssets.hpp"
#include "TerrainTextures.hpp"
#include "TerrainView.hpp"
#include "TextureBrowser.hpp"
#include "TextureManager.hpp"
#include "TreeView.hpp"
#include "ViewerEffects.hpp"
#include "VrControls.hpp"

#include "viewer/assets/TextureCache.hpp"
#include "viewer/core/DiskCache.hpp"
#include "viewer/core/JobSystem.hpp"
#include "viewer/image/J2kDecoder.hpp"
#include "viewer/net/HttpClient.hpp"
#include "viewer/render/Renderer.hpp"
#include "viewer/session/Session.hpp"
#include "viewer/ui/ImGuiLayer.hpp"
#include "viewer/world/World.hpp"
#include "viewer/world/WorldThread.hpp"
#include "viewer/xr/Headset.hpp"

#include <SDL3/SDL_video.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace viewer
{

struct AppOptions
{
    std::filesystem::path cacheDirectory;  // empty = platform default
    std::filesystem::path importDirectory; // optional: texture browser import on startup
    // --ui-dir: the interface's documents (empty: assets/ui next to the
    // program; the source folder's, to edit them while running).
    std::filesystem::path uiDirectory;
    bool mainMenu = false;           // --main-menu: open it at the start
    std::string loginUri;            // empty = Second Life main grid
    std::filesystem::path loginFile; // optional: log in at startup (see Credentials.hpp)
    bool testScene = false;          // show the material test scene
    bool pbrTerrain = false;         // start with the PBR terrain shader
    bool firstPerson = false;        // go first person once in the world
    bool vr = false;                 // --vr: show the world in a headset (OpenXR)
    float vrOverscan = 0.1f; // --vr-overscan PERCENT: each eye drawn this much wider per side
    // --vr-eye-height METRES: the wearer's eye height standing, which the
    // headset's movements are scaled from to the avatar's (0: not scaled;
    // unset: measured from the floor).
    std::optional<float> vrEyeHeight;
    bool vsync = true;                          // --no-vsync: uncapped, for measuring
    std::string start;                          // login start: last, home, Region[/x/y/z]
    std::filesystem::path screenshotPath;       // optional: save a screenshot, then quit
    float screenshotDelay = 3.0f;               // seconds after startup
    std::optional<std::array<float, 6>> camera; // x y z distance yaw pitch
    std::string teleport;                       // teleport here 5 s after logging in ("home" too)
    std::optional<float> walk;     // walk this way (degrees from east) once in (after --teleport)
    std::optional<float> dayTime;  // a fixed time of day, 0..1 (0.5 is midday in most cycles)
    bool shadows = true;           // --no-shadows: off (for comparing)
    uint32_t textureMemory = 2048; // --texture-memory: GPU memory for textures, MiB
    // --bandwidth: downstream bandwidth to ask regions for, kbps (the official
    // viewer's ThrottleBandwidthKBPS)
    float bandwidth = session::kDefaultBandwidthKbps;
    // For testing: point at this spot (fractions of the window, from the
    // top left) instead of the mouse, and click there this many seconds in.
    std::optional<std::array<float, 2>> pointer;
    std::optional<float> clickAt;
    Uuid pointAt; // or at this object (its center)
    // For testing: say this in local chat this many seconds in (typing
    // for 3 seconds before).
    std::string say;
    float sayAt = 20.0f;
    bool occlusion = true; // --no-occlusion: draw what's hidden too
    // --stereo-benchmark [SIZE]: draw both eyes of a headset view, SIZE
    // pixels square each (2160), and log what it costs.
    uint32_t stereoSize = 0;
    // --without shadows,lights,probes,glow,exposure,clusters,particles,water: leave passes out, to
    // measure what they cost.
    std::vector<std::string> without;
};

// Parses command-line arguments; returns nullopt (after printing usage) on error.
std::optional<AppOptions> parseArguments(std::span<char*> args);

// Top-level owner of everything in the process. There are no singletons: every
// subsystem is a member here (or of something owned here) and is handed
// explicitly to whatever needs it.
//
// Lifetimes are split in two:
//   - App-lifetime: window, renderer, UI, job system, asset cache, HTTP
//     client, and later audio. These survive logging out and back in.
//   - Session-lifetime: login data and capabilities now; the UDP circuit,
//     event queue and world state later. Held as std::unique_ptr<Session>;
//     logging out destroys it, logging in builds a fresh one. Nothing
//     App-lifetime may hold pointers into a Session.
class App
{
public:
    static std::unique_ptr<App> create(const AppOptions& options);
    ~App() = default;

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    int run();

private:
    App() = default;

    void drawUi();
    void drawFpsOverlay();
    void handleLoginAction(LoginAction action);
    // Wears the Current Outfit Folder's attachments the region didn't
    // bring back (OutfitAttachments).
    void updateOutfit();
    void loadTerrainBlendRamp();
    void updateSession();
    void logOutBeforeExit();
    // Fills the frame's scene (cleared first).
    void buildScene(render::Scene& scene);
    // The headset's eyes (eyeCameras_) and head (into `camera`), placed
    // from `camera`.
    void placeEyes(render::Camera& camera, const xr::HeadsetFrame& frame);
    void endSession();
    // The agent arrived (login, teleport, region crossing): move the world
    // origin and the camera.
    void handleArrival(const session::Arrival& arrival);
    // Asks for the appearance of avatars that still have none (and so are
    // drawn with default shape parameters).
    void requestMissingAppearances();
    // Every 30 s: the process's resident memory and what the caches hold.
    void logMemory(uint64_t now);
    // Our avatar's position, in global meters; nothing until we see it.
    std::optional<glm::dvec3> agentGlobalPosition();
    void drawTeleportWindow();
    void drawEnvironmentWindow();

    struct SdlRuntime
    {
        bool initialized = false;
        ~SdlRuntime();
    };
    struct WindowDeleter
    {
        void operator()(SDL_Window* window) const { SDL_DestroyWindow(window); }
    };

    // Members are destroyed in reverse declaration order, which is the
    // shutdown order: session (cancels its HTTP requests) -> login panel ->
    // HTTP client -> browser (GPU textures) -> cache (waits for its jobs) ->
    // decoder -> job threads -> UI -> headset -> renderer -> window ->
    // SDL_Quit.
    SdlRuntime sdl_;
    std::unique_ptr<SDL_Window, WindowDeleter> window_;
    std::unique_ptr<render::Renderer> renderer_;
    // --vr: the headset (its session goes before the GPU device). Its frame
    // under way, and each eye's camera this frame (buildScene).
    std::unique_ptr<xr::Headset> headset_;
    std::optional<xr::HeadsetFrame> headsetFrame_;
    std::array<render::Camera, 2> eyeCameras_{};
    glm::uvec2 headsetSize_{0}; // per eye, for the log
    // The headset's origin in the scene this frame (placeEyes), and what
    // follows from its tracking: the head's heading from the origin's
    // (degrees), the body's (turning once the head is far round, or to
    // where it looks when walking), the right stick's snap turn.
    struct Vr
    {
        bool placed = false;
        glm::vec3 base{0.0f};
        glm::quat heading{1.0f, 0.0f, 0.0f, 0.0f};
        float headYaw = 0.0f;
        std::optional<float> bodyYaw;
        bool walking = false;
        bool snapped = false;
        bool recentering = false; // the right stick held down
        // The avatar's eye height over the wearer's: the headset's
        // movements (and the eyes' distance apart) scaled by it, so a
        // smaller avatar sees the world bigger and its arms reach as far
        // as the wearer's do, in proportion.
        float scale = 1.0f;
        float loggedScale = 1.0f;
    };
    Vr vr_;
    std::optional<float> vrEyeHeight_; // --vr-eye-height
    // How much the headset's movements are scaled this frame (Vr::scale).
    float vrScale(const xr::HeadsetFrame& frame) const;
    // A pose from the headset's origin into the scene.
    xr::Pose vrToScene(const xr::Pose& pose) const
    {
        return {vr_.base + vr_.heading * (pose.position * vr_.scale),
                vr_.heading * pose.orientation};
    }
    // Our avatar's IK from the headset and controllers.
    void updateVrIk(const render::Scene& scene, const Uuid& self);
    std::unique_ptr<ui::ImGuiLayer> ui_;
    // The viewer's own interface (RmlUi), and its main menu (before it goes).
    std::unique_ptr<gui::Gui> gui_;
    std::unique_ptr<MainMenu> mainMenu_;
    // The main menu: shown over the window (the desktop, third person) or
    // on a panel (first person, VR); its values each frame, its input from
    // the window's events (true: the event was its), and where it's drawn
    // over the window (pixels: x, y, width, height).
    void updateMainMenu();
    bool mainMenuEvent(const SDL_Event& event);
    bool mainMenuOverWindow() const;
    glm::vec4 mainMenuRect() const;
    bool mainMenuPressed_ = false; // the mouse button went down on it
    std::unique_ptr<JobSystem> jobs_;
    std::unique_ptr<image::J2kDecoder> j2kDecoder_;
    std::unique_ptr<DiskCache> diskCache_;                    // before everything using it
    std::unique_ptr<assets::TextureCache> textureCache_;      // texture browser panel
    std::unique_ptr<assets::TextureCache> worldTextureCache_; // textures of the world
    std::unique_ptr<TextureBrowser> textureBrowser_;
    std::unique_ptr<net::HttpClient> http_;
    std::unique_ptr<TextureManager> textures_;
    std::unique_ptr<EnvironmentManager> environment_;
    std::unique_ptr<MeshManager> meshes_;
    std::unique_ptr<MaterialManager> materials_;
    std::unique_ptr<TerrainOverrides> terrainOverrides_;
    // The whole outfit worn (the Current Outfit Folder's attachments), and
    // when our avatar was first seen this session.
    // The inventory as fetched (per session: cleared when one ends) and the
    // AISv3 client filling it; before outfit_, whose requests it carries.
    std::unique_ptr<inventory::Model> inventory_;
    std::unique_ptr<inventory::Ais> ais_;
    std::unique_ptr<OutfitAttachments> outfit_;
    std::optional<double> selfSeenAt_;
    std::unordered_set<Uuid> worn_; // its items worn, as last looked (twice a second)
    double wornAt_ = -1e9;
    std::unique_ptr<LegacyMaterialManager> legacyMaterials_;
    std::unique_ptr<AvatarNames> avatarNames_;     // display names, for name tags
    ObjectMotion motion_;                          // before the views that read it
    std::unique_ptr<AnimationManager> animations_; // with avatars_
    std::unique_ptr<AvatarView> avatars_; // nullptr if the default avatar couldn't be loaded
    std::unique_ptr<ObjectView> objects_;
    // What the mouse points at, and touching it.
    PointerController pointer_;
    // Names and touch text of what the pointer rests on.
    ObjectProperties objectProperties_;
    std::optional<world::ObjectKey> hoverKey_;
    uint64_t hoverSinceTicks_ = 0;
    const ObjectInfo* hoverInfo_ = nullptr;
    std::optional<std::array<float, 2>> pointerOverride_;
    Uuid pointAt_;
    bool loggedHoverInfo_ = false;
    std::optional<float> clickAt_;
    std::string say_; // --say, and when
    float sayAt_ = 0.0f;
    int said_ = 0;                     // 1: typing, 2: said
    bool occlusion_ = true;            // occlusion culling
    uint32_t stereoSize_ = 0;          // --stereo-benchmark: eye size, 0 off
    std::vector<std::string> without_; // --without: passes left out
    uint64_t stereoLoggedTicks_ = 0;   // its last timing log
    // The stats window's texture and mesh counts, refreshed now and then.
    TextureManager::Stats uiTextureStats_{};
    MeshManager::Stats uiMeshStats_{};
    uint64_t uiStatsTicks_ = 0;
    LocalChat localChat_;
    uint64_t clickedTicks_ = 0; // --click-at's press, released a moment later

    // The VR controls (radial menu, keyboard): the controllers point at
    // them; on the desktop, in first person, the mouse does (the right
    // button holds the menu open; Tab, held, frees the cursor to point
    // with).
    VrControls controls_;
    // What the radial menu holds: written here, all of it.
    MenuItem radialMenu();
    // The pointers this frame (before the pointer's ray is aimed), and what
    // they're on.
    void updateControls(render::Scene& scene);
    std::array<VrControls::Pointed, VrControls::kPointers> controlsPointed_{};
    std::optional<PointerController::Ray> vrAim_; // the right controller's ray
    std::array<bool, 2> vrTrigger_{};             // held (with some slack)
    bool cursorFree() const; // first person, the cursor let go of to point with
    bool vrScaling_ = true;  // the radial menu's "Avatar scale"
    // The primary action (a controller's trigger, a click): on what the
    // pointer's on, sit if that's what it does, else touch; held a moment
    // on something that isn't touched, sit on it.
    void primaryPress();
    void primaryRelease();
    void sitOn(const PointerController::Hover& hover);
    bool sitting() const;
    struct SitHold
    {
        world::ObjectKey key;
        uint64_t sinceTicks = 0;
    };
    std::optional<SitHold> sitHold_;
    bool mouselookPressed_ = false; // the left button went to scripts (mouselook)
    // The pointer's ray this frame (none over the UI or while dragging).
    std::optional<PointerController::Ray> pointerRay(const render::Scene& scene) const;
    // The regions' water and the water beyond them, with the day's water
    // settings, into the scene.
    void addWater(render::Scene& scene, float seconds);
    // Our avatar's viewer-made motion (avatar::IkGoals). Desktop: the head
    // looks where the pointer is; while pressing on something, the left
    // hand reaches for it (the arm the official viewer points with). (VR
    // will give the headset's and controllers' poses instead.)
    void updateIk(const render::Scene& scene, const std::optional<PointerController::Ray>& ray);
    void releasePointer();
    // First person (the official viewer's mouselook) on or off: the camera
    // at our eyes, our head hidden, the mouse captured.
    void setFirstPerson(bool on);
    // Others' look-at and point-at, as their heads and arms move.
    void updateOthersIk(const Uuid& self, float seconds);
    // A look-at or point-at's target in the scene (nothing if what it's at
    // isn't known here).
    std::optional<glm::vec3> attentionPoint(const ViewerEffects::Target& target, bool look);
    // Our look-at and point-at to viewers nearby (ViewerEffects).
    void sendAttention(const Uuid& self, bool looking, double seconds,
                       const glm::vec3& cameraForward);
    struct Ik
    {
        std::optional<glm::vec3> reachPoint; // what was pressed on (scene coordinates)
        bool reaching = false;
        // What was pressed, and how far from the camera: dragging off it,
        // the hand follows the pointer's ray at that distance.
        std::optional<world::ObjectKey> pressed;
        float pressDistance = 0.0f;
        glm::vec3 rayOrigin{0.0f};
        ViewerEffects effects; // told to viewers nearby, and theirs
        // Others' look-at and point-at, eased as ours.
        struct Other
        {
            glm::vec3 lookPoint{0.0f};
            float lookWeight = 0.0f;
            glm::vec3 pointPoint{0.0f};
            float pointWeight = 0.0f;
        };
        std::unordered_map<Uuid, Other> others;
        // Targets' keys by id (looked up once, checked after).
        std::unordered_map<Uuid, world::ObjectKey> targetKeys;
        glm::vec3 lookPoint{0.0f};
        float lookWeight = 0.0f;
        float handWeight = 0.0f;
        uint64_t lastTicks = 0;
    } ik_;
    void pressPointer();
    std::unique_ptr<TreeView> trees_;
    TerrainTextures terrainTextures_;
    // Remembered accounts, the logging in (apart from the UI), its panel.
    std::unique_ptr<SavedLogins> savedLogins_;
    std::unique_ptr<LoginFlow> loginFlow_;
    std::unique_ptr<LoginPanel> loginPanel_;
    std::unique_ptr<SceneAssets> sceneAssets_;
    std::unique_ptr<session::Session> session_;
    // Session-lifetime, created and destroyed together with session_. The
    // world thread owns the world; world_ is the snapshot this frame reads.
    std::unique_ptr<world::WorldThread> worldThread_;
    std::shared_ptr<const world::World> world_;
    TerrainView terrainView_;
    std::vector<session::RegionMessage> regionMessages_;
    std::vector<world::WorldRequest> worldRequests_;
    std::vector<std::pair<uint32_t, uint8_t>> objectRequests_;
    std::optional<uint64_t> originHandle_; // world coordinates are relative to this region
    uint64_t seenArrivals_ = 0;
    std::optional<world::ObjectKey> agentKey_; // our avatar's object, when last found
    uint64_t agentSearchRevision_ = ~0ull;     // objects revision we last searched
    std::optional<glm::dvec3> lastAgentGlobal_;
    // Avatars without an appearance: when to ask for it (next).
    std::unordered_map<Uuid, std::chrono::steady_clock::time_point> appearanceDue_;
    std::chrono::steady_clock::time_point nextAppearanceCheck_{};
    AgentController agent_;
    std::string pendingTeleport_;      // --teleport, once we're in
    std::optional<float> walkHeading_; // --walk
    uint64_t arrivedTicks_ = 0;
    uint64_t lastMemoryLog_ = 0;
    std::array<char, 256> teleportInput_{};

    CameraController camera_;
    bool running_ = true;
    bool showDemoWindow_ = false;
    bool showTestScene_ = false;
    bool pbrTerrain_ = false;
    bool wantFirstPerson_ = false;
    bool fixedCamera_ = false; // --camera given: don't move it to the agent
    std::filesystem::path screenshotPath_;
    float screenshotDelay_ = 0.0f;
    bool screenshotRequested_ = false;
    uint64_t startTicks_ = 0;
    uint64_t lastFrameTicks_ = 0;
    // Recent frame times (milliseconds, oldest first) for the FPS overlay.
    std::vector<float> frameTimes_;
    FrameProfile profile_;
    render::Scene scene_; // this frame's, kept for its lists' capacity
    bool showFps_ = true;
    bool shadows_ = true;
    // Where HUD attachments show (render::Hud): over the screen, on the
    // left forearm, or floating in front of the camera.
    enum class HudPlacement : int
    {
        Screen,
        Forearm,
        Floating,
    };
    HudPlacement hudPlacement_ = HudPlacement::Screen;
    // The HUD panel's frame for the placements in the scene.
    void placeHud(render::Scene& scene);
    float bandwidth_ = session::kDefaultBandwidthKbps; // kbps, for each new session
};

} // namespace viewer
