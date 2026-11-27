#pragma once

// Which avatars are impostors, as the official viewer's
// RenderAvatarMaxNonImpostors (llvoavatar.cpp computeUpdatePeriod,
// shouldImpostor): avatars in view, nearest first, past the nearest
// `maxFull`, are drawn as a picture on a card (render::Impostor), the
// picture taken again now and then. And when: each gets a slot for its
// picture (a big one if it's shown big), and a new picture when it's old
// (more often for nearer ones: the official 32, 48 and 64 frame periods,
// here in seconds), when the camera has moved around it, or when it has
// come nearer or gone farther; a few a frame, the most overdue first.
//
// For headsets: a card is flat, the same picture to both eyes, which shows
// up close; so nothing nearer than `minDistance` is an impostor (the
// official viewer's least is the avatar's size, plus a meter), and the
// camera may move further around one before it's taken again (a head is
// never still).

#include "viewer/core/Uuid.hpp"
#include "viewer/render/Scene.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>

namespace viewer
{

class ImpostorPlanner
{
public:
    struct Candidate
    {
        Uuid id;
        glm::vec3 center{0.0f}; // of a sphere around it and what it wears (scene coordinates)
        float radius = 1.0f;
        float pixels = 0.0f; // how tall the sphere shows, in the view's pixels
    };
    struct Options
    {
        uint32_t maxFull = 16; // drawn whole; 0: everyone
        float minDistance = 0.0f;
        // How far (radians) the camera may move around one, and how much
        // nearer or farther it may come (a fraction), before it's taken again.
        float maxAngle = 0.05f;
        float maxDistanceChange = 0.1f;
        uint32_t capturesPerFrame = 4;
    };
    // Seconds between pictures: those ranked within 3 x maxFull, within 4 x,
    // and beyond.
    static constexpr std::array<double, 3> kPeriods = {0.25, 0.5, 1.0};

    struct Plan
    {
        uint32_t slot = 0;
        bool capture = false; // its picture is taken this frame
        render::Impostor card;
    };

    // Once a frame: those in view that could be impostors (not ours, not
    // jelly dolls, loaded), and where the camera is. `now` in seconds.
    void update(std::span<const Candidate> candidates, const glm::vec3& eye, const Options& options,
                double now);

    // An avatar's plan this frame; null: drawn whole.
    const Plan* find(const Uuid& avatar) const
    {
        const auto it = plans_.find(avatar);
        return it == plans_.end() ? nullptr : &it->second;
    }
    const std::unordered_map<Uuid, Plan>& all() const { return plans_; }

    void clear();

private:
    struct Taken
    {
        glm::vec3 eye{0.0f};
        glm::vec3 center{0.0f};
        double at = 0.0;
    };
    // A camera framing `c` from `eye` into a card.
    static render::Impostor frame(const glm::vec3& eye, const glm::vec3& c, float radius,
                                  uint32_t slot);

    std::unordered_map<Uuid, Plan> plans_;
    std::unordered_map<Uuid, Taken> taken_;
    std::array<bool, render::kImpostorSlots> used_{};
};

} // namespace viewer
