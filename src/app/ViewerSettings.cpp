#include "ViewerSettings.hpp"

#include "viewer/session/Session.hpp"

#include <cmath>

namespace viewer
{

std::vector<SettingDefinition> viewerSettings()
{
    using namespace setting;
    return {
        // --- Graphics ---------------------------------------------------------------
        defineSetting(kShadows, true,
                      {.page = "Graphics",
                       .section = "Lighting",
                       .label = "Sun and moon shadows",
                       .description = "Shadows cast by the sun or moon."}),
        defineSetting(kPbrTerrain, false,
                      {.page = "Graphics",
                       .label = "PBR terrain shader test",
                       .description = "Draw every region's terrain with built-in procedural "
                                      "PBR layers in place of its own textures or materials, "
                                      "to test the PBR terrain shader. Regions with PBR "
                                      "terrain draw as PBR without it.",
                       .hidden = true}),
        defineSetting(kOcclusion, true,
                      {.page = "Graphics",
                       .section = "Performance",
                       .label = "Occlusion culling",
                       .description = "Skip drawing what's hidden behind walls and hills."}),
        defineSetting(kTextureMemory, int32_t{2048},
                      {.page = "Graphics",
                       .label = "Texture memory",
                       .description = "GPU memory textures may use before the farthest are "
                                      "made smaller.",
                       .minimum = 256,
                       .maximum = 16384,
                       .step = 256,
                       .unit = "MiB"}),
        defineSetting(kVsync, true,
                      {.page = "Graphics",
                       .label = "Vertical sync",
                       .description = "Show frames in step with the display, no faster than "
                                      "it refreshes.",
                       .restart = true}),

        // --- Sound ------------------------------------------------------------------
        defineSetting(kMasterVolume, int32_t{100},
                      {.page = "Sound",
                       .section = "Volume",
                       .label = "Master",
                       .description = "Everything the viewer plays.",
                       .minimum = 0,
                       .maximum = 100,
                       .step = 5,
                       .unit = "%"}),
        defineSetting(kSoundVolume, int32_t{100},
                      {.page = "Sound",
                       .label = "Sounds",
                       .description = "Objects' sounds, footsteps and wind.",
                       .minimum = 0,
                       .maximum = 100,
                       .step = 5,
                       .unit = "%"}),
        defineSetting(kMusicVolume, int32_t{50},
                      {.page = "Sound",
                       .label = "Music",
                       .description = "Parcels' music streams (also set on the Media tab).",
                       .minimum = 0,
                       .maximum = 100,
                       .step = 5,
                       .unit = "%"}),
        defineSetting(kMusicAutoPlay, true,
                      {.page = "Sound",
                       .section = "Music",
                       .label = "Play parcel music by itself",
                       .description = "Start a parcel's music on arriving (off: it waits for "
                                      "Play on the Media tab)."}),

        // --- World ------------------------------------------------------------------
        defineSetting(kStart, std::string("last"),
                      {.page = "World",
                       .section = "Logging in",
                       .label = "Start at",
                       .description = "Where logging in takes you.",
                       .choices = {{"last", "Last location"}, {"home", "Home"}}}),
        defineSetting(kSkyAsset, Uuid(),
                      {.page = "World",
                       .section = "Environment",
                       .label = "Fixed sky",
                       .description = "A sky settings asset's id, to see everywhere instead of "
                                      "the region's sky (empty: the region's).",
                       .allowNull = true}),

        // --- Network ----------------------------------------------------------------
        defineSetting(kBandwidth, static_cast<int32_t>(std::lround(session::kDefaultBandwidthKbps)),
                      {.page = "Network",
                       .label = "Bandwidth",
                       .description = "How much regions may send us a second.",
                       .minimum = 100,
                       .maximum = 10000,
                       .step = 100,
                       .unit = "kbps"}),
        defineSetting(kViewerConfig, std::string("https://viewerconfig.coyoteware.com/api/v1/"),
                      {.page = "Network",
                       .label = "Viewer news and status service",
                       .description =
                           "Its .../api/v1/ address, or \"off\" to use only what's saved on "
                           "disk.",
                       .maxLength = 512,
                       .check = [](std::string_view s) -> std::string
                       {
                           if(s == "off" || s.starts_with("https://") || s.starts_with("http://"))
                           {
                               return {};
                           }
                           return "Must be an http(s):// address, or off.";
                       },
                       .restart = true}),

        // --- VR ---------------------------------------------------------------------
        defineSetting(kVrOverscan, 10.0,
                      {.page = "VR",
                       .label = "Overscan",
                       .description = "How much wider than the headset's view each eye is "
                                      "drawn, a side, so turning quickly shows no edges.",
                       .minimum = 0.0,
                       .maximum = 50.0,
                       .step = 1.0,
                       .unit = "%",
                       .decimals = 0,
                       .restart = true}),

        // --- Developer --------------------------------------------------------------
        defineSetting(kDeveloper, false,
                      {.page = "Developer",
                       .label = "Developer windows",
                       .description = "Statistics, the texture browser, teleport and "
                                      "environment windows."}),

        // --- Hidden: set in the file, or for a run with --setting ---------------------
        defineSetting(kCacheDirectory, std::string(),
                      {.page = "Paths",
                       .label = "Cache folder",
                       .description = "Where downloaded assets are kept (empty: the platform's "
                                      "cache folder).",
                       .restart = true,
                       .hidden = true}),
        defineSetting(kUiDirectory, std::string(),
                      {.page = "Paths",
                       .label = "Interface documents",
                       .description = "The folder the interface's pages are read from (empty: "
                                      "assets/ui beside the program; the source's, to edit them "
                                      "while running).",
                       .restart = true,
                       .hidden = true}),
        defineSetting(kHomeScene, std::string(),
                      {.page = "Paths",
                       .label = "Lobby scene",
                       .description = "The lobby's scene file (empty: the viewerconfig "
                                      "service's, or assets/home.scene beside the program).",
                       .restart = true,
                       .hidden = true}),
        defineSetting(kVrEyeHeight, -1.0,
                      {.page = "VR",
                       .label = "Eye height",
                       .description = "The wearer's eye height standing, which the headset's "
                                      "movements are scaled from to the avatar's (-1: measured "
                                      "from the floor; 0: not scaled).",
                       .minimum = -1.0,
                       .maximum = 3.0,
                       .step = 0.05,
                       .unit = "m",
                       .decimals = 2,
                       .restart = true,
                       .hidden = true}),
        defineSetting(kFirstPerson, false,
                      {.page = "World",
                       .label = "Start in first person",
                       .description = "Go first person once in the world.",
                       .restart = true,
                       .hidden = true}),
        defineSetting(kDayTime, -1.0,
                      {.page = "World",
                       .label = "Fixed time of day",
                       .description = "0 to 1 through the day cycle (0.5 is midday in most); -1: "
                                      "the region's time.",
                       .minimum = -1.0,
                       .maximum = 1.0,
                       .step = 0.05,
                       .decimals = 2,
                       .hidden = true}),
        defineSetting(kTestScene, false,
                      {.page = "Developer",
                       .label = "Material test scene",
                       .description = "Show the material test scene.",
                       .hidden = true}),
    };
}

} // namespace viewer
