#pragma once

// Our look-at and point-at, told to viewers nearby (ViewerEffect), as the
// official viewer's LLHUDEffectLookAt and LLHUDEffectPointAt do: each is
// one effect (its own id) updated when what it's at changes, or the point
// moves more than 5 cm (at most 4 times a second), sent again before it
// would run out at the receivers, and cleared when it ends.

#include "viewer/core/Uuid.hpp"
#include "viewer/session/Session.hpp"

#include <array>
#include <functional>
#include <optional>

namespace viewer
{

class ViewerEffects
{
public:
    // The official target types (llhudeffectlookat.h, llhudeffectpointat.h).
    static constexpr uint8_t kNone = 0; // what a clear is sent as
    static constexpr uint8_t kLookIdle = 1;
    static constexpr uint8_t kLookFreeLook = 3;
    static constexpr uint8_t kLookSelect = 7;
    static constexpr uint8_t kLookClear = 10;
    static constexpr uint8_t kPointSelect = 1;
    static constexpr uint8_t kPointGrab = 2;
    static constexpr uint8_t kPointClear = 3;

    struct Target
    {
        uint8_t type = 0;
        Uuid object{}; // null: `position` is global
        std::array<double, 3> position{};
    };

    using Send = std::function<void(const session::Session::AttentionEffect&)>;
    // Each frame: what we look at and point at now (nothing: not anything,
    // cleared once, as type "none" lasting no time); `send` sends an effect
    // (Session::sendAttention).
    void update(const Send& send, const std::optional<Target>& lookAt,
                const std::optional<Target>& pointAt, double seconds);

    // How long a look or point lasts at the receivers (the official
    // attention timeouts; "for ever" is a quarter of a float's range).
    static float lookDuration(uint8_t type);
    static float pointDuration(uint8_t type);

private:
    struct Channel
    {
        explicit Channel(uint8_t type) : effectType(type) {}
        Uuid id = Uuid::random();
        uint8_t effectType = 0;
        std::optional<Target> sent;
        double sentAt = -1e9;
    };
    static void update(const Send& send, Channel& channel, const std::optional<Target>& want,
                       double seconds, float (*duration)(uint8_t));

    Channel look_{session::Session::kLookAtEffect};
    Channel point_{session::Session::kPointAtEffect};
};

} // namespace viewer
