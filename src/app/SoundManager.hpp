#pragma once

// What the world sounds like, as the official viewer plays it
// (llaudioengine.cpp, llviewerobject.cpp setAttachedSound,
// llaudiosourcevo.cpp, llviewermessage.cpp):
// - Sounds triggered at a place (SoundTrigger: llTriggerSound, gestures,
//   collisions) play once there.
// - Objects' own sounds (llPlaySound, llLoopSound, ...) follow the object.
//   Each object update, and AttachedSound, sets its sound again: a null
//   sound stops a loop (or anything, with the stop flag); the same loop
//   carries on; anything else stops what played (unless queued: then it
//   plays once that ends) and starts. AttachedSoundGainChange only changes
//   the gain. Beyond the object's cutoff radius (llSetSoundRadius) it's
//   silent.
// - Distance: OpenAL's inverse distance as the official viewer's (reference
//   1 m, rolloff 1).
// - The listener is the camera.
// - Footsteps: a step's sound where a foot came down, by what's underfoot
//   (land, or the object's physical material), quietly (llvoavatar.cpp).
// - Wind: noise around the listener while flying, louder and higher the
//   faster the air goes past (the region's wind at half strength, less our
//   own motion), from the side it comes from (llvieweraudio.cpp,
//   llaudioengine.cpp mapWindVecTo*).
// Sync (llLoopSoundMaster/Slave) plays as plain loops for now.

#include "SoundAssets.hpp"

#include "viewer/audio/AudioEngine.hpp"
#include "viewer/world/Objects.hpp"
#include "viewer/world/World.hpp"

#include <glm/vec3.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace viewer
{

class SoundManager
{
public:
    // `engine` may be null (no output): then nothing plays.
    SoundManager(std::unique_ptr<audio::AudioEngine> engine, SoundAssets& assets);
    ~SoundManager();

    bool enabled() const { return engine_ != nullptr; }
    // The output (null: none), for the music stream.
    audio::AudioEngine* engine() const { return engine_.get(); }

    // A sound played once at a place (scene coordinates), at a gain.
    void trigger(const Uuid& sound, const glm::vec3& position, float gain);
    // A foot came down here, on an object of this physical material
    // (nothing: land).
    void footstep(const glm::vec3& position, std::optional<uint8_t> material);
    // Once a frame: the region's wind where we are and our own velocity
    // (scene axes), whether we fly, and the frame's length.
    void updateWind(const glm::vec3& weather, const glm::vec3& velocity, bool flying,
                    float seconds);
    // Loads it ahead (PreloadSound).
    void preload(const Uuid& sound) { assets_.get(sound); }

    // An object's sound as it stands, and where it is (scene coordinates;
    // `hud`: on the screen, so where the listener is).
    struct Source
    {
        Uuid object;
        world::ObjectSound sound;
        uint32_t events = 0;
        glm::vec3 position{0.0f};
        bool hud = false;
    };
    using Locate = std::function<std::optional<Source>(const world::ObjectKey&)>;
    // Once a frame: objects with a sound (and those playing are looked up
    // too, to notice theirs stopping), the listener, and the volumes.
    void update(const std::unordered_set<world::ObjectKey, world::ObjectKeyHash>& withSound,
                const Locate& locate, const glm::vec3& listener, const glm::vec3& forward,
                const glm::vec3& up);

    // 0..1: everything, and objects' sounds.
    void setVolumes(float master, float sounds);

    // Logged out (or teleported): everything stops.
    void clear();

    struct Stats
    {
        size_t voices = 0;
        size_t objectSources = 0;
        size_t pendingTriggers = 0;
    };
    Stats stats() const;

private:
    using Clock = std::chrono::steady_clock;
    using Voice = audio::AudioEngine::Voice;
    // An object's sound, playing or (kept while the object names one) done.
    struct Playing
    {
        Uuid object;
        uint32_t events = 0;
        Uuid sound; // what plays (or will, once loaded)
        Voice voice = 0;
        bool loop = false;
        bool started = false;
        Clock::time_point asked{};
        // Queued to play once this one ends.
        Uuid queued;
        bool queuedLoop = false;
        float gain = 1.0f;
    };
    struct Trigger
    {
        Uuid sound;
        glm::vec3 position{0.0f};
        float gain = 1.0f;
        Clock::time_point asked{};
    };

    // setAttachedSound for this source.
    void set(Playing& p, const Source& s);
    void startPlaying(Playing& p, const Source& s, float gain);
    float objectGain(const Playing& p, const Source& s) const;

    std::unique_ptr<audio::AudioEngine> engine_;
    SoundAssets& assets_;
    std::unordered_map<world::ObjectKey, Playing, world::ObjectKeyHash> objects_;
    std::vector<Trigger> triggers_; // waiting for their sounds to load
    glm::vec3 listener_{0.0f};
    glm::vec3 forward_{0.0f, 1.0f, 0.0f};
    glm::vec3 up_{0.0f, 0.0f, 1.0f};
    float windVolume_ = 0.0f; // fades in while flying
    float master_ = 1.0f;
    float sounds_ = 1.0f;
};

} // namespace viewer
