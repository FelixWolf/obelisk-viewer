#pragma once

#include "AccountPicture.hpp"
#include "AgentController.hpp"
#include "AnimationManager.hpp"
#include "AvatarComplexity.hpp"
#include "AvatarNames.hpp"
#include "AvatarRenderInfo.hpp"
#include "AvatarView.hpp"
#include "CameraController.hpp"
#include "CurrentOutfit.hpp"
#include "EnvironmentManager.hpp"
#include "FrameProfile.hpp"
#include "InstantMessages.hpp"
#include "LegacyMaterialManager.hpp"
#include "LobbyBoards.hpp"
#include "LocalChat.hpp"
#include "LoginPanel.hpp"
#include "LoginScreen.hpp"
#include "MainMenu.hpp"
#include "MaterialManager.hpp"
#include "MediaManager.hpp"
#include "MicrophoneList.hpp"
#include "MusicStream.hpp"
#include "ObjectMedia.hpp"
#include "ObjectProperties.hpp"
#include "ObjectView.hpp"
#include "Offers.hpp"
#include "OutfitAttachments.hpp"
#include "ParcelSounds.hpp"
#include "People.hpp"
#include "PeopleView.hpp"
#include "PointerController.hpp"
#include "RegionMap.hpp"
#include "SceneAssets.hpp"
#include "ScriptDialogPage.hpp"
#include "ScriptDialogs.hpp"
#include "Settings.hpp"
#include "SoundAssets.hpp"
#include "SoundManager.hpp"
#include "TerrainTextures.hpp"
#include "TerrainView.hpp"
#include "TextureBrowser.hpp"
#include "TextureManager.hpp"
#include "TreeView.hpp"
#include "Updater.hpp"
#include "ViewerConfig.hpp"
#include "ViewerEffects.hpp"
#include "ViewerSettings.hpp"
#include "VoiceIndicators.hpp"
#include "VrControls.hpp"
#include "VrTurning.hpp"

#include "viewer/assets/TextureCache.hpp"
#include "viewer/core/DiskCache.hpp"
#include "viewer/core/JobSystem.hpp"
#include "viewer/core/ProcessLock.hpp"
#include "viewer/image/J2kDecoder.hpp"
#include "viewer/net/HttpClient.hpp"
#include "viewer/render/Renderer.hpp"
#include "viewer/session/Session.hpp"
#include "viewer/ui/ImGuiLayer.hpp"
#include "viewer/voice/VoiceClient.hpp"
#include "viewer/world/HomeScene.hpp"
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

#if defined(VIEWER_TEST_OPTIONS)
// For testing and measuring: only in Debug builds, or with CMake's
// VIEWER_TEST_OPTIONS on (src/app/CMakeLists.txt).
struct TestOptions
{
    std::filesystem::path loginFile;            // log in at startup (see Credentials.hpp)
    std::filesystem::path importDirectory;      // texture browser import on startup
    bool mainMenu = false;                      // --main-menu: open it at the start
    std::string mainMenuTab;                    // on this tab (--main-menu TAB)
    std::filesystem::path screenshotPath;       // save a screenshot, then quit
    float screenshotDelay = 3.0f;               // seconds after startup
    std::optional<std::array<float, 6>> camera; // x y z distance yaw pitch
    std::string teleport;                       // teleport here 5 s after logging in ("home" too)
    std::optional<float> walk; // walk this way (degrees from east) once in (after --teleport)
    // Point at this spot (fractions of the window, from the top left)
    // instead of the mouse, and click there this many seconds in.
    std::optional<std::array<float, 2>> pointer;
    std::optional<float> clickAt;
    // For finding why an avatar looks wrong: log it this many seconds in.
    std::string dumpAvatar;
    float dumpAt = 45.0f;
    bool frontView = false; // --front-view: the camera close in front of our avatar
    Uuid pointAt;           // or at this object (its center)
    // Say this in local chat this many seconds in (typing for 3 seconds
    // before).
    std::string say;
    float sayAt = 20.0f;
    // --stereo-benchmark [SIZE]: draw both eyes of a headset view, SIZE
    // pixels square each (2160), and log what it costs.
    uint32_t stereoSize = 0;
    // --without shadows,lights,probes,glow,exposure,clusters,particles,water:
    // leave passes out, to measure what they cost.
    std::vector<std::string> without;
};
#endif

