#pragma once

// Name tags over avatars, as the official viewer shows them by default
// (LLVOAvatar::idleUpdateNameTag, LLHUDNameTag): in a dark bubble over the
// head, a status line (away, do not disturb, editing appearance), the group
// title, the display name, and the username when the display name isn't
// made from it. They fade as the avatar gets small on screen and with
// distance (gone past 25 m). Until an avatar's display name arrives, its
// legacy name stands in.
//
// Chat bubbles: while an avatar has recent chat or is typing, its tag
// shows that chat (left aligned, new lines bright, then dimmer as they age,
// ours yellow), "..." while typing, with the names in a band across the
// top; seen twice as far.

#include "AvatarNames.hpp"
#include "AvatarView.hpp"
#include "LocalChat.hpp"

#include "viewer/render/Scene.hpp"
#include "viewer/world/World.hpp"

#include <cstdint>
#include <functional>
#include <optional>

namespace viewer
{

// Adds a tag per avatar (scene.camera must be set); `self` is our avatar.
// The display name is green while they speak; `voice` gives their
// indicator (nothing: not in voice).
void addNameTags(render::Scene& scene, const world::World& world, const AvatarView& avatars,
                 AvatarNames& names, LocalChat& chat, const Uuid& self, uint64_t originHandle,
                 LocalChat::Clock::time_point now,
                 const std::function<std::optional<render::VoiceMark>(const Uuid&)>& voice);

} // namespace viewer
