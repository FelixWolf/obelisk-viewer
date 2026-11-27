#include "AvatarComplexity.hpp"

#include "AvatarLoading.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/llsd/Serialization.hpp"
#include "viewer/volume/Volume.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numbers>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace viewer
{

namespace
{

const Uuid kInvisibleTexture = *Uuid::parse("3a367d1c-bef1-6d43-7595-e88c1e3aadb3");

// The bakes' texture entry indices (llavatarappearancedefines.cpp: head,
// upper, lower, eyes, skirt, hair, left arm, left leg, aux 1 to 3).
constexpr int kBakeSlots[] = {8, 9, 10, 11, 19, 20, 40, 41, 42, 43, 44};

// HUD attachment points (llvoavatar.cpp: 31 to 38): not counted.
bool isHudPoint(int point)
{
    return point >= 31 && point <= 38;
}

int attachmentPoint(uint8_t state)
{
    return ((state & 0xF0) >> 4) | ((state & 0x0F) << 4);
}

// llvovolume.cpp getRenderCost's numbers.
constexpr float kParticleCost = 1.0f;
constexpr uint32_t kParticleMax = 2048;
constexpr float kLightCost = 500.0f;
constexpr float kMediaFaceCost = 1500.0f;
constexpr float kGlowMult = 1.5f;
constexpr float kBumpMult = 1.25f;
constexpr float kFlexiMult = 5.0f;
constexpr float kShinyMult = 1.6f;
constexpr float kWeightedMeshMult = 1.2f;
constexpr float kPlanarMult = 1.0f;
constexpr float kAnimTexMult = 4.0f;
constexpr float kAlphaMult = 4.0f;
// llmeshrepository.h.
constexpr float kAnimatedObjectBaseCost = 15.0f;
constexpr float kAnimatedObjectCostPerKTri = 1.5f;
// llvoavatar.cpp.
constexpr float kMaxAttachmentComplexity = 1.0e6f;
constexpr float kAnimatedAttachmentSurcharge = 1000.0f;
constexpr uint32_t kBodyPartCost = 200;

const char* choiceName(AvatarComplexity::Choice c)
{
    switch(c)
    {
    case AvatarComplexity::Choice::Always:
        return "always";
    case AvatarComplexity::Choice::Never:
        return "never";
    default:
        return "normal";
    }
}

} // namespace

namespace complexity
{

std::array<float, 4> estimatedTriangles(const std::array<int64_t, 4>& lodBytes)
{
    // A missing level is the next higher one.
    std::array<int64_t, 4> bytes = lodBytes;
    for(int i = 2; i >= 0; i--)
    {
        if(bytes[static_cast<size_t>(i)] == 0)
        {
            bytes[static_cast<size_t>(i)] = bytes[static_cast<size_t>(i) + 1];
        }
    }
    // MeshMetaDataDiscount, MeshMinimumByteSize, MeshBytesPerTriangle.
    constexpr float kDiscount = 384.0f;
    constexpr float kMinimum = 16.0f;
    constexpr float kBytesPerTriangle = 16.0f;
    std::array<float, 4> triangles{};
    for(size_t i = 0; i < triangles.size(); i++)
    {
        triangles[i] =
            std::max(static_cast<float>(bytes[i]) - kDiscount, kMinimum) / kBytesPerTriangle;
    }
    return triangles;
}

float radiusWeightedTriangles(const std::array<float, 4>& triangles, float radius)
{
    constexpr float kMaxDistance = 512.0f;
    const float lowest = std::min(radius / 0.03f, kMaxDistance);
    const float low = std::min(radius / 0.06f, kMaxDistance);
    const float mid = std::min(radius / 0.24f, kMaxDistance);

    constexpr float kMaxArea = 102944.0f; // a circle around the region
    constexpr float kMinArea = 1.0f;
    constexpr float pi = std::numbers::pi_v<float>;
    float highArea = std::min(pi * mid * mid, kMaxArea);
    float midArea = std::min(pi * low * low, kMaxArea);
    float lowArea = std::min(pi * lowest * lowest, kMaxArea);
    float lowestArea = kMaxArea;
    lowestArea -= lowArea;
    lowArea -= midArea;
    midArea -= highArea;
    highArea = std::clamp(highArea, kMinArea, kMaxArea);
    midArea = std::clamp(midArea, kMinArea, kMaxArea);
    lowArea = std::clamp(lowArea, kMinArea, kMaxArea);
    lowestArea = std::clamp(lowestArea, kMinArea, kMaxArea);
    const float total = highArea + midArea + lowArea + lowestArea;
    return (triangles[3] * highArea + triangles[2] * midArea + triangles[1] * lowArea +
            triangles[0] * lowestArea) /
           total;
}

float streamingTriangles(const std::array<float, 4>& triangles)
{
    float charged = triangles[3];
    float allowed = triangles[3];
    constexpr float kFloor = 64.0f;
    for(int i = 2; i >= 0; i--)
    {
        // A level should be at most half the one above, down to the floor.
        // (llclamp: the floor first, then the level's own count.)
        const float t = triangles[static_cast<size_t>(i)];
        const float half = allowed / 2.0f;
        allowed = half < kFloor ? kFloor : std::min(half, t);
        charged += std::max(0.0f, t - allowed);
    }
    return charged;
}

uint32_t textureCost(glm::uvec2 size)
{
    constexpr float kTextureCost = 16.0f; // ARC_TEXTURE_COST
    return 256 + static_cast<uint32_t>(kTextureCost * (static_cast<float>(size.y) / 128.0f +
                                                       static_cast<float>(size.x) / 128.0f));
}

float primCost(const world::Object& prim, bool animated, bool animatedRoot,
               const ComplexitySources& sources, std::unordered_set<Uuid>& textures, bool& pending)
{
    if(!prim.hasVolume)
    {
        return 0.0f;
    }
    const uint8_t sculpt = prim.sculptType & world::SculptType::Mask;
    const bool isMesh = sculpt == world::SculptType::Mesh;
    bool rigged = false;
    float triangles = 0.0f;
    const float radius = glm::length(prim.scale) * 0.5f;
    if(isMesh)
    {
        const auto sizes =
            sources.meshLodSizes ? sources.meshLodSizes(prim.sculptTexture) : std::nullopt;
        if(!sizes)
        {
            pending = true;
            return 0.0f; // nothing known yet
        }
        if(std::ranges::all_of(*sizes, [](int64_t s) { return s <= 0; }))
        {
            return 0.0f; // nothing to draw (the official viewer counts it so too)
        }
        rigged = sources.meshRigged && sources.meshRigged(prim.sculptTexture).value_or(false);
        const auto estimated = estimatedTriangles(*sizes);
        triangles = animated && rigged ? kAnimatedObjectCostPerKTri * 0.001f *
                                             streamingTriangles(estimated) / 0.06f
                                       : radiusWeightedTriangles(estimated, radius);
    }
    else
    {
        // A prim's (or sculpt's) shape: its triangles as if 10 bytes each.
        const auto counts = volume::lodTriangleCounts(prim.volume);
        std::array<int64_t, 4> bytes{};
        for(size_t i = 0; i < bytes.size(); i++)
        {
            bytes[i] = static_cast<int64_t>(counts[i]) * 10;
        }
        triangles = radiusWeightedTriangles(estimatedTriangles(bytes), radius);
        if(sculpt != world::SculptType::None && !prim.sculptTexture.isNull())
        {
            textures.insert(prim.sculptTexture);
        }
    }
    if(triangles <= 0.0f)
    {
        triangles = 4.0f;
    }
    // (The official viewer truncates the triangles to a whole number.)
    triangles = std::floor(triangles);

    bool alpha = false;
    bool bump = false;
    bool shiny = false;
    bool glow = false;
    bool planar = false;
    int mediaFaces = 0;
    for(const world::TextureFace& face : prim.faces)
    {
        if(!face.texture.isNull())
        {
            textures.insert(face.texture);
        }
        alpha |= face.color.a < 1.0f || (!face.texture.isNull() && sources.textureBlends &&
                                         sources.textureBlends(face.texture));
        mediaFaces += face.hasMedia() ? 1 : 0;
        bump |= face.bump() != 0;
        shiny |= face.shiny() != 0;
        glow |= face.glow > 0.0f;
        planar |= face.planarMapping();
    }

    float shame = std::max(triangles * 5.0f, 2.0f);
    if(planar)
    {
        shame *= kPlanarMult;
    }
    if(prim.textureAnimation)
    {
        shame *= kAnimTexMult;
    }
    if(alpha)
    {
        shame *= kAlphaMult;
    }
    if(glow)
    {
        shame *= kGlowMult;
    }
    if(bump)
    {
        shame *= kBumpMult;
    }
    if(shiny)
    {
        shame *= kShinyMult;
    }
    if(rigged)
    {
        shame *= kWeightedMeshMult;
    }
    if(prim.flexible)
    {
        shame *= kFlexiMult;
    }
    if(prim.particles)
    {
        const world::ParticleSystem& p = *prim.particles;
        const float bursts =
            p.burstRate > 0.0f ? std::ceil(p.particleMaxAge / p.burstRate) : p.particleMaxAge;
        const uint32_t count = std::min(
            static_cast<uint32_t>(static_cast<float>(p.burstCount) * std::max(bursts, 0.0f)),
            kParticleMax);
        const float size =
            (std::max(p.startScale.x, p.endScale.x) + std::max(p.startScale.y, p.endScale.y)) /
            2.0f;
        shame += static_cast<float>(count) * size * kParticleCost;
    }
    if(prim.light)
    {
        shame += kLightCost;
    }
    shame += static_cast<float>(mediaFaces) * kMediaFaceCost;
    if(animatedRoot)
    {
        shame += kAnimatedObjectBaseCost / 0.06f * 5.0f;
    }
    return shame;
}

uint32_t attachmentCost(const world::Object& root, std::span<const world::Object* const> children,
                        const ComplexitySources& sources, bool& pending)
{
    std::unordered_set<Uuid> textures;
    const bool animated = root.animatedMesh;
    float cost = animated ? kAnimatedAttachmentSurcharge : 0.0f;
    cost += primCost(root, animated, animated, sources, textures, pending);
    for(const world::Object* child : children)
    {
        cost += primCost(*child, animated, false, sources, textures, pending);
    }
    for(const Uuid& texture : textures)
    {
        const auto size = sources.textureSize ? sources.textureSize(texture) : std::nullopt;
        cost += static_cast<float>(textureCost(size.value_or(glm::uvec2(0))));
    }
    return static_cast<uint32_t>(std::clamp(cost, 0.0f, kMaxAttachmentComplexity));
}

uint32_t bodyCost(const world::AvatarAppearance* appearance)
{
    if(!appearance)
    {
        return 0;
    }
    uint32_t cost = 0;
    for(const int slot : kBakeSlots)
    {
        const Uuid& id = appearance->faces[static_cast<size_t>(slot)].texture;
        if(bakeDefined(id) && id != kInvisibleTexture)
        {
            cost += kBodyPartCost;
        }
    }
    return cost;
}

float surfaceArea(const world::Object& prim)
{
    if(!prim.hasVolume)
    {
        return 0.0f;
    }
    return std::max({prim.scale.x, prim.scale.y, prim.scale.z});
}

} // namespace complexity

AvatarComplexity::AvatarComplexity(fs::path file) : file_(std::move(file))
{
    load();
}

AvatarComplexity::~AvatarComplexity() = default;

void AvatarComplexity::setLimits(const Limits& limits)
{
    limits_ = limits;
}

AvatarComplexity::Choice AvatarComplexity::choice(const Uuid& avatar) const
{
    const auto it = choices_.find(avatar);
    return it == choices_.end() ? Choice::Normal : it->second;
}

void AvatarComplexity::setChoice(const Uuid& avatar, Choice choice)
{
    if(avatar.isNull() || this->choice(avatar) == choice)
    {
        return;
    }
    if(choice == Choice::Normal)
    {
        choices_.erase(avatar);
    }
    else
    {
        choices_[avatar] = choice;
    }
    save();
}

const AvatarComplexity::Info* AvatarComplexity::find(const Uuid& avatar) const
{
    const auto it = avatars_.find(avatar);
    return it == avatars_.end() ? nullptr : &it->second;
}

void AvatarComplexity::clear()
{
    avatars_.clear();
    if(!jelly_.empty())
    {
        jelly_.clear();
        revision_++;
    }
    seenRevisions_.reset();
}

void AvatarComplexity::update(const world::World& world, const ComplexitySources& sources,
                              const Uuid& self, const IsFriend& isFriend,
                              std::chrono::steady_clock::time_point now)
{
    const std::pair revisions(world.objectContentsRevision(), world.appearanceRevision());
    const bool changed = seenRevisions_ != revisions;
    if(!(changed && now - updatedAt_ >= kInterval) && now - updatedAt_ < kRefresh)
    {
        return;
    }
    seenRevisions_ = revisions;
    updatedAt_ = now;

    // Every worn link set (not on a HUD), by its avatar and root.
    struct Worn
    {
        const world::Object* root = nullptr;
        std::vector<const world::Object*> children;
    };
    std::unordered_map<Uuid, std::unordered_map<uint32_t, Worn>> worn; // avatar -> root's local id
    for(const auto& [key, object] : world.objects())
    {
        if(object.type != world::ObjectType::Prim || object.parentId == 0)
        {
            continue;
        }
        // Up to the prim attached to an avatar (link sets are one deep,
        // but an attachment's parent may come later than it).
        const world::Object* root = &object;
        const world::Object* parent = world.object(object.region, object.parentId);
        if(parent && parent->type == world::ObjectType::Prim && parent->parentId != 0)
        {
            root = parent;
            parent = world.object(parent->region, parent->parentId);
        }
        if(!parent || parent->type != world::ObjectType::Avatar ||
           isHudPoint(attachmentPoint(root->state)))
        {
            continue;
        }
        Worn& w = worn[parent->id][root->localId];
        if(root == &object)
        {
            w.root = &object;
        }
        else
        {
            w.root = root;
            w.children.push_back(&object);
        }
    }

    std::unordered_map<Uuid, Info> avatars;
    for(const world::ObjectKey& key : world.avatarKeys())
    {
        const world::Object* avatar = world.object(key.region, key.localId);
        if(!avatar || avatar->id.isNull())
        {
            continue;
        }
        Info info;
        info.key = key;
        uint64_t total = complexity::bodyCost(world.appearance(avatar->id));
        if(const auto it = worn.find(avatar->id); it != worn.end())
        {
            for(const auto& [rootId, w] : it->second)
            {
                total += complexity::attachmentCost(*w.root, w.children, sources, info.pending);
                info.area += complexity::surfaceArea(*w.root);
                for(const world::Object* child : w.children)
                {
                    info.area += complexity::surfaceArea(*child);
                }
            }
        }
        info.complexity = static_cast<uint32_t>(std::min<uint64_t>(total, UINT32_MAX));
        avatars[avatar->id] = info;
    }
    avatars_ = std::move(avatars);
    decide(self, isFriend);
}

void AvatarComplexity::decide(const Uuid& self, const IsFriend& isFriend)
{
    // llvoavatar.cpp isTooComplex and getOverallAppearance.
    std::unordered_set<Uuid> jelly;
    for(auto& [id, info] : avatars_)
    {
        const Choice c = choice(id);
        const bool isAFriend = isFriend && isFriend(id);
        bool tooComplex = false;
        if(id == self || c == Choice::Always || (isAFriend && limits_.mode != Mode::Everyone))
        {
            tooComplex = false;
        }
        else if(c == Choice::Never || limits_.mode == Mode::OnlyFriends)
        {
            tooComplex = true;
        }
        else
        {
            // No complexity limit is no surface area limit either.
            tooComplex = limits_.maxComplexity > 0 &&
                         (info.complexity > limits_.maxComplexity ||
                          (limits_.maxArea > 0.0f && info.area > limits_.maxArea));
        }
        info.jelly = tooComplex;
        if(tooComplex)
        {
            jelly.insert(id);
        }
    }
    if(jelly != jelly_)
    {
        jelly_ = std::move(jelly);
        revision_++;
    }
}

void AvatarComplexity::load()
{
    if(file_.empty())
    {
        return;
    }
    std::ifstream in(file_, std::ios::binary);
    if(!in)
    {
        return; // none yet
    }
    std::stringstream text;
    text << in.rdbuf();
    const std::string data = text.str();
    const auto parsed = llsd::detectFormat(data) ? llsd::parse(data) : llsd::fromNotation(data);
    if(!parsed || !parsed->isMap())
    {
        log::warn("avatar render choices: {} couldn't be read", file_.string());
        return;
    }
    for(const auto& [name, value] : *parsed->get<llsd::Map>())
    {
        const auto id = Uuid::parse(name);
        const std::string c = value.asString();
        if(!id || id->isNull())
        {
            continue;
        }
        if(c == "always")
        {
            choices_[*id] = Choice::Always;
        }
        else if(c == "never")
        {
            choices_[*id] = Choice::Never;
        }
    }
}

void AvatarComplexity::save() const
{
    if(file_.empty())
    {
        return;
    }
    llsd::Map out;
    for(const auto& [id, c] : choices_)
    {
        out.set(id.toString(), llsd::Value(std::string(choiceName(c))));
    }
    const std::string text =
        llsd::toNotation(llsd::Value(std::move(out)), {.pretty = true, .header = true});
    // Into a new file, then over the old one.
    std::error_code error;
    fs::create_directories(file_.parent_path(), error);
    fs::path temporary = file_;
    temporary += ".new";
    {
        std::ofstream o(temporary, std::ios::binary | std::ios::trunc);
        o << text;
        if(!o)
        {
            log::warn("avatar render choices: couldn't write {}", temporary.string());
            return;
        }
    }
    fs::rename(temporary, file_, error);
    if(error)
    {
        log::warn("avatar render choices: couldn't replace {}: {}", file_.string(),
                  error.message());
    }
}

} // namespace viewer
