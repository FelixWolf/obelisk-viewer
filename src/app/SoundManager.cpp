#include "SoundManager.hpp"

#include <glm/geometric.hpp>

#include <algorithm>

namespace viewer
{

namespace
{

// How long a sound may take to load and still play: triggered ones and
// objects' one-offs (llviewermessage.cpp waits 15 s for an object).
constexpr auto kTriggerWait = std::chrono::seconds(10);
constexpr auto kObjectWait = std::chrono::seconds(15);

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
        // A loop stops; anything else only with the stop flag.
        if(p.loop || (sound.flags & world::SoundFlags::Stop))
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
    p.started = true;
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
        if(!source || (it != objects_.end() && it->second.object != source->object))
        {
            // Gone (or another object in its place).
            if(it != objects_.end())
            {
                engine_->stop(it->second.voice);
                objects_.erase(it);
            }
            if(!source)
            {
                continue;
            }
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
        if(p.sound.isNull() && !p.voice && p.queued.isNull())
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
