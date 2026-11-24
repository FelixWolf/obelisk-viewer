#pragma once

// The viewer's settings: their keys, and their definitions (in the order
// the Settings tab shows them, page by page; hidden ones aren't shown,
// only set in the file or with --setting NAME VALUE). To add one: a key
// here, its definition in ViewerSettings.cpp, and where the app reads it
// (App::create for those read at the start, App::applySettings for those
// that apply at once).

#include "Settings.hpp"

#include <vector>

namespace viewer::setting
{

// Graphics.
inline constexpr SettingKey<bool> kShadows{"graphics.shadows"};
inline constexpr SettingKey<bool> kOcclusion{"graphics.occlusion"};
inline constexpr SettingKey<bool> kPbrTerrain{"graphics.pbr_terrain"};
inline constexpr SettingKey<int32_t> kTextureMemory{"graphics.texture_memory"}; // MiB
inline constexpr SettingKey<bool> kVsync{"graphics.vsync"};
// Sound.
inline constexpr SettingKey<int32_t> kMasterVolume{"audio.master_volume"}; // percent
inline constexpr SettingKey<int32_t> kSoundVolume{"audio.sound_volume"};
inline constexpr SettingKey<int32_t> kMusicVolume{"audio.music_volume"};
inline constexpr SettingKey<bool> kMusicAutoPlay{"audio.music_autoplay"};
// World.
inline constexpr SettingKey<std::string> kStart{"login.start"}; // "last", "home"
inline constexpr SettingKey<Uuid> kSkyAsset{"world.sky_asset"}; // null: the region's
// Network.
inline constexpr SettingKey<int32_t> kBandwidth{"network.bandwidth"}; // kbps
inline constexpr SettingKey<std::string> kViewerConfig{"network.viewerconfig"};
inline constexpr SettingKey<bool> kAutoUpdate{"network.auto_update"};
// VR.
inline constexpr SettingKey<double> kVrOverscan{"vr.overscan"}; // percent a side
// Developer.
inline constexpr SettingKey<bool> kDeveloper{"developer.windows"};

// Hidden (not on the Settings tab: the file, or --setting NAME VALUE).
inline constexpr SettingKey<std::string> kCacheDirectory{"paths.cache"}; // empty: the platform's
inline constexpr SettingKey<std::string> kUiDirectory{"paths.ui"};       // empty: assets/ui
inline constexpr SettingKey<std::string> kHomeScene{"paths.home_scene"}; // empty: the usual
inline constexpr SettingKey<double> kVrEyeHeight{"vr.eye_height"};       // m; -1 measured
inline constexpr SettingKey<bool> kFirstPerson{"camera.first_person"};   // at the start
inline constexpr SettingKey<double> kDayTime{"world.day_time"};          // 0..1; -1 the region's
inline constexpr SettingKey<bool> kTestScene{"developer.test_scene"};

} // namespace viewer::setting

namespace viewer
{

std::vector<SettingDefinition> viewerSettings();

} // namespace viewer
