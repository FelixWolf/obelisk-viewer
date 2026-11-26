#include "ParcelSounds.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace viewer
{

void ParcelSounds::overlay(uint64_t region, int part, std::span<const uint8_t> data)
{
    constexpr size_t kPart = kCellsPerEdge * kCellsPerEdge / kOverlayParts;
    // Another size: a region of another width (not drawn either).
    if(part < 0 || part >= kOverlayParts || data.size() != kPart)
    {
        return;
    }
    auto [it, fresh] = overlays_.try_emplace(region);
    if(fresh)
    {
        it->second.fill(0);
    }
    std::memcpy(it->second.data() + static_cast<size_t>(part) * kPart, data.data(), kPart);
}

void ParcelSounds::removeRegion(uint64_t region)
{
    overlays_.erase(region);
}

void ParcelSounds::agentParcel(uint64_t region, std::span<const uint8_t> bitmap, bool soundLocal)
{
    agentRegion_ = region;
    agentSoundLocal_ = soundLocal;
    agentCells_.clear();
    if(bitmap.size() * 8 == kCellsPerEdge * kCellsPerEdge)
    {
        agentCells_.resize(bitmap.size() * 8);
        for(size_t i = 0; i < agentCells_.size(); i++)
        {
            agentCells_[i] = (bitmap[i / 8] >> (i % 8) & 1) != 0;
        }
    }
}

int ParcelSounds::cell(glm::vec2 local)
{
    const auto index = [](float meters)
    {
        const int i = static_cast<int>(std::floor(meters / 4.0f));
        return std::clamp(i, 0, kCellsPerEdge - 1);
    };
    return index(local.y) * kCellsPerEdge + index(local.x);
}

bool ParcelSounds::inAgentParcel(uint64_t region, int cell) const
{
    return region == agentRegion_ && !agentCells_.empty() && agentCells_[static_cast<size_t>(cell)];
}

bool ParcelSounds::canHear(uint64_t region, glm::vec2 local) const
{
    return canHear(glm::dvec3(static_cast<double>(region >> 32) + local.x,
                              static_cast<double>(region & 0xFFFFFFFFu) + local.y, 0.0));
}

bool ParcelSounds::canHear(const glm::dvec3& global) const
{
    constexpr double kWidth = kCellsPerEdge * 4.0;
    if(!std::isfinite(global.x) || !std::isfinite(global.y) || global.x < 0.0 || global.y < 0.0)
    {
        return true;
    }
    const double rx = std::floor(global.x / kWidth) * kWidth;
    const double ry = std::floor(global.y / kWidth) * kWidth;
    const uint64_t region = static_cast<uint64_t>(rx) << 32 | static_cast<uint64_t>(ry);
    return canHearCell(region, cell(glm::vec2(static_cast<float>(global.x - rx),
                                              static_cast<float>(global.y - ry))));
}

bool ParcelSounds::canHearCell(uint64_t region, int c) const
{
    if(inAgentParcel(region, c))
    {
        return true;
    }
    if(agentSoundLocal_ && !agentCells_.empty())
    {
        return false;
    }
    const auto it = overlays_.find(region);
    return it == overlays_.end() || (it->second[static_cast<size_t>(c)] & kSoundLocal) == 0;
}

void ParcelSounds::clear()
{
    overlays_.clear();
    agentRegion_ = 0;
    agentCells_.clear();
    agentSoundLocal_ = false;
}

} // namespace viewer
