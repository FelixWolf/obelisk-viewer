#pragma once

// Avatar complexity and jelly dolls, as the official viewer has them: each
// avatar's complexity ("ARC") is added up from what it wears, and others
// over the limit (or whose attachments cover too much surface) are drawn as
// "jelly dolls": the body alone, in one flat color, with none of their
// attachments, their lights or their particles. Someone can be picked to be
// always drawn whole, or always as a jelly doll; friends can be let off the
// limit, or only friends drawn at all.
//
// The cost follows the official viewer's (llvoavatar.cpp
// calculateUpdateRenderComplexity and accountRenderComplexityForObject,
// llvovolume.cpp getRenderCost and getTextureCost, llmeshrepository.cpp
// LLMeshCostData), which every viewer is asked to keep the same so that
// limits mean the same everywhere:
// - the body: 200 for each bake it shows;
// - each attachment (its link set; not HUDs): every prim's cost, plus each
//   texture it uses once (256 + 16 x (width + height) / 128), held to at most
//   1,000,000; 1000 more for an animated object worn.
// - a prim: 5 a triangle (at least 2), the triangles a mesh's level of
//   detail sizes or a prim's shape suggest, weighted by how far each level
//   is seen from; times 4 for alpha, 4 for texture animation, 1.5 for glow,
//   1.25 for bump, 1.6 for shiny, 1.2 for rigged mesh and 5 for flexible;
//   plus particles (their count times size), 500 for a light and 1500 for
//   each face with media.
// Ways ours can differ: the face count (all of a texture entry's faces are
// looked at, which change nothing unless set apart from the default),
// textures not loaded yet (counted at 256, as the official viewer counts
// those it hasn't sized), and sculpt surface areas (taken as 1, like every
// other prim's).
//
// The per-avatar choices are kept in a file of their own (as the official
// avatar_render_settings.xml).

#include "viewer/core/Uuid.hpp"
#include "viewer/world/World.hpp"

