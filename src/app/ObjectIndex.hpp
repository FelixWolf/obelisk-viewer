#pragma once

// Where the world's prims are, kept up to date from each world snapshot's
// changes (World::changes), so that what's hidden can be skipped a cell at
// a time instead of looking at every prim every frame.
//
// Prims are kept by link set (a root and its children). Link sets that
// stand still go into cells of kCellSize meters (global coordinates), each
// with bounds and its link sets' spheres; ones that move (worn, moving or
// spinning, rigged mesh, or with a parent not known yet) are kept apart,
// to be looked at every frame. The prims that light, are reflection
// probes or emit particles are listed too: they matter even out of view.

#include "viewer/world/World.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace viewer
{

class ObjectIndex
{
public:
    static constexpr double kCellSize = 32.0;

    // Brings it up to date with this snapshot: its changes if it follows
    // the last one seen, otherwise everything again.
    void update(const world::World& world);
    void clear();

    struct LinkSet
    {
        world::ObjectKey root;
        std::vector<world::ObjectKey> members;
        bool moving = false;
        // Still ones: a sphere around every member (global coordinates;
        // each member's radius the length of its size, as generous as the
        // culling of single prims), and the members big enough to cast a
        // shadow from behind a wall (render::Scene::kHiddenShadowRadius).
        glm::dvec3 center{0.0};
        float radius = 0.0f;
        std::vector<world::ObjectKey> big;
        // Each member's box, for automatic reflection probes: its center
        // (global), half its size's length, its rotation and half its size.
        struct Prim
        {
            glm::dvec3 center{0.0};
            float radius = 0.0f;
            glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
            glm::vec3 half{0.0f};
        };
        std::vector<Prim> prims;
    };
    struct CellKey
    {
        int64_t x = 0;
        int64_t y = 0;
        int64_t z = 0;
        bool operator==(const CellKey&) const = default;
    };
    struct CellKeyHash
    {
        size_t operator()(const CellKey& k) const;
    };
    struct Cell
    {
        std::vector<world::ObjectKey> roots; // its still link sets
        glm::dvec3 lo{0.0};                  // bounds of their spheres
        glm::dvec3 hi{0.0};
        bool hasBig = false;
    };

    const std::unordered_map<CellKey, Cell, CellKeyHash>& cells() const { return cells_; }
    const LinkSet* linkSet(const world::ObjectKey& root) const;
    // Moving link sets' roots.
    const std::unordered_set<world::ObjectKey, world::ObjectKeyHash>& moving() const
    {
        return moving_;
    }
    // Prims that light, or are reflection probes.
    const std::unordered_set<world::ObjectKey, world::ObjectKeyHash>& lights() const
    {
        return lights_;
    }
    const std::unordered_set<world::ObjectKey, world::ObjectKeyHash>& probes() const
    {
        return probes_;
    }
    // Prims with a particle system.
    const std::unordered_set<world::ObjectKey, world::ObjectKeyHash>& particleSources() const
    {
        return particleSources_;
    }
    // Prims playing a sound (or set to).
    const std::unordered_set<world::ObjectKey, world::ObjectKeyHash>& soundSources() const
    {
        return soundSources_;
    }
    size_t prims() const { return rootOf_.size(); }

private:
    void rebuild(const world::World& world);
    // A prim updated, added or removed.
    void changedPrim(const world::World& world, const world::ObjectKey& key);
    // Its members' spheres and whether it moves, then into its cell (or the
    // moving ones).
    void place(const world::World& world, const world::ObjectKey& root);
    void unplace(const world::ObjectKey& root);
    void boundCell(const CellKey& key);

    const world::World* seen_ = nullptr;
    uint64_t serial_ = 0;
    std::unordered_map<world::ObjectKey, LinkSet, world::ObjectKeyHash> sets_;
    std::unordered_map<world::ObjectKey, world::ObjectKey, world::ObjectKeyHash> rootOf_;
    std::unordered_map<world::ObjectKey, CellKey, world::ObjectKeyHash> cellOf_; // still roots
    std::unordered_map<CellKey, Cell, CellKeyHash> cells_;
    std::unordered_set<world::ObjectKey, world::ObjectKeyHash> moving_;
    std::unordered_set<world::ObjectKey, world::ObjectKeyHash> lights_;
    std::unordered_set<world::ObjectKey, world::ObjectKeyHash> probes_;
    std::unordered_set<world::ObjectKey, world::ObjectKeyHash> particleSources_;
    std::unordered_set<world::ObjectKey, world::ObjectKeyHash> soundSources_;
    std::unordered_set<world::ObjectKey, world::ObjectKeyHash> dirty_;
    // Link sets whose root's parent wasn't known: placed again once it is.
    std::unordered_set<world::ObjectKey, world::ObjectKeyHash> orphans_;
};

} // namespace viewer
