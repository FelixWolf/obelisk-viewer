#pragma once

// The home scene: what stands around you before logging in (the lobby). A
// file of binary LLSD made from an OXP export by
// default_scene/sceneConvert.py, shaped like a simulator's own state:
//
//   {_version: 1, asset_cdn: "http://host" (or a bare host),
//    scene: {environment: {water, skies: [{height, sky}], dayLength, dayOffset},
//            terrain: {water: meters, textures: {base[4], detail[4],
//                      startHeights[4], heightRanges[4]},
//                      heights: binary (256 x 256 little-endian float32,
//                               row by row from the south edge)},
//            objects: [[root, child, ...], ...]}}
//
// Linksets whose root is named "#HOME:Floor", "#HOME:Landing", "#HOME:Login",
// "#HOME:News" or "#HOME:GridStatus" are markers: their position and
// rotation place the lobby's things, and they aren't drawn.
//
// It's turned into the updates a region would send (terrain, the
// handshake's textures and water, full object updates, material
// overrides), so the World and the usual views show it. Assets aren't in
// the file: they come from `asset_cdn` (its viewer-asset service needs no
// login). Nothing in it runs: there are no scripts.

#include "viewer/assets/LegacyMaterial.hpp"
#include "viewer/core/Uuid.hpp"
#include "viewer/world/World.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace viewer::world
{

// Where something of the lobby's is (a "#HOME:" marker object): region
// coordinates. It faces its local +Y (the panels, the landing).
struct HomeMarker
{
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

struct HomeScene
{
    // The viewer-asset service, as a ViewerAsset capability would give it.
    std::string assetUrl;
    // The region's updates, to apply to an empty World in order.
    std::vector<WorldUpdate> updates;
    // Legacy materials the faces name (by ids made from their contents).
    std::vector<std::pair<Uuid, assets::LegacyMaterial>> legacyMaterials;

    // The lobby's places (missing: the viewer picks its own).
    std::optional<float> floor; // the ground you walk on, meters
    std::optional<HomeMarker> landing;
    std::optional<HomeMarker> login;
    std::optional<HomeMarker> news;
    std::optional<HomeMarker> gridStatus;

    // The environment's settings assets (not used yet: the default sky).
    Uuid water;
    Uuid sky; // the ground level's
};

// The PBR material a face with only overrides is given (the export keeps
// overrides, not the material they're over): the default glTF material.
// The app serves it locally.
inline const Uuid kHomeBlankMaterial = *Uuid::parse("5c3b1d2e-0000-4000-8000-686f6d65626c");

// Reads a home scene for a region with this handle.
std::expected<HomeScene, std::string> loadHomeScene(std::span<const uint8_t> bytes,
                                                    uint64_t regionHandle);

} // namespace viewer::world
