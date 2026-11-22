#include "TestUtil.hpp"

#include "viewer/audio/SoundClip.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

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
