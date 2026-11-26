#include "SoundManager.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace viewer
{

namespace
{

// How long a sound may take to load and still play: triggered ones and
// objects' one-offs (llviewermessage.cpp waits 15 s for an object).
constexpr auto kTriggerWait = std::chrono::seconds(10);
constexpr auto kObjectWait = std::chrono::seconds(15);

// Footsteps (llvoavatar.cpp sStepSoundOnLand, sStepSounds by LL_MCODE_*).
constexpr float kStepGain = 0.1f;
const Uuid kStepOnLand = *Uuid::parse("e8af4a28-aa83-4310-a7c4-c047e15ea0df");
const Uuid kStepOn[] = {
    *Uuid::parse("25a863e8-dc42-4e8a-a357-e76422ace9b5"), // stone
    *Uuid::parse("8761f73f-6cf9-4186-8aaa-0948ed002db1"), // metal
    *Uuid::parse("2de4da5a-faf8-46be-bac6-c4d74f1e5767"), // glass
    *Uuid::parse("7d1826f4-24c4-4aac-8c2e-eff45df37783"), // wood
    *Uuid::parse("68d62208-e257-4d0c-bbe2-20c9ea9760bb"), // flesh
    *Uuid::parse("75cf3ade-9a5b-4c4d-bb35-f9799bda7fb2"), // plastic
    *Uuid::parse("153c8bf7-fb89-4d89-b263-47e58b1b4774"), // rubber
};
// The weather's share of the wind sound (AudioLevelWind), and how long it
// takes to fade in or out (WIND_SOUND_TRANSITION_TIME).
constexpr float kWeatherWind = 0.5f;
constexpr float kWindFade = 2.0f;

} // namespace

SoundManager::SoundManager(std::unique_ptr<audio::AudioEngine> engine, SoundAssets& assets)
    : engine_(std::move(engine)), assets_(assets)
{
}

SoundManager::~SoundManager() = default;

void SoundManager::trigger(const Uuid& sound, const glm::vec3& position, float gain)
{
    if(!engine_ || sound.isNull())
    {
        return;
    }
    assets_.get(sound);
    triggers_.push_back({sound, position, std::clamp(gain, 0.0f, 1.0f), Clock::now()});
}

void SoundManager::footstep(const glm::vec3& position, std::optional<uint8_t> material)
{
    const Uuid& sound = !material ? kStepOnLand : kStepOn[std::min<size_t>(*material, 6)];
    trigger(sound, position, kStepGain);
}

void SoundManager::updateWind(const glm::vec3& weather, const glm::vec3& velocity, bool flying,
                              float seconds)
{
    // Walking north, the air goes south past us.
    const glm::vec3 wind = weather * kWeatherWind - velocity;
    if(!engine_)
    {
        return;
    }
    // Fades in while flying, out otherwise.
    const float most = master_ > 0.0f ? 1.0f : 0.0f; // (the master volume applies on top)
    const float delta = seconds / kWindFade * most;
    windVolume_ =
        flying ? std::min(windVolume_ + delta, most) : std::max(windVolume_ - delta, 0.0f);
    if(windVolume_ <= 0.0f && !flying)
    {
        engine_->setWind(0.0f, 100.0f, 0.5f);
        return;
    }
    // In the listener's frame: how fast (to 20 m/s), and the angle from its
    // right (0 from the right, 1 from the left); straight on or from behind
    // sounds highest.
    const float speed = glm::length(wind);
    const float strength = std::min(speed, 20.0f) / 20.0f;
    const glm::vec3 right = glm::normalize(glm::cross(forward_, up_));
    const float side = speed > 1e-4f ? glm::dot(wind / speed, right) : 0.0f;
    const float theta = std::acos(std::clamp(side, -1.0f, 1.0f)) / 3.14159265f;
    const float across = theta > 0.5f ? 1.0f - theta : theta;
    const float frequency = 80.0f * std::pow(1.0f + across, 2.5f * (strength + 1.0f));
    engine_->setWind(strength * windVolume_, frequency, theta);
}

void SoundManager::setVolumes(float master, float sounds)
{
    master_ = std::clamp(master, 0.0f, 1.0f);
    sounds_ = std::clamp(sounds, 0.0f, 1.0f);
    if(engine_)
    {
        engine_->setVolume(master_);
    }
}

float SoundManager::objectGain(const Playing& p, const Source& s) const
{
    if(!s.audible)
    {
        return 0.0f;
    }
    // Silent beyond its cutoff radius, if it has one (under 0.1 m: none).
    if(s.sound.radius >= 0.1f && !s.hud && glm::length(s.position - listener_) >= s.sound.radius)
    {
        return 0.0f;
    }
    return std::clamp(p.gain, 0.0f, 1.0f) * sounds_;
}

void SoundManager::set(Playing& p, const Source& s)
{
    const world::ObjectSound& sound = s.sound;
    p.gain = sound.gain;
    if(sound.id.isNull())
    {
        // A loop (or one queued) stops; anything else only with the stop flag.
        const bool queuedLoop = !p.queued.isNull() && p.queuedLoop;
        if(p.loop || queuedLoop || (sound.flags & world::SoundFlags::Stop))
        {
            engine_->stop(p.voice);
            p.voice = 0;
            p.sound = Uuid();
            p.started = false;
            p.queued = Uuid();
        }
        return;
    }
    const bool loop = (sound.flags & world::SoundFlags::Loop) != 0;
    if(loop && p.loop && p.sound == sound.id)
    {
        return; // the same loop carries on
    }
    // Queued: after what plays now (a loop finishes its round first).
    if((sound.flags & world::SoundFlags::Queue) && p.voice && engine_->playing(p.voice))
    {
        p.queued = sound.id;
        p.queuedLoop = loop;
        engine_->setLooping(p.voice, false);
        assets_.get(sound.id);
        return;
    }
    engine_->stop(p.voice);
    p.voice = 0;
    p.sound = sound.id;
    p.loop = loop;
    p.started = false;
    p.asked = Clock::now();
    p.queued = Uuid();
    assets_.get(sound.id);
}

void SoundManager::startPlaying(Playing& p, const Source& s, float gain)
{
    const auto clip = assets_.get(p.sound);
    if(!clip)
    {
        // Still loading: one-offs give up after a while.
        if(assets_.failed(p.sound) || (!p.loop && Clock::now() - p.asked > kObjectWait))
        {
            p.sound = Uuid();
        }
        return;
    }
    audio::AudioEngine::PlayParams params;
    params.position = s.hud ? listener_ : s.position;
    params.gain = gain;
    params.loop = p.loop;
    p.voice = engine_->play(clip, params);
    p.started = p.voice != 0;
    if(!p.started && !p.loop && Clock::now() - p.asked > kObjectWait)
    {
        p.sound = Uuid(); // no voice free in time (loops keep trying)
    }
}

void SoundManager::update(
    const std::unordered_set<world::ObjectKey, world::ObjectKeyHash>& withSound,
    const Locate& locate, const glm::vec3& listener, const glm::vec3& forward, const glm::vec3& up)
{
    if(!engine_)
    {
        return;
    }
    listener_ = listener;
    forward_ = forward;
    up_ = up;
    engine_->setListener(listener, forward, up);
    engine_->update();

    // Sounds triggered at a place, once they're loaded.
    const auto now = Clock::now();
    std::erase_if(triggers_,
                  [&](const Trigger& t)
                  {
                      if(const auto clip = assets_.get(t.sound))
                      {
                          audio::AudioEngine::PlayParams params;
                          params.position = t.position;
                          params.gain = t.gain * sounds_;
                          engine_->play(clip, params);
                          return true;
                      }
                      return assets_.failed(t.sound) || now - t.asked > kTriggerWait;
                  });

    // Objects' sounds: those with one now, and those playing (to notice
    // theirs stopping or the object going).
    std::vector<world::ObjectKey> keys(withSound.begin(), withSound.end());
    for(const auto& [key, playing] : objects_)
    {
        if(!withSound.contains(key))
        {
            keys.push_back(key);
        }
    }
    for(const world::ObjectKey& key : keys)
    {
        const std::optional<Source> source = locate(key);
        auto it = objects_.find(key);
        if(!source)
        {
            // Gone, or not placed for now (its parent isn't here): then it's
            // remembered, so only a loop starts again once it's back.
            if(it != objects_.end())
            {
                Playing& p = it->second;
                engine_->stop(p.voice);
                p.voice = 0;
                p.started = false;
                if(!p.queued.isNull())
                {
                    p.sound = std::exchange(p.queued, Uuid());
                    p.loop = p.queuedLoop;
                }
                if(!p.loop)
                {
                    p.sound = Uuid();
                }
                if(!withSound.contains(key))
                {
                    objects_.erase(it);
                }
            }
            continue;
        }
        if(it != objects_.end() && it->second.object != source->object)
        {
            // Another object in its place.
            engine_->stop(it->second.voice);
            objects_.erase(it);
            it = objects_.end();
        }
        if(it == objects_.end())
        {
            if(source->sound.id.isNull())
            {
                continue;
            }
            Playing fresh;
            fresh.object = source->object;
            fresh.events = source->events - 1; // its sound applies below
            it = objects_.emplace(key, fresh).first;
        }
        Playing& p = it->second;
        if(p.events != source->events)
        {
            p.events = source->events;
            set(p, *source);
        }
        else
        {
            p.gain = source->sound.gain; // AttachedSoundGainChange
        }
        const float gain = objectGain(p, *source);
        // Ended: the queued one next; a one-off is done.
        if(p.started && !engine_->playing(p.voice))
        {
            engine_->stop(p.voice);
            p.voice = 0;
            p.started = false;
            if(!p.queued.isNull())
            {
                p.sound = std::exchange(p.queued, Uuid());
                p.loop = p.queuedLoop;
                p.asked = now;
            }
            else
            {
                p.sound = Uuid();
            }
        }
        if(!p.started && !p.sound.isNull())
        {
            startPlaying(p, *source, gain);
        }
        if(p.voice)
        {
            engine_->setPosition(p.voice, source->hud ? listener : source->position);
            engine_->setGain(p.voice, gain);
        }
        // Done. While the object still names a sound (it does after a one-off
        // ends) it's remembered, or it'd be taken for new and play again.
        if(p.sound.isNull() && !p.voice && p.queued.isNull() && source->sound.id.isNull())
        {
            objects_.erase(it);
        }
    }
}

void SoundManager::clear()
{
    if(engine_)
    {
        for(auto& [key, p] : objects_)
        {
            engine_->stop(p.voice);
        }
    }
    objects_.clear();
    triggers_.clear();
}

SoundManager::Stats SoundManager::stats() const
{
    return {engine_ ? engine_->voices() : 0, objects_.size(), triggers_.size()};
}

} // namespace viewer
