#pragma once

// Whether a sound can be heard across parcel borders: "Restrict sounds
// and gestures to this parcel" (PF_SOUND_LOCAL), as the official viewer
// keeps it (llviewerparcelmgr.cpp canHearSound):
// - A sound on the agent's own parcel is heard.
// - Otherwise not, if the agent's parcel keeps its sounds in, or the
//   sound's parcel keeps its own in.
// The agent's parcel is its cells (ParcelProperties' Bitmap); other
// parcels' setting comes from each region's overlay (ParcelOverlay: a
// byte per 4 m cell, in four parts, PARCEL_SOUND_LOCAL among its bits).
// Until they're known, sounds are heard.
//
// No messages of its own: the app gives it the overlays and the agent's
// parcel. Per session (clear() between).

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace viewer
{

class ParcelSounds
{
public:
    // A 4 m cell's byte in a region's overlay (llparcel.h PARCEL_*).
    static constexpr uint8_t kSoundLocal = 0x20;
    static constexpr int kCellsPerEdge = 64; // a 256 m region's
    static constexpr int kOverlayParts = 4;

    // ParcelOverlay from a region: one of its parts (SequenceID 0..3, a
    // quarter of its rows each, south first).
    void overlay(uint64_t region, int part, std::span<const uint8_t> data);
    // The region's gone: its overlay with it.
    void removeRegion(uint64_t region);
    // The parcel the agent's on: its region, its cells (a bit each, west
    // to east then south to north) and whether it keeps sounds in.
    void agentParcel(uint64_t region, std::span<const uint8_t> bitmap, bool soundLocal);

    // Whether a sound at this place is heard: a region, and meters into it
    // (past its edges: in the region there), or global meters.
    bool canHear(uint64_t region, glm::vec2 local) const;
    bool canHear(const glm::dvec3& global) const;

    void clear();

private:
    using Cells = std::array<uint8_t, kCellsPerEdge * kCellsPerEdge>;
    static int cell(glm::vec2 local);
    bool canHearCell(uint64_t region, int cell) const;
    bool inAgentParcel(uint64_t region, int cell) const;

    std::unordered_map<uint64_t, Cells> overlays_;
    uint64_t agentRegion_ = 0;
    std::vector<bool> agentCells_; // empty: not known
    bool agentSoundLocal_ = false;
};

} // namespace viewer
