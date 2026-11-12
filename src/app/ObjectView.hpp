#pragma once

// Draws the world's prims. Geometry is generated on worker threads from each
// prim's shape parameters and cached per (shape, level of detail), so the many
// identical boxes and cylinders of a region share one mesh. Each face is drawn
// with its texture entry: texture, color, texture transform, fullbright and
// transparency.
//
// Mesh objects get their geometry from the MeshManager instead.
//
// Attachments ride on their avatar's attachment point (rest pose for now).
//
// Not drawn yet: sculpted prims, rigged mesh, trees and grass.

#include "AvatarView.hpp"
#include "MeshManager.hpp"
#include "ObjectMotion.hpp"
#include "TextureManager.hpp"

#include "viewer/core/JobSystem.hpp"
#include "viewer/core/MessageQueue.hpp"
#include "viewer/render/Renderer.hpp"
#include "viewer/render/Scene.hpp"
#include "viewer/volume/Volume.hpp"
#include "viewer/world/World.hpp"

#include <atomic>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace viewer
{

class ObjectView
{
public:
    // `motion` (where objects are this frame) must outlive the view.
    ObjectView(JobSystem& jobs, render::Renderer& renderer, const ObjectMotion& motion);
    // Waits for this view's geometry jobs.
    ~ObjectView();

    ObjectView(const ObjectView&) = delete;
    ObjectView& operator=(const ObjectView&) = delete;

    // Uploads finished geometry.
    void update();

    // Adds the prims within `drawDistance` of the camera.
    void addTo(render::Scene& scene, const world::World& world, uint64_t originHandle,
               TextureManager& textures, MeshManager& meshes, const AvatarView* avatars,
               float drawDistance);

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
        size_t palettes = 0;    // rigged meshes skinned last frame
    };
    Stats stats() const { return stats_; }

private:
    struct Key
    {
        volume::VolumeParams params;
        int detail = 0; // index into volume::kDetailScales
        bool operator==(const Key&) const = default;
    };
    struct KeyHash
    {
        size_t operator()(const Key& key) const;
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
    // A face's material; for faces worn by `wearer` showing a Bake on Mesh
    // placeholder, its bake instead. Nothing while that bake loads.
    static std::optional<render::BlinnPhongMaterial>
    material(const world::TextureFace& face, TextureManager& textures, const world::World& world,
             const world::Object* wearer, const AvatarView* avatars);
    // A rigged mesh, skinned to the posed skeleton it follows (its avatar's
    // or its animated object's).
    void addRigged(render::Scene& scene, const world::World& world, const world::Object& object,
                   uint64_t originHandle, TextureManager& textures, MeshManager& meshes,
                   const AvatarView* avatars, float drawDistance);
    // An object's frame (position and rotation, no scale) in scene
    // coordinates, through its linkset root and, for attachments, its
    // avatar's attachment point. Nothing if a parent isn't known yet.
    std::optional<glm::mat4> frameOf(const world::Object& object, const world::World& world,
                                     uint64_t originHandle, const AvatarView* avatars,
                                     int depth = 0) const;

    JobSystem& jobs_;
    render::Renderer& renderer_;
    const ObjectMotion& motion_;
    std::unordered_map<Key, Geometry, KeyHash> cache_;
    MessageQueue<Generated> generated_;
    std::vector<Generated> drained_;
    std::atomic<int> running_{0};
    Stats stats_;
    // This frame's joint matrices for rigged draws (the scene refers to them
    // until it's drawn; a deque keeps them in place as it grows).
    std::deque<std::vector<glm::mat4>> palettes_;
    size_t palettesUsed_ = 0; // this frame's
};

} // namespace viewer
