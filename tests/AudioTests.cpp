#include "SoundManager.hpp"
#include "TestUtil.hpp"

#include "viewer/audio/SoundClip.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

using namespace viewer;

TEST_CASE("Sound clips: Ogg Vorbis decodes to float samples")
{
    const auto bytes = test::readTestFile("tone_440hz_quarter.ogg");
    const auto clip = audio::decodeVorbis(bytes);
    REQUIRE(clip.has_value());
    CHECK(clip->channels == 1);
    CHECK(clip->sampleRate == 22050);
    CHECK(clip->seconds() == doctest::Approx(0.25).epsilon(0.05));
    float peak = 0.0f;
    for(const float s : clip->samples)
    {
        peak = std::max(peak, std::abs(s));
    }
    CHECK(peak > 0.05f); // a tone, not silence
    CHECK(peak <= 1.0f);

    const std::vector<uint8_t> junk(64, 0x42);
    CHECK_FALSE(audio::decodeVorbis(junk).has_value());
    CHECK_FALSE(audio::decodeVorbis({}).has_value());
}

namespace
{

struct OneSound : DiskCache
{
    std::vector<uint8_t> bytes = test::readTestFile("tone_440hz_quarter.ogg");
    std::optional<std::vector<uint8_t>> read(const CacheKey&) override { return bytes; }
    bool write(const CacheKey&, std::span<const uint8_t>) override { return true; }
    bool contains(const CacheKey&) override { return true; }
    void remove(const CacheKey&) override {}
    void clear(CacheKind) override {}
    void setLimit(uint64_t) override {}
    std::string describe() const override { return "test"; }
};

} // namespace

TEST_CASE("Object sounds: one-offs play once per event, loops until stopped")
{
    auto engine = audio::AudioEngine::create();
    if(!engine)
    {
        MESSAGE("no audio output; skipped");
        return;
    }
    OneSound store;
    const auto http = net::createHttpClient();
    JobSystem jobs(1);
    SoundAssets assets(*http, jobs, store);
    SoundManager sounds(std::move(engine), assets);
    sounds.setVolumes(0.0f, 0.0f);

    const world::ObjectKey key{1, 2};
    SoundManager::Source source;
    source.object = *Uuid::parse("11111111-2222-3333-4444-555555555555");
    const Uuid tone = *Uuid::parse("21111111-2222-3333-4444-555555555555");
    bool placed = true;
    // Runs frames, counting how often a sound starts.
    const auto run = [&](int ms)
    {
        int starts = 0;
        size_t before = sounds.stats().voices;
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while(std::chrono::steady_clock::now() < end)
        {
            assets.update();
            std::unordered_set<world::ObjectKey, world::ObjectKeyHash> with;
            if(!source.sound.id.isNull())
            {
                with.insert(key);
            }
            sounds.update(
                with,
                [&](const world::ObjectKey&)
                { return placed ? std::optional(source) : std::nullopt; },
                glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
            const size_t now = sounds.stats().voices;
            starts += now > before;
            before = now;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return starts;
    };

    source.sound.id = tone;
    source.events = 1;
    CHECK(run(1500) == 1);
    CHECK(sounds.stats().voices == 0);
    // Out of sight and back: not again.
    placed = false;
    run(30);
    placed = true;
    CHECK(run(300) == 0);
    // Played anew.
    source.events++;
    CHECK(run(1500) == 1);
    CHECK(sounds.stats().voices == 0);

    // A loop carries on, also after its object was away, and stops.
    source.sound.flags = world::SoundFlags::Loop;
    source.events++;
    run(800);
    CHECK(sounds.stats().voices == 1);
    placed = false;
    run(30);
    CHECK(sounds.stats().voices == 0);
    placed = true;
    run(30);
    CHECK(sounds.stats().voices == 1);
    source.sound = {};
    source.events++;
    run(30);
    CHECK(sounds.stats().voices == 0);
    CHECK(sounds.stats().objectSources == 0);

    // A loop queued behind a one-off doesn't start once stopped.
    source.sound.id = tone;
    source.events++;
    run(30);
    source.sound.flags = world::SoundFlags::Loop | world::SoundFlags::Queue;
    source.events++;
    run(20);
    source.sound = {};
    source.events++;
    run(800);
    CHECK(sounds.stats().voices == 0);
}
