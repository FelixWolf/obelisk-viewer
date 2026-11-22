#include "ObjectIndex.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace viewer
{

namespace
{

// Shadows from behind a wall: render::Scene::kHiddenShadowRadius, for a
// member's generous radius (the length of its size).
constexpr float kBig = 8.0f;

glm::dvec3 regionCorner(uint64_t handle)
{
    return glm::dvec3(static_cast<double>(handle >> 32), static_cast<double>(handle & 0xFFFFFFFFu),
                      0.0);
}

} // namespace

size_t ObjectIndex::CellKeyHash::operator()(const CellKey& k) const
{
    uint64_t h = static_cast<uint64_t>(k.x) * 0x9E3779B97F4A7C15ull;
    h ^= static_cast<uint64_t>(k.y) * 0xC2B2AE3D27D4EB4Full + (h << 6) + (h >> 2);
    h ^= static_cast<uint64_t>(k.z) * 0x165667B19E3779F9ull + (h << 6) + (h >> 2);
    return static_cast<size_t>(h);
}

void ObjectIndex::clear()
{
    seen_ = nullptr;
    serial_ = 0;
    sets_.clear();
    rootOf_.clear();
    cellOf_.clear();
    cells_.clear();
    moving_.clear();
    lights_.clear();
    probes_.clear();
    particleSources_.clear();
    dirty_.clear();
    orphans_.clear();
}

const ObjectIndex::LinkSet* ObjectIndex::linkSet(const world::ObjectKey& root) const
{
    const auto it = sets_.find(root);
    return it == sets_.end() ? nullptr : &it->second;
}

void ObjectIndex::update(const world::World& world)
{
    if(&world == seen_ && world.serial() == serial_)
    {
        return;
    }
    const bool follows = seen_ && world.serial() == serial_ + 1 && !world.changes().everything;
    seen_ = &world;
    serial_ = world.serial();
    if(!follows)
    {
        rebuild(world);
        return;
    }
    for(const world::ObjectKey& key : world.changes().keys)
    {
        changedPrim(world, key);
    }
    // Link sets waiting for their root's parent: again once it's here.
    std::vector<world::ObjectKey> found;
    for(const world::ObjectKey& root : orphans_)
    {
        const world::Object* o = world.object(root.region, root.localId);
        if(!o || o->parentId == 0 || world.object(root.region, o->parentId))
        {
            found.push_back(root);
        }
    }
    for(const world::ObjectKey& root : found)
    {
        orphans_.erase(root);
        const auto it = sets_.find(root);
        if(it == sets_.end())
        {
            continue;
        }
        const std::vector<world::ObjectKey> members = it->second.members;
        for(const world::ObjectKey& m : members)
        {
            changedPrim(world, m);
        }
    }
    for(const world::ObjectKey& root : dirty_)
    {
        place(world, root);
    }
    dirty_.clear();
}

void ObjectIndex::rebuild(const world::World& world)
{
    const world::World* seen = seen_;
    const uint64_t serial = serial_;
    clear();
    seen_ = seen;
    serial_ = serial;
    for(const auto& [key, object] : world.objects())
    {
        changedPrim(world, key);
    }
    for(const world::ObjectKey& root : dirty_)
    {
        place(world, root);
    }
    dirty_.clear();
}

void ObjectIndex::changedPrim(const world::World& world, const world::ObjectKey& key)
{
    // Out of the link set it was in.
    if(const auto it = rootOf_.find(key); it != rootOf_.end())
    {
        const world::ObjectKey oldRoot = it->second;
        rootOf_.erase(it);
        if(const auto set = sets_.find(oldRoot); set != sets_.end())
        {
            std::erase(set->second.members, key);
        }
        dirty_.insert(oldRoot);
    }
    lights_.erase(key);
    probes_.erase(key);
    particleSources_.erase(key);

    const world::Object* object = world.object(key.region, key.localId);
    if(!object || object->type != world::ObjectType::Prim)
    {
        return; // gone (or not a prim)
    }
    // Into its root's: up through its parents, stopping below an avatar
    // (and at a parent not known yet).
    world::ObjectKey root = key;
    const world::Object* o = object;
    for(int depth = 0; o->parentId != 0 && depth < 16; depth++)
    {
        const world::Object* parent = world.object(root.region, o->parentId);
        if(!parent || parent->type != world::ObjectType::Prim)
        {
            break;
        }
        root = world::ObjectKey{root.region, o->parentId};
        o = parent;
    }
    LinkSet& set = sets_[root];
    set.root = root;
    set.members.push_back(key);
    rootOf_[key] = root;
    dirty_.insert(root);
    if(object->light)
    {
        lights_.insert(key);
    }
    if(object->reflectionProbe)
    {
        probes_.insert(key);
    }
    if(object->particles)
    {
        particleSources_.insert(key);
    }
}

void ObjectIndex::unplace(const world::ObjectKey& root)
{
    moving_.erase(root);
    if(const auto it = cellOf_.find(root); it != cellOf_.end())
    {
        const CellKey cell = it->second;
        cellOf_.erase(it);
        if(const auto c = cells_.find(cell); c != cells_.end())
        {
            std::erase(c->second.roots, root);
            boundCell(cell);
        }
    }
}

void ObjectIndex::place(const world::World& world, const world::ObjectKey& root)
{
    unplace(root);
    const auto it = sets_.find(root);
    if(it == sets_.end())
    {
        return;
    }
    LinkSet& set = it->second;
    if(set.members.empty())
    {
        sets_.erase(it);
        orphans_.erase(root);
        return;
    }
    set.big.clear();
    set.prims.clear();
    const world::Object* r = world.object(root.region, root.localId);
    // Worn, or under a parent not known yet: looked at every frame.
    set.moving = !r || r->parentId != 0 || r->animatedMesh;
    if(r && r->parentId != 0)
    {
        const world::Object* parent = world.object(root.region, r->parentId);
        if(!parent)
        {
            orphans_.insert(root);
        }
    }
    if(r && !set.moving)
    {
        const glm::dvec3 rootPosition = regionCorner(root.region) + glm::dvec3(r->position);
        set.center = rootPosition;
        set.radius = 0.0f;
        for(const world::ObjectKey& m : set.members)
        {
            const world::Object* member = m == root ? r : world.object(m.region, m.localId);
            if(!member)
            {
                continue;
            }
            // Moving or spinning between updates: looked at every frame.
            if(glm::dot(member->velocity, member->velocity) > 1e-6f ||
               glm::dot(member->angularVelocity, member->angularVelocity) > 1e-6f ||
               glm::dot(member->acceleration, member->acceleration) > 1e-6f)
            {
                set.moving = true;
            }
            const glm::dvec3 position =
                m == root ? rootPosition
                          : rootPosition + glm::dvec3(r->rotation * member->position);
            const float size = glm::length(member->scale);
            set.radius = std::max(set.radius,
                                  static_cast<float>(glm::length(position - rootPosition)) + size);
            const glm::quat rotation = m == root ? r->rotation : r->rotation * member->rotation;
            set.prims.push_back({position, size * 0.5f, rotation, member->scale * 0.5f});
            if(size >= kBig)
            {
                set.big.push_back(m);
            }
        }
    }
    if(set.moving)
    {
        moving_.insert(root);
        return;
    }
    const CellKey cell{static_cast<int64_t>(std::floor(set.center.x / kCellSize)),
                       static_cast<int64_t>(std::floor(set.center.y / kCellSize)),
                       static_cast<int64_t>(std::floor(set.center.z / kCellSize))};
    cells_[cell].roots.push_back(root);
    cellOf_[root] = cell;
    boundCell(cell);
}

void ObjectIndex::boundCell(const CellKey& key)
{
    const auto it = cells_.find(key);
    if(it == cells_.end())
    {
        return;
    }
    Cell& cell = it->second;
    if(cell.roots.empty())
    {
        cells_.erase(it);
        return;
    }
    cell.lo = glm::dvec3(1e300);
    cell.hi = glm::dvec3(-1e300);
    cell.hasBig = false;
    for(const world::ObjectKey& root : cell.roots)
    {
        const LinkSet& set = sets_.at(root);
        cell.lo = glm::min(cell.lo, set.center - glm::dvec3(set.radius));
        cell.hi = glm::max(cell.hi, set.center + glm::dvec3(set.radius));
        cell.hasBig = cell.hasBig || !set.big.empty();
    }
}

} // namespace viewer
