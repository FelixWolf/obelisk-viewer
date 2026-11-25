#pragma once

// Who's in voice and how loud they're speaking, as shown over their name
// tags: a dot for being in it, and bars to its sides while speaking, as many
// and as tall as it's loud (the official viewer's LLVoiceVisualizer has a dot
// and waves, with the same colors and levels): gray when quiet, greener the
// louder, red when too loud. The bars fall off over 0.4 s.

#include "viewer/core/Uuid.hpp"
#include "viewer/render/Scene.hpp"
#include "viewer/voice/VoiceClient.hpp"

#include <glm/vec3.hpp>

#include <optional>
#include <unordered_map>

namespace viewer
{

class VoiceIndicators
{
public:
    // Once a frame, before get().
    void beginFrame(float seconds);
    // Someone's, from what voice says of them (nothing: not in voice, no
    // indicator).
    std::optional<render::VoiceMark> get(const Uuid& id,
                                         const std::optional<voice::VoiceClient::Status>& status);
    void clear() { people_.clear(); }

    // How loud, 0 to 1, for a power; and its color (sRGB).
    static float level(float power);
    static glm::vec3 color(float power);

private:
    struct Person
    {
        float power = 0.0f; // eased down
        bool seen = true;   // this frame
    };
    float seconds_ = 0.0f;
    std::unordered_map<Uuid, Person> people_;
};

} // namespace viewer
