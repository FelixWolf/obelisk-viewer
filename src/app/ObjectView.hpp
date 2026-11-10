#pragma once

// Draws the world's prims. Geometry is generated on worker threads from each
// prim's shape parameters and cached per (shape, level of detail), so the many
// identical boxes and cylinders of a region share one mesh. Each face is drawn
// with its texture entry: texture, color, texture transform, fullbright and
// transparency.
//
// Mesh objects get their geometry from the MeshManager instead.
//
// Not drawn yet: sculpted prims, attachments (they need avatars), trees and
// grass.

#include "MeshManager.hpp"
#include "TextureManager.hpp"

#include "viewer/core/JobSystem.hpp"
#include "viewer/core/MessageQueue.hpp"
#include "viewer/render/Renderer.hpp"
#include "viewer/render/Scene.hpp"
#include "viewer/volume/Volume.hpp"
#include "viewer/world/World.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <vector>

namespace viewer
{

class ObjectView
{
public:
    ObjectView(JobSystem& jobs, render::Renderer& renderer);
    // Waits for this view's geometry jobs.
    ~ObjectView();

    ObjectView(const ObjectView&) = delete;
    ObjectView& operator=(const ObjectView&) = delete;

    // Uploads finished geometry.
    void update();

    // Adds the prims within `drawDistance` of the camera.
    void addTo(render::Scene& scene, const world::World& world, uint64_t originHandle,
               TextureManager& textures, MeshManager& meshes, float drawDistance);

    // Drops cached geometry (e.g. on logout).
    void clear();

    struct Stats
    {
        size_t meshes = 0;      // cached (shape, detail) meshes
        size_t pending = 0;     // being generated
        size_t drawn = 0;       // prims drawn last frame
        size_t faces = 0;       // faces drawn last frame
        size_t meshObjects = 0; // of drawn, how many are mesh
        size_t skipped = 0;     // sculpted/attached prims not drawn
    };
    Stats stats() const { return stats_; }

private:
    struct Key
    {
        volume::VolumeParams params;
        int detail = 0; // index into volume::kDetailScales
        auto operator<=>(const Key&) const = default;
    };
    struct Geometry
    {
        bool ready = false;
        std::vector<render::Mesh> faces; // one per volume face (may be empty)
    };
    struct Generated
    {
        Key key;
        std::vector<volume::VolumeFace> faces;
    };

    Geometry& geometry(const Key& key);

    JobSystem& jobs_;
    render::Renderer& renderer_;
    std::map<Key, Geometry> cache_;
    MessageQueue<Generated> generated_;
    std::vector<Generated> drained_;
    std::atomic<int> running_{0};
    Stats stats_;
};

} // namespace viewer