// What the command line says for this run. Everything else is a setting
// (ViewerSettings.hpp), set for a run with --setting NAME VALUE.
struct AppOptions
{
    std::string loginUri;   // --grid: empty = Second Life main grid
    bool gridGiven = false; // also over a login file's
    std::string start;      // --start: last, home, Region[/x/y/z] (over login.start)
    // Whether to show the world in a headset (OpenXR).
    enum class Vr
    {
        Auto,    // if SteamVR has one
        Force,   // --force-vr: try any OpenXR runtime's, starting it
        Desktop, // --force-desktop: on the screen only
    };
    Vr vr = Vr::Auto;
    std::vector<std::string> arguments; // as given, to restart with after an update
#if defined(VIEWER_TEST_OPTIONS)
    TestOptions test;
#endif
};

// Parses command-line arguments, --setting's into `settings` (for this run,
// not saved); returns nullopt (after printing usage) on error.
std::optional<AppOptions> parseArguments(std::span<char*> args, Settings& settings);

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
    // `settings`: kept (and saved) while the app runs; most of what it
    // starts with is read from them.
    static std::unique_ptr<App> create(const AppOptions& options,
                                       std::unique_ptr<Settings> settings);
    ~App() = default;

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    int run();

private:
    App() = default;

    void drawUi();
    // Puts settings that changed (their names) into effect; those read only
    // at the start wait for the next.
    void applySettings(const std::vector<std::string>& names);
    // The antialiasing method and strength settings, into the renderer.
    void applyAntialiasing();
    void drawFpsOverlay();
    void handleLoginAction(LoginAction action);
    // Wears the Current Outfit Folder's attachments the region didn't
    // bring back (OutfitAttachments).
    void updateOutfit();
    void loadTerrainBlendRamp();
    void loadCloudTexture();
    void updateSession();
    void logOutBeforeExit();
    // Fills the frame's scene (cleared first).
    void buildScene(render::Scene& scene);
    // The same, `home`: from the home scene (in world_'s place).
    void buildSceneOf(render::Scene& scene, bool home);
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
    // developer.windows: the debugging windows (stats, the FPS overlay, the
    // texture browser, teleport, environment).
    void drawDeveloperWindows();
    bool developer_ = false;
    void drawEnvironmentWindow();
    void drawWornMeshWindow();
    std::string wornMeshText();
    // --dump-avatar: everything about one avatar's look, logged once (its
    // appearance's textures, animations, and each attachment's faces as
    // received and as drawn), to compare with the official viewer.
    void dumpAvatar(const world::Object& avatar);
    std::optional<world::ObjectKey> dumpTarget() const;

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
    // The mouse cursors made so far (the system's own), and the one shown
    // (updateCursor).
    struct CursorDeleter
    {
        void operator()(SDL_Cursor* cursor) const { SDL_DestroyCursor(cursor); }
    };
    std::array<std::unique_ptr<SDL_Cursor, CursorDeleter>, SDL_SYSTEM_CURSOR_COUNT> cursors_;
    std::optional<SDL_SystemCursor> cursorShown_;
    std::unique_ptr<render::Renderer> renderer_;
    // The headset, if any (its session goes before the GPU device). Its frame
    // under way, and each eye's camera this frame (buildScene).
    std::unique_ptr<xr::Headset> headset_;
    std::optional<xr::HeadsetFrame> headsetFrame_;
    std::array<render::Camera, 2> eyeCameras_{};
    glm::uvec2 headsetSize_{0}; // per eye, for the log
    // The headset's origin in the scene this frame (placeEyes), and what
    // follows from its tracking: the head's heading from the origin's
    // (degrees), the body's (turning once the head is far round, or to
    // where it looks when walking), the right stick's turning.
    struct Vr
    {
        bool placed = false;
        glm::vec3 base{0.0f};
        glm::quat heading{1.0f, 0.0f, 0.0f, 0.0f};
        float headYaw = 0.0f;
        std::optional<float> bodyYaw;
        bool walking = false;
        VrTurning turning; // vr.turning, vr.snap_angle, vr.turn_speed
        uint64_t turnTicks = 0;
        bool recentering = false; // the right stick held down
        // The avatar's eye height over the wearer's: the headset's
        // movements (and the eyes' distance apart) scaled by it, so a
        // smaller avatar sees the world bigger and its arms reach as far
        // as the wearer's do, in proportion.
        float scale = 1.0f;
        float loggedScale = 1.0f;
    };
    Vr vr_;
    std::optional<float> vrEyeHeight_; // vr.eye_height (nothing: measured)
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
    // The settings (settings.llsd): before the main menu, whose Settings
    // tab shows them, so they outlive it.
    std::unique_ptr<Settings> settings_;
    std::unique_ptr<ui::ImGuiLayer> ui_;
    // The viewer's own interface (RmlUi), and its main menu (before it goes).
    std::unique_ptr<gui::Gui> gui_;
    std::unique_ptr<MainMenu> mainMenu_;
    // What scripts ask of us (dialogs, permissions, web pages), and the page
    // that shows the oldest: over the window's top right on the desktop
    // (third person), on a panel beside the view in first person and VR.
    ScriptDialogs scriptDialogs_;
    std::unique_ptr<ScriptDialogPage> dialogPage_;
    bool dialogPressed_ = false; // the mouse's button went down on it
    // The VR keyboard types a text box's reply (that notice's serial), not
    // local chat.
    std::optional<uint64_t> keyboardForDialog_;
    // Or into a chat conversation (ChatView::kNearby, or a session's id).
    std::optional<std::string> keyboardForChat_;
    // Or a setting's (its name), asked for on the Settings tab.
    std::optional<std::string> keyboardForSetting_;
    // The keyboard was opened for the page that has the keyboard (a text
    // field on it took it, in a headset): what's typed goes there.
    bool keyboardForMedia_ = false;
    uint64_t mediaEditingSeen_ = 0;
    // The pick id of the face whose page has the keyboard (outlined).
    uint64_t mediaFocusPick_ = 0;
    // Text input is on for that page (SDL's, while a page has the keyboard).
    bool mediaTextInput_ = false;
    void updateScriptDialogs();
    // A script's notice or an offer waits (the notice page shows).
    bool noticeWaiting() const;
    // Local chat as typed (the Chat tab, the VR keyboard): "/5 text" on
    // channel 5.
    void say(std::string_view typed);
    bool dialogOverWindow() const;
    glm::vec4 dialogRect() const;
    bool scriptDialogEvent(const SDL_Event& event);
    // A page's text field has the keyboard (its keys aren't the avatar's
    // or the camera's); one on a panel (first person: the window's keyboard
    // typed into it), and its events.
    bool typingOnPage() const;
    gui::Surface* panelTyping() const;
    bool panelTypingEvent(const SDL_Event& event);
    // The main menu: shown over the window (the desktop, third person) or
    // on a panel (first person, VR); its values each frame, its input from
    // the window's events (true: the event was its), and where it's drawn
    // over the window (pixels: x, y, width, height).
    void updateMainMenu();
    bool mainMenuEvent(const SDL_Event& event);
    bool mainMenuOverWindow() const;
    glm::vec4 mainMenuRect() const;
    bool mainMenuPressed_ = false; // the mouse button went down on it
    std::unique_ptr<AccountPicture> accountPicture_;
    Uuid profilePicture_; // ours, this login (null: none, or not known yet)
    bool loginScreenShown() const;
    bool loginScreenEvent(const SDL_Event& event);
    bool pageIsLogin_ = false; // the VR page shows the login screen
    // The login lobby: logged out, we stand (bodiless) in the scene and walk
    // about on the ground; the login screen is a panel standing in front of
    // where we start.
    bool lobby_ = false;
    void updateLobby();
    // A headset's sticks in the lobby: the left walks where the head looks
    // (the right one turns as everywhere: run()).
    void walkLobby(const VrControls::Input& input, const glm::vec3& headForward);
    uint64_t lobbyTicks_ = 0;
    // The lobby's scene (none: the test scene on a plain floor), as a world
    // of its own, a region with this handle. Shown in the world's place
    // until we arrive (homeShown()); its markers place the start and the
    // panels.
    static constexpr uint64_t kHomeRegion = (uint64_t{256000} << 32) | uint64_t{256000};
    std::filesystem::path homeScenePath_;
    // The built-in one (viewerconfig's replaces it, unless paths.home_scene).
    std::filesystem::path builtInHomeScene_;
    bool homeSceneGiven_ = false;   // paths.home_scene
    uint64_t homeSceneVersion_ = 0; // viewerconfig's, loaded
    std::optional<world::HomeScene> home_;
    std::shared_ptr<const world::World> homeWorld_;
    void loadHomeScene();
    bool homeShown() const { return lobby_ && homeWorld_ && !originHandle_; }
    // Where the lobby starts (eye height) and which way it faces (degrees
    // from east); where the login panel stands and the point it faces.
    glm::vec3 lobbyStart() const;
    float lobbyYaw() const;
    std::pair<glm::vec3, glm::vec3> loginPanelPlace() const;
    // The viewer-asset service for every asset manager.
    void setViewerAssetUrl(const std::string& url);
    // A page over the window: `size` pixels fitted in the middle, at most
    // `fill` of the window each way, at the display's scale or smaller.
    glm::vec4 pageRect(glm::ivec2 size, float fill) const;
    // The mouse and typing, to a page over the window at `rect`; true if
    // it took the event. `pressed`: a button went down on it.
    bool pageEvent(gui::Surface& surface, const glm::vec4& rect, glm::ivec2 size, bool& pressed,
                   const SDL_Event& event);
    // The panel (first person, VR) shows `surface`.
    // The main menu's size on its panel: about 77 cm across, 70 cm away
    // (big enough to read and point at in a headset).
    static constexpr float kMenuMetersPerPixel = 0.0006f;
    void setPanelPage(gui::Surface& surface, glm::ivec2 size,
                      float metersPerPixel = kMenuMetersPerPixel);
    VrControls::Page pageOf(gui::Surface& surface, glm::ivec2 size, float metersPerPixel);
    // The lobby's boards as the VR controls' standing pages while the home
    // scene shows (again when it's read again: boardsMoved_).
    void placeBoards();
    // What a click on what the pointer's on would do, by it (a label).
    void addHoverLabel(render::Scene& scene) const;
    // First person: a dot at the view's middle, where clicks go.
    void addCenterDot(render::Scene& scene) const;
    // The mouse cursor: what the page under the mouse asks for, else a hand
    // over what a click does something to (ImGui's own over its windows).
    void updateCursor();
    bool boardsPlaced_ = false;
    bool boardsMoved_ = false;
    std::unique_ptr<JobSystem> jobs_;
    std::unique_ptr<image::J2kDecoder> j2kDecoder_;
    std::unique_ptr<DiskCache> diskCache_;                    // before everything using it
    std::unique_ptr<assets::TextureCache> textureCache_;      // texture browser panel
    std::unique_ptr<assets::TextureCache> worldTextureCache_; // textures of the world
    std::unique_ptr<TextureBrowser> textureBrowser_;
    std::unique_ptr<net::HttpClient> http_;
    std::unique_ptr<Destinations> destinations_; // the main menu's (it goes after this)
    std::unique_ptr<RegionMap> regionMap_;       // the main menu's map (likewise)
    // viewerconfig (news, grid status, the home scene, flags), and the
    // lobby's boards showing its news and status.
    std::unique_ptr<ViewerConfig> viewerConfig_;
    std::unique_ptr<LobbyBoards> boards_;
    // The login screen (RmlUi; the ImGui panel if it couldn't be made):
    // shown until we're in the world. (After http_: it asks it for the
    // terms of service, and cancels that when it goes.)
    std::unique_ptr<LoginScreen> loginScreen_;
    std::unique_ptr<Updater> updater_; // null: this build doesn't update itself
    GridPopulation population_;        // people in regions, for the main menu
    ParcelInfo parcel_;                // the parcel we're on, for the main menu
    ParcelSounds parcelSounds_;        // which sounds parcels keep in
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
    std::unique_ptr<InventoryBrowser> inventoryBrowser_; // the main menu's
    std::unique_ptr<CurrentOutfit> currentOutfit_;       // the COF, as we attach and detach
    std::unique_ptr<OutfitAttachments> outfit_;
    std::optional<double> selfSeenAt_;
    std::unordered_set<Uuid> worn_; // its items worn, as last looked (twice a second)
    std::unordered_map<Uuid, std::vector<uint32_t>> wornObjects_; // their objects (local ids)
    std::vector<OutfitAttachments::Item> outfitAttachQueue_;      // an outfit's, to put on
    double wornAt_ = -1e9;
    std::unique_ptr<LegacyMaterialManager> legacyMaterials_;
    std::unique_ptr<AvatarNames> avatarNames_;     // display names, for name tags
    ObjectMotion motion_;                          // before the views that read it
    std::unique_ptr<AnimationManager> animations_; // with avatars_
    // Sounds: their assets, and what plays (nothing without audio output).
    std::unique_ptr<SoundAssets> soundAssets_;
    std::unique_ptr<SoundManager> sounds_;
    void updateSounds(const render::Scene& scene);
    // Voice (after sounds_, whose output it plays through).
    std::unique_ptr<voice::VoiceClient> voice_;
    std::unique_ptr<MicrophoneList> microphones_; // for the Settings tab
    VoiceIndicators voiceIndicators_;
    // The microphone's own, at the bottom of the view.
    void addMicrophoneIndicator(render::Scene& scene);
    uint64_t voiceTicks_ = 0;
    bool pushToTalk_ = false;
    bool voiceFromAvatar_ = false;
    void updateVoice();
    // V, on the desktop.
    void voiceKeyEvent(const SDL_Event& event, bool consumed);
    void setMicrophone(bool on);
    // Voice's state for the main menu and the radial menu.
    std::string voiceText() const;
    // Media: the parcel's music stream (and whether it starts by itself on
    // coming onto a parcel: off once stopped, on again once played, as the
    // official viewer's), and prims' media (MOAP: listed, not shown yet).
    std::unique_ptr<MusicStream> music_;
    bool musicAutoPlay_ = true;
    std::string musicWanted_;                             // the parcel's URL we last decided on
    std::optional<std::pair<uint64_t, int>> musicParcel_; // its region and parcel
    std::unique_ptr<ObjectMedia> objectMedia_;
    // Prims' media played (a media host started when needed), and the lock
    // on the folder that host uses (one per running viewer).
    std::unique_ptr<MediaManager> media_;
    std::unique_ptr<ProcessLock> mediaLock_;
    int mediaSlot_ = 0;
    // The lobby's media (the home scene's), as ObjectMedia keeps prims'.
    struct HomeMedia
    {
        uint32_t localId = 0;
        MediaFaceKey key;
        MediaEntry entry;
    };
    std::vector<HomeMedia> homeMedia_;
    std::vector<MediaSource> mediaSources_; // (kept for its capacity)
    // Keys and text for the page with the keyboard (Escape lets it go);
    // true if it took the event.
    bool mediaKeyEvent(const SDL_Event& event);
    // The wheel over a page: the page's (scrolled), not the camera's.
    bool mediaWheelEvent(const SDL_Event& event);
    // `home`: the lobby is what's drawn (its world is world_ then).
    void updateMedia(bool home);
    uint64_t lastSoundTicks_ = 0;
    std::unique_ptr<AvatarView> avatars_; // nullptr if the default avatar couldn't be loaded
    std::unique_ptr<ObjectView> objects_;
    // Avatars' complexity and who's a jelly doll; what regions hear of it
    // and say back.
    std::unique_ptr<AvatarComplexity> complexity_;
    std::unique_ptr<AvatarRenderInfo> renderInfo_;
    uint64_t jellyRevision_ = ~0ull; // what avatars_ was last given
    // Adds up avatars' complexity (now and then) and gives AvatarView the
    // jelly dolls; tells regions and hears back.
    void updateComplexity();
    // Decides who's a jelly doll again (limits, choices or friends changed).
    void decideJellyDolls();
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
    std::string dumpAvatar_; // --dump-avatar: a name ("self": ours), or an id
    float dumpAt_ = 45.0f;
    bool dumped_ = false;
    bool frontView_ = false;
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
    // Instant messages, group and ad-hoc chats (the Chat tab shows them).
    std::unique_ptr<InstantMessages> ims_;
    // Friends (from the login reply on) and who's around (People tab).
    Friends friends_;
    bool friendsLoaded_ = false;
    Nearby nearby_;
    // What the People tab asked for.
    // Resets an avatar's skeleton; ours is announced to viewers nearby.
    void resetSkeleton(const Uuid& avatar, bool animations);
    void peopleAction(const PeopleView::Action& action);
    // Says it in a conversation (ChatView::kNearby: local chat).
    void sayIn(const std::string& conversation, std::string_view typed);
    // An object's instant message: local chat, as said to us alone.
    void objectMessage(const InstantMessages::FromObject& m);
    // Offers: what they're answered with, done (some wait for the
    // inventory's system folders to be known: since when, in seconds).
    std::unique_ptr<Offers> offers_;
    struct PendingAnswer
    {
        OfferAnswer answer;
        double since = 0.0;
    };
    std::vector<PendingAnswer> offerAnswers_;
    void updateOfferAnswers();
    // False if it waits for a folder.
    bool carryOut(const OfferAnswer& answer, double waited);
    // The folder of a folder type (-1: the root): the system folder once
    // the root's contents are known; after a while without them, the root
    // the login named. Nothing while waiting.
    std::optional<Uuid> systemFolder(int type, double waited);
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
    // pointer's on, its click action: sit if that's what it does, else
    // touch; held a moment on something that isn't touched, sit on it.
    // The radial menu offers both (menuTarget_), the click's first.
    void primaryPress();
    void primaryRelease();
    void sitOn(const PointerController::Hover& hover);
    bool sitting() const;
    // Whether a click on `hover` sits (its click action is sit, and we can).
    bool clickSits(const PointerController::Hover& hover) const;
    // What the pointer was on as the radial menu opened, and what its
    // script calls touching and sitting (llSetTouchText, llSetSitText):
    // the menu offers both, what a click on it does first.
    struct MenuTarget
    {
        PointerController::Hover hover;
        std::string touchName;
        std::string sitName;
    };
    std::optional<MenuTarget> menuTarget_;
    void radialMenuOpening();
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
    // An Alt click at window position (x, y): the camera focuses on what's
    // there (the prim under the pointer, or the ground), as the official
    // viewer's (lltoolfocus.cpp).
    void altFocus(float x, float y);
    // The camera behind our avatar, looking where it faces (on arriving, and
    // walking off from an Alt focus).
    void cameraBehindAgent();
    // The cursor held while the camera is dragged with the mouse, and where
    // the drag began (put back there after).
    void syncDragCapture();
    bool dragCaptured_ = false;
    glm::vec2 dragFrom_{0.0f};
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
    std::vector<session::RegionEvent> regionEvents_;
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
    bool showWornMesh_ = false;
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
