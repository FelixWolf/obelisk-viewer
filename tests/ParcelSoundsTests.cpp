#include "ParcelSounds.hpp"

#include <doctest/doctest.h>

#include <vector>

using namespace viewer;

namespace
{

constexpr uint64_t kRegion = uint64_t{256000} << 32 | 256256; // a region's corner
constexpr uint64_t kEast = uint64_t{256256} << 32 | 256256;   // the one east of it
constexpr int kEdge = ParcelSounds::kCellsPerEdge;

// A region's overlay, its cells west of x = 128 m one parcel and the rest
// another, given in its four parts.
void giveOverlay(ParcelSounds& sounds, uint64_t region, uint8_t west, uint8_t east)
{
    std::vector<uint8_t> cells(kEdge * kEdge);
    for(int i = 0; i < kEdge * kEdge; i++)
    {
        cells[static_cast<size_t>(i)] = i % kEdge < kEdge / 2 ? west : east;
    }
    const size_t part = cells.size() / ParcelSounds::kOverlayParts;
    for(int p = 0; p < ParcelSounds::kOverlayParts; p++)
    {
        sounds.overlay(region, p, std::span(cells).subspan(static_cast<size_t>(p) * part, part));
    }
}

// The agent's parcel: the west half of kRegion.
std::vector<uint8_t> westHalf()
{
    std::vector<uint8_t> bitmap(kEdge * kEdge / 8);
    for(int i = 0; i < kEdge * kEdge; i++)
    {
        if(i % kEdge < kEdge / 2)
        {
            bitmap[static_cast<size_t>(i / 8)] |= static_cast<uint8_t>(1u << (i % 8));
        }
    }
    return bitmap;
}

} // namespace

TEST_CASE("Parcel sounds: heard everywhere until parcels say otherwise")
{
    ParcelSounds sounds;
    CHECK(sounds.canHear(kRegion, {10.0f, 10.0f}));
    CHECK(sounds.canHear(kRegion, {200.0f, 10.0f}));
}

TEST_CASE("Parcel sounds: a parcel keeping its sounds in isn't heard from outside")
{
    ParcelSounds sounds;
    giveOverlay(sounds, kRegion, 0, ParcelSounds::kSoundLocal);
    sounds.agentParcel(kRegion, westHalf(), false);
    CHECK(sounds.canHear(kRegion, {10.0f, 10.0f}));        // our own parcel
    CHECK_FALSE(sounds.canHear(kRegion, {200.0f, 10.0f})); // the one keeping them in
    CHECK_FALSE(sounds.canHear(kRegion, {130.0f, 250.0f}));
}

TEST_CASE("Parcel sounds: on a parcel keeping sounds in, others' aren't heard")
{
    ParcelSounds sounds;
    giveOverlay(sounds, kRegion, ParcelSounds::kSoundLocal, 0);
    giveOverlay(sounds, kEast, 0, 0);
    sounds.agentParcel(kRegion, westHalf(), true);
    CHECK(sounds.canHear(kRegion, {127.9f, 255.0f}));
    CHECK_FALSE(sounds.canHear(kRegion, {128.0f, 10.0f}));
    CHECK_FALSE(sounds.canHear(kEast, {10.0f, 10.0f})); // another region
    // Global meters, and past a region's edge: the region there.
    CHECK(sounds.canHear(glm::dvec3(256000.0 + 20.0, 256256.0 + 20.0, 30.0)));
    CHECK_FALSE(sounds.canHear(glm::dvec3(256256.0 + 20.0, 256256.0 + 20.0, 30.0)));
    CHECK_FALSE(sounds.canHear(kRegion, {260.0f, 10.0f}));

    // Moved to a parcel that doesn't: heard again.
    sounds.agentParcel(kEast, std::vector<uint8_t>(kEdge * kEdge / 8, 0xFF), false);
    CHECK(sounds.canHear(kRegion, {200.0f, 10.0f}));
    // Except from the one keeping them in.
    CHECK_FALSE(sounds.canHear(kRegion, {10.0f, 10.0f}));

    sounds.clear();
    CHECK(sounds.canHear(kRegion, {10.0f, 10.0f}));
}

TEST_CASE("Parcel sounds: overlays of another size and a missing bitmap are left out")
{
    ParcelSounds sounds;
    std::vector<uint8_t> big(2048, ParcelSounds::kSoundLocal);
    sounds.overlay(kRegion, 0, big);
    CHECK(sounds.canHear(kRegion, {10.0f, 10.0f}));
    // Keeping sounds in, but where it is not said: nothing kept out.
    sounds.agentParcel(kRegion, {}, true);
    CHECK(sounds.canHear(kRegion, {10.0f, 10.0f}));
    sounds.removeRegion(kRegion);
}