#include <glm/glm.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace viewer
{

// What the cost needs to know about meshes and textures (MeshManager and
// TextureManager, in the viewer). Asking may start loading what's asked
// about.
struct ComplexitySources
{
    // A mesh's level of detail block sizes in bytes, lowest to highest;
    // nothing until its header is in.
    std::function<std::optional<std::array<int64_t, 4>>(const Uuid& mesh)> meshLodSizes;
    // Whether a mesh is rigged; nothing until known.
    std::function<std::optional<bool>(const Uuid& mesh)> meshRigged;
    // A texture's whole width and height; nothing until known.
    std::function<std::optional<glm::uvec2>(const Uuid& texture)> textureSize;
    // Whether a texture is drawn blended (alpha); false until known.
    std::function<bool(const Uuid& texture)> textureBlends;
};

namespace complexity
{

// LLMeshCostData: triangles a level's bytes suggest (lowest to highest; a
// missing level is the next higher one's).
std::array<float, 4> estimatedTriangles(const std::array<int64_t, 4>& lodBytes);
// The triangles seen on average, each level weighted by the area of the
// region it's seen from (LLMeshCostData::getRadiusWeightedTris).
float radiusWeightedTriangles(const std::array<float, 4>& triangles, float radius);
// The triangles charged for an animated object's mesh: the highest level's,
// plus any lower level has beyond half the one above it
// (getEstTrisForStreamingCost).
float streamingTriangles(const std::array<float, 4>& triangles);
// One texture's cost, by its whole size (0 x 0 while unknown).
uint32_t textureCost(glm::uvec2 size);

// One prim's cost, without its textures (which are added to `textures`:
// each counts once per attachment). `animated`: it's in an animated
// object's link set (`animatedRoot`: its root). `pending` is set if a
// mesh's header hasn't loaded (it costs nothing till then).
float primCost(const world::Object& prim, bool animated, bool animatedRoot,
               const ComplexitySources& sources, std::unordered_set<Uuid>& textures, bool& pending);

// An attachment's cost: its root and the prims linked to it.
uint32_t attachmentCost(const world::Object& root, std::span<const world::Object* const> children,
                        const ComplexitySources& sources, bool& pending);

// The body's: 200 for each bake shown.
uint32_t bodyCost(const world::AvatarAppearance* appearance);

// Square meters an attachment counts for (the surface area limit): each
// prim's largest side (LLVolume::getSurfaceArea is 1 but for sculpts).
float surfaceArea(const world::Object& prim);

} // namespace complexity

class AvatarComplexity
{
public:
    // How someone is drawn, as picked for them (the official visual mute
    // settings).
    enum class Choice : uint8_t
    {
        Normal, // by the limits
        Always, // whole, whatever their complexity
        Never,  // as a jelly doll
    };
    // Whose complexity the limit applies to (RenderAvatarComplexityMode).
    enum class Mode : uint8_t
    {
        Everyone,
        FriendsAlways, // friends are always drawn whole
        OnlyFriends,   // everyone else is a jelly doll
    };
    struct Limits
    {
        uint32_t maxComplexity = 350000; // 0: no limit (nor on surface area)
        float maxArea = 1000.0f;         // square meters; 0: none
        Mode mode = Mode::Everyone;
    };
    // The jelly dolls' color (LLColor4::grey4, sRGB).
    static constexpr glm::vec3 kJellyColor{0.3f, 0.3f, 0.3f};

    // `file`: where the per-avatar choices are kept (empty: nowhere).
    explicit AvatarComplexity(std::filesystem::path file = {});
    ~AvatarComplexity();
    AvatarComplexity(const AvatarComplexity&) = delete;
    AvatarComplexity& operator=(const AvatarComplexity&) = delete;

    void setLimits(const Limits& limits);
    const Limits& limits() const { return limits_; }

    Choice choice(const Uuid& avatar) const;
    void setChoice(const Uuid& avatar, Choice choice);

    // Adds up every avatar's complexity again when what they wear may have
    // changed (or has loaded), at most a few times a second, and decides
    // who's a jelly doll. `self` is never one.
    using IsFriend = std::function<bool(const Uuid&)>;
    void update(const world::World& world, const ComplexitySources& sources, const Uuid& self,
                const IsFriend& isFriend, std::chrono::steady_clock::time_point now);
    // Decides again (limits, choices or friends changed) from the last
    // costs, without adding them up.
    void decide(const Uuid& self, const IsFriend& isFriend);

    struct Info
    {
        uint32_t complexity = 0;
        float area = 0.0f; // of its attachments, square meters
        bool jelly = false;
        // Some attachment's mesh hasn't told its size yet (it costs
        // nothing until it does).
        bool pending = false;
        world::ObjectKey key;
    };
    // An avatar's, as of the last update; null if it isn't around.
    const Info* find(const Uuid& avatar) const;
    const std::unordered_map<Uuid, Info>& all() const { return avatars_; }
    // Those drawn as jelly dolls.
    const std::unordered_set<Uuid>& jellyDolls() const { return jelly_; }
    // Bumped whenever who's a jelly doll changes.
    uint64_t revision() const { return revision_; }

    // A session ended: everyone's forgotten (the choices are kept).
    void clear();

private:
    void load();
    void save() const;

    std::filesystem::path file_;
    Limits limits_;
    std::unordered_map<Uuid, Choice> choices_;
    std::unordered_map<Uuid, Info> avatars_;
    std::unordered_set<Uuid> jelly_;
    uint64_t revision_ = 0;
    // What the last costs were added up from, and when.
    std::optional<std::pair<uint64_t, uint64_t>> seenRevisions_;
    std::chrono::steady_clock::time_point updatedAt_{};
    // Costs change without the world changing (meshes' and textures'
    // sizes arriving): added up again this often regardless.
    static constexpr auto kInterval = std::chrono::milliseconds(500);
    static constexpr auto kRefresh = std::chrono::seconds(3);
};

} // namespace viewer
