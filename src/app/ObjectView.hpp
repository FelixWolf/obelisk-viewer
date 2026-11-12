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
#include "LegacyMaterialManager.hpp"
#include "MaterialManager.hpp"
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
               TextureManager& textures, MeshManager& meshes, MaterialManager& materials,
               LegacyMaterialManager& legacyMaterials, const AvatarView* avatars,
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
        size_t culled = 0;      // in range but out of view last frame
        size_t pbrFaces = 0;    // faces drawn with their PBR material
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
        // Largest |coordinate| on each axis, before scaling (prims can reach
        // past their half-unit box: tori, twists, radius offsets).
        glm::vec3 extent{0.5f};
    };
    struct Generated
    {
        Key key;
        std::vector<volume::VolumeFace> faces;
        glm::vec3 extent{0.0f};
    };

    Geometry& geometry(const Key& key);
    // A light-giving prim's light (it may light the view while out of it).
    void addLight(render::Scene& scene, const world::World& world, const world::Object& object,
                  uint64_t originHandle, TextureManager& textures, const AvatarView* avatars,
                  float drawDistance) const;
    // Fills in `extent` from the faces.
    static Generated withExtent(Generated g);
    // A face's material; for faces worn by `wearer` showing a Bake on Mesh
    // placeholder, its bake instead. Nothing while that bake loads.
    // `texels`: how many across the object needs, at one repeat
    // (TextureManager::texelsFor).
    // With a legacy material (normal and specular maps, alpha mode) once it
    // has arrived.
    static std::optional<render::BlinnPhongMaterial>
    material(const world::TextureFace& face, TextureManager& textures,
             LegacyMaterialManager& legacyMaterials, const world::World& world,
             const world::Object* wearer, const AvatarView* avatars, float texels);
    // A rigged mesh, skinned to the posed skeleton it follows (its avatar's
    // or its animated object's).
    void addRigged(render::Scene& scene, const world::World& world, const world::Object& object,
                   uint64_t originHandle, TextureManager& textures, MeshManager& meshes,
                   MaterialManager& materials, LegacyMaterialManager& legacyMaterials,
                   const AvatarView* avatars, float drawDistance);
    // A face's PBR material, with its override, and its texture transform,
    // into `draw`. False if the face has none, or it hasn't loaded yet (the
    // legacy texture stands in).
    static bool pbrMaterial(render::ObjectDraw& draw, const world::Object& object, size_t face,
                            const world::World& world, TextureManager& textures,
                            MaterialManager& materials, float texels);
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
