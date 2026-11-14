#pragma once

// Floating text (llSetText): a label over each prim that has some, as the
// official viewer places it (LLViewerObject::updateText, LLHUDText): above
// the prim by 0.6 of its height, brought nearer the camera by the prim's
// radius so the prim doesn't hide it, and faded out with distance. HUD
// attachments' text goes on the HUD, always shown.

#include "AvatarView.hpp"
#include "ObjectView.hpp"

#include "viewer/render/Scene.hpp"
#include "viewer/world/World.hpp"

#include <cstdint>

namespace viewer
{

// Adds the labels (scene.camera must be set).
void addFloatingText(render::Scene& scene, const world::World& world, const ObjectView& objects,
                     uint64_t originHandle, const AvatarView* avatars);

} // namespace viewer
