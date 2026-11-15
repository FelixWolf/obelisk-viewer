#pragma once

// Where each frame's time goes on the main thread, by stage: the frame
// loop marks the end of each stage, and the time since the previous mark
// counts towards it. Keeps the last kHistory frames, for averages and for
// a breakdown of the slowest one (what a hitch was made of).

#include <SDL3/SDL_timer.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace viewer
{

class FrameProfile
{
public:
    enum Stage : uint8_t
    {
        Session,  // network messages in, world updates out
        Textures, // finished decodes uploaded
        Acquire,  // waiting for the swapchain (vsync, or the GPU behind)
        Loading,  // meshes, prims and animations that finished, uploaded
        Terrain,
        Avatars,
        Objects,
        Ui,
        Render, // recording the frame's GPU commands
        Submit,
        HeadsetWait, // the headset's xrWaitFrame (its pace, or the GPU behind)
        HeadsetEnd,  // handing the eyes to the headset (xrEndFrame)
        kStageCount,
    };
    static constexpr std::array<const char*, kStageCount> kNames = {
        "session", "textures", "acquire", "loading", "terrain",      "avatars",
        "objects", "ui",       "render",  "submit",  "headset wait", "headset end",
    };
    static constexpr size_t kHistory = 240;

    using Times = std::array<float, kStageCount>; // milliseconds

    void beginFrame()
    {
        current_ = {};
        last_ = SDL_GetTicksNS();
    }
    void mark(Stage stage)
    {
        const uint64_t now = SDL_GetTicksNS();
        current_[stage] += static_cast<float>(now - last_) * 1e-6f;
        last_ = now;
    }
    void endFrame()
    {
        frames_[next_] = current_;
        next_ = (next_ + 1) % kHistory;
        count_ = count_ < kHistory ? count_ + 1 : kHistory;
    }

    // Per stage, over the frames kept.
    Times average() const
    {
        Times sum{};
        for(size_t i = 0; i < count_; i++)
        {
            for(size_t s = 0; s < kStageCount; s++)
            {
                sum[s] += frames_[i][s];
            }
        }
        for(float& t : sum)
        {
            t = count_ > 0 ? t / static_cast<float>(count_) : 0.0f;
        }
        return sum;
    }
    // The slowest frame kept.
    Times worst() const
    {
        Times out{};
        float worstTotal = -1.0f;
        for(size_t i = 0; i < count_; i++)
        {
            const float total = sum(frames_[i]);
            if(total > worstTotal)
            {
                worstTotal = total;
                out = frames_[i];
            }
        }
        return out;
    }

    static float sum(const Times& times)
    {
        float total = 0.0f;
        for(const float t : times)
        {
            total += t;
        }
        return total;
    }
    // "session 0.10, textures 0.02, ..." (stages over 0.005 ms).
    static std::string describe(const Times& times);

private:
    std::array<Times, kHistory> frames_{};
    size_t next_ = 0;
    size_t count_ = 0;
    Times current_{};
    uint64_t last_ = 0;
};

} // namespace viewer
