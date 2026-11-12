#pragma once

// Linden trees and grass: objects of their own types (not prims), built
// procedurally from per-species tables, as the official viewer's
// llvotree.cpp and llvograss.cpp. The species tables are the official
// viewer's app_settings/trees.xml and grass.xml (LGPL-2.1, like this
// project).
//
// A tree's mesh depends only on its species and level of detail, so trees
// share them; the object's position, rotation and size place it. Grass is
// 32 blades scattered over the object's area, each standing on the
// terrain under it, so each grass object gets its own mesh.

#include "TextureManager.hpp"

#include "viewer/render/Renderer.hpp"
#include "viewer/render/Scene.hpp"
#include "viewer/world/World.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <unordered_map>

namespace viewer
{

class TreeView
{
public:
    explicit TreeView(render::Renderer& renderer);

    // Adds the trees and grass within `drawDistance` of the camera.
    void addTo(render::Scene& scene, const world::World& world, uint64_t originHandle,
               TextureManager& textures, float drawDistance);

    // Drops cached meshes (e.g. on logout).
    void clear();

    struct Stats
    {
        size_t trees = 0; // drawn last frame
        size_t grass = 0;
    };
    Stats stats() const { return stats_; }

private:
    struct GrassMesh
    {
        render::Mesh mesh;
        uint32_t objectRevision = 0;
        uint32_t terrainRevision = 0;
        bool used = false;
    };

    // The mesh of a species at a level of detail (0 finest .. 3), built
    // when first needed.
    // Builds at most a few per frame (kMeshBuildsPerFrame); meanwhile
    // another level of the species stands in, or nothing.
    const render::Mesh* treeMesh(uint8_t species, int lod);

    render::Renderer& renderer_;
    std::map<std::pair<uint8_t, int>, render::Mesh> trees_;
    std::unordered_map<world::ObjectKey, GrassMesh, world::ObjectKeyHash> grass_;
    Stats stats_;
    int buildsLeft_ = 0; // this frame's
};

} // namespace viewer
