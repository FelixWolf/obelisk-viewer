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
inline constexpr SettingKey<std::string> kAntialiasing{
    "graphics.antialiasing"}; // "off", "2x", "4x"
inline constexpr SettingKey<bool> kVsync{"graphics.vsync"};
// Sound.
inline constexpr SettingKey<int32_t> kMasterVolume{"audio.master_volume"}; // percent
inline constexpr SettingKey<int32_t> kSoundVolume{"audio.sound_volume"};
inline constexpr SettingKey<int32_t> kMusicVolume{"audio.music_volume"};
inline constexpr SettingKey<bool> kMusicAutoPlay{"audio.music_autoplay"};
// Voice.
inline constexpr SettingKey<bool> kVoice{"voice.enabled"};
inline constexpr SettingKey<int32_t> kVoiceVolume{"voice.volume"};     // percent
inline constexpr SettingKey<std::string> kVoiceEar{"voice.hear_from"}; // "camera", "avatar"
inline constexpr SettingKey<bool> kVoiceIndicators{"voice.indicators"};
inline constexpr SettingKey<bool> kMicIndicator{"voice.mic_indicator"};
inline constexpr SettingKey<bool> kPushToTalk{"voice.push_to_talk"};
inline constexpr SettingKey<std::string> kMicrophone{"voice.microphone"}; // empty: the default
inline constexpr SettingKey<bool> kVoiceEcho{"voice.echo_cancellation"};
inline constexpr SettingKey<bool> kVoiceNoise{"voice.noise_suppression"};
inline constexpr SettingKey<bool> kVoiceLevel{"voice.auto_level"};
// World.
inline constexpr SettingKey<std::string> kStart{"login.start"}; // "last", "home"
inline constexpr SettingKey<Uuid> kSkyAsset{"world.sky_asset"}; // null: the region's
// Network.
inline constexpr SettingKey<int32_t> kBandwidth{"network.bandwidth"}; // kbps
inline constexpr SettingKey<std::string> kViewerConfig{"network.viewerconfig"};
inline constexpr SettingKey<int32_t> kCacheLimit{"network.cache_limit"}; // GiB; 0 none
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
