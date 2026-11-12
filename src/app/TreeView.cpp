#include "TreeView.hpp"

#include "FaceUpload.hpp"

#include "viewer/volume/Volume.hpp"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <random>

namespace viewer
{

namespace
{

// --- Species (the official viewer's trees.xml and grass.xml) -------------------

struct TreeSpecies
{
    const char* texture;
    float droop; // degrees
    float twist; // degrees
    float branches;
    int depth;
    float scaleStep;
    int trunkDepth;
    float branchLength;
    float trunkLength;
    float leafScale;
    float trunkAspect;
    float branchAspect;
    float noiseMag;
    float noiseScale;
    float taper;
    float repeatZ;
};

// clang-format off
constexpr TreeSpecies kTrees[] = {
    {"0187babf-6c0d-5891-ebed-4ecab1426683", 60.0f, 5.0f, 5.0f, 1, 0.7f, 6, 8.0f, 11.5f, 22.0f, 0.1f, 0.05f, 0.5f, 2.5f, 0.8f, 3.0f},   // Pine 1
    {"8a515889-eac9-fb55-8eba-d2dc09eb32c8", 35.0f, 3.0f, 4.0f, 3, 0.7f, 0, 3.0f, 3.8f, 7.0f, 0.15f, 0.07f, 1.2f, 4.0f, 0.3f, 4.0f},     // Oak
    {"5bc11cd6-2f40-071e-a8da-0903394204f9", 10.0f, 0.0f, 6.0f, 1, 0.5f, 1, 0.5f, 0.15f, 7.5f, 1.0f, 0.08f, 1.0f, 1.0f, 0.2f, 1.0f},     // Tropical Bush 1
    {"ca4e8c27-473c-eb1c-2f5d-50ee3f07d85c", 0.0f, 0.0f, 3.0f, 1, 0.5f, 0, 0.7f, 9.0f, 10.0f, 0.035f, 0.03f, 0.2f, 6.0f, 0.7f, 10.0f},   // Palm 1
    {"64367bd1-697e-b3e6-0b65-3f862a577366", 30.0f, 0.0f, 3.0f, 2, 0.7f, 1, 2.75f, 4.0f, 5.5f, 0.06f, 0.05f, 1.5f, 2.0f, 0.8f, 3.0f},    // Dogwood
    {"cdd9a9fc-6d0b-f90d-8416-c72b6019bca8", 10.0f, 0.0f, 3.0f, 1, 0.5f, 1, 0.5f, 0.15f, 6.0f, 1.0f, 0.08f, 1.0f, 1.0f, 0.2f, 1.0f},     // Tropical Bush 2
    {"2d784476-d0db-9979-0cff-9408745a7cf3", 0.0f, 0.0f, 3.0f, 1, 0.5f, 0, 0.7f, 10.0f, 7.5f, 0.035f, 0.03f, 0.2f, 6.0f, 0.6f, 12.0f},   // Palm 2
    {"fb2ae204-3fd1-df33-594f-c9f882830e66", 30.0f, 0.0f, 3.0f, 4, 0.5f, 0, 10.0f, 10.0f, 70.0f, 0.05f, 0.03f, 1.2f, 1.0f, 0.5f, 6.0f},  // Cypress 1
    {"30047cec-269d-408e-0c30-b2603b887268", 30.0f, 0.0f, 3.0f, 4, 0.6f, 3, 7.5f, 10.0f, 35.0f, 0.05f, 0.04f, 1.2f, 1.0f, 0.5f, 5.0f},   // Cypress 2
    {"d691a01c-13b7-578d-57c0-5caef0b4e7e1", 50.0f, 7.5f, 4.0f, 2, 0.7f, 6, 6.0f, 10.0f, 15.5f, 0.1f, 0.08f, 0.5f, 2.5f, 0.7f, 3.0f},   // Pine 2
    {"6de37e4e-7029-61f5-54b8-f5e63f983f58", 8.0f, 7.0f, 3.0f, 2, 0.6f, 0, 3.0f, 0.1f, 20.0f, 0.1f, 0.075f, 0.0f, 0.0f, 0.85f, 2.0f},   // Plumeria
    {"10d2a01a-0818-84b9-4b96-c2eb63256519", 90.0f, 2.5f, 6.0f, 1, 0.66f, 8, 0.0f, 4.0f, 6.75f, 0.1f, 0.05f, 0.0f, 2.5f, 0.85f, 2.0f},  // Winter Pine 1
    {"7c0cf89b-44b1-1ce2-dd74-07102a98ac2a", 85.0f, 3.0f, 5.0f, 1, 0.6f, 8, 3.0f, 4.5f, 8.0f, 0.06f, 0.05f, 0.75f, 2.5f, 0.8f, 2.0f},   // Winter Aspen
    {"67931331-0c02-4876-1255-28770896c6a2", 140.0f, 5.0f, 6.0f, 1, 0.6f, 7, 0.0f, 3.0f, 5.0f, 0.1f, 0.05f, 0.75f, 2.5f, 0.5f, 2.0f},   // Winter Pine 2
    {"a6162133-724b-54df-a12f-51cd070ad6f3", 20.0f, 5.0f, 3.6f, 4, 0.6f, 0, 12.0f, 8.0f, 33.0f, 0.15f, 0.08f, 0.0f, 0.0f, 0.675f, 3.0f}, // Eucalyptus
    {"8872f2b8-31db-42d8-580a-b3e4a91262de", 12.0f, 0.0f, 7.0f, 1, 0.5f, 0, 0.01f, 0.0f, 4.0f, 1.0f, 0.08f, 1.0f, 1.0f, 0.2f, 1.0f},    // Fern
    {"96b4de31-f4fa-337d-ec78-451e3609769e", 0.0f, 0.0f, 5.0f, 1, 0.5f, 1, 0.5f, 0.15f, 5.0f, 1.0f, 0.08f, 1.0f, 1.0f, 0.2f, 1.0f},     // Eelgrass
    {"5894e2e7-ab8d-edfa-e61c-18cf16854ba3", 0.0f, 0.0f, 6.0f, 1, 0.7f, 1, 0.0f, 0.0f, 2.0f, 1.0f, 1.0f, 0.5f, 0.0f, 0.0f, 1.0f},       // Sea Sword
    {"2caf1179-7861-6ff3-4b7d-46e17780bdfa", -15.0f, 0.0f, 1.0f, 1, 1.0f, 3, 2.5f, 0.75f, 1.85f, 0.04f, 0.05f, 1.0f, 2.0f, 0.8f, 2.0f},  // Kelp 1
    {"18fb888b-e8f1-dce7-7da7-321d651ea6b0", 0.0f, 0.0f, 4.0f, 1, 0.7f, 1, 0.0f, 0.0f, 4.0f, 1.0f, 1.0f, 0.5f, 0.0f, 0.0f, 1.0f},       // Beach Grass 1
    {"2a4880b6-b7a3-690a-2049-bfbe38eafb9f", -15.0f, 0.0f, 1.0f, 1, 1.0f, 3, 2.5f, 1.35f, 2.0f, 0.025f, 0.05f, 1.0f, 2.0f, 0.8f, 2.0f}, // Kelp 2
};
// clang-format on

struct GrassSpecies
{
    const char* texture;
    float bladeWidth;
    float bladeHeight;
};

constexpr GrassSpecies kGrass[] = {
    {"6c4727b8-ac79-ba44-3b81-f9aa887b47eb", 1.35f, 1.35f},
    {"79504bf5-c3ec-0763-6563-d843de66d0a1", 1.0f, 0.66f},
    {"6c4727b8-ac79-ba44-3b81-f9aa887b47eb", 1.8f, 1.8f},
    {"99bd60a2-3250-efc9-2e39-2fbcadefbecc", 1.0f, 1.0f},
    {"7a2b3a4a-53c2-53ac-5716-aac7d743c020", 2.25f, 2.25f},
    {"8f458549-173b-23ff-d4ff-bfaa5ea2371b", 2.0f, 2.0f},
};

const TreeSpecies& treeSpecies(uint8_t species)
{
    // Unknown species are the first (as the official viewer).
    return species < std::size(kTrees) ? kTrees[species] : kTrees[0];
}

const GrassSpecies& grassSpecies(uint8_t species)
{
    return species < std::size(kGrass) ? kGrass[species] : kGrass[0];
}

// --- Noise for the bark -----------------------------------------------------------

float hashNoise(int x, int y, int z)
{
    uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u ^
                 static_cast<uint32_t>(z) * 83492791u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return static_cast<float>(h & 0xFFFF) / 32767.5f - 1.0f;
}

// Smooth value noise in -1..1.
float noise3(glm::vec3 p)
{
    const glm::vec3 cell = glm::floor(p);
    const glm::vec3 f = p - cell;
    const glm::vec3 t = f * f * (3.0f - 2.0f * f);
    const int x = static_cast<int>(cell.x);
    const int y = static_cast<int>(cell.y);
    const int z = static_cast<int>(cell.z);
    auto lerp = [](float a, float b, float s) { return a + (b - a) * s; };
    const float x00 = lerp(hashNoise(x, y, z), hashNoise(x + 1, y, z), t.x);
    const float x10 = lerp(hashNoise(x, y + 1, z), hashNoise(x + 1, y + 1, z), t.x);
    const float x01 = lerp(hashNoise(x, y, z + 1), hashNoise(x + 1, y, z + 1), t.x);
    const float x11 = lerp(hashNoise(x, y + 1, z + 1), hashNoise(x + 1, y + 1, z + 1), t.x);
    return lerp(lerp(x00, x10, t.y), lerp(x01, x11, t.y), t.z);
}

// Octaves of noise, each half as strong (as the official turbulence).
float turbulence(glm::vec3 p, float frequency)
{
    float sum = 0.0f;
    for(float f = frequency; f >= 1.0f; f *= 0.5f)
    {
        sum += noise3(p * f) / f;
    }
    return sum;
}

// --- Tree geometry (as llvotree.cpp) ---------------------------------------------

constexpr int kSlices[4] = {10, 5, 4, 3};                    // per level of detail
constexpr float kLodAngles[4] = {30.0f, 20.0f, 15.0f, 0.0f}; // degrees the tree spans
// The leaf texture is the right half of the species' texture; bark the left.
constexpr float kLeafLeft = 0.52f;
constexpr float kLeafRight = 0.98f;
constexpr float kLeafTop = 1.0f;
constexpr float kLeafBottom = 0.52f;

struct Piece
{
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> uvs;
    std::vector<uint32_t> indices;
};

// Two crossed quads, each with both sides: a clump of leaves 1 high.
Piece leafPiece()
{
    Piece p;
    const float r2 = 0.707106781f;
    const float r3 = 0.577350269f;
    auto quad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, glm::vec3 na, glm::vec3 nb,
                    glm::vec3 nc, glm::vec3 nd, bool flip)
    {
        const auto base = static_cast<uint32_t>(p.positions.size());
        p.positions.insert(p.positions.end(), {a, b, c, d});
        p.normals.insert(p.normals.end(), {na, nb, nc, nd});
        p.uvs.insert(p.uvs.end(),
                     {glm::vec2(kLeafLeft, kLeafBottom), glm::vec2(kLeafRight, kLeafTop),
                      glm::vec2(kLeafLeft, kLeafTop), glm::vec2(kLeafRight, kLeafBottom)});
        if(flip)
        {
            p.indices.insert(p.indices.end(), {base, base + 2, base + 1, base, base + 1, base + 3});
        }
        else
        {
            p.indices.insert(p.indices.end(), {base, base + 1, base + 2, base, base + 3, base + 1});
        }
    };
    // In the XZ plane, then the YZ plane; each seen from both sides.
    quad({-0.5f, 0, 0}, {0.5f, 0, 1}, {-0.5f, 0, 1}, {0.5f, 0, 0}, {-r2, -r2, 0}, {r3, -r3, r3},
         {-r3, -r3, r3}, {r2, -r2, 0}, false);
    quad({-0.5f, 0, 0}, {0.5f, 0, 1}, {-0.5f, 0, 1}, {0.5f, 0, 0}, {-r2, r2, 0}, {r3, r3, r3},
         {-r3, r3, r3}, {r2, r2, 0}, true);
    quad({0, -0.5f, 0}, {0, 0.5f, 1}, {0, -0.5f, 1}, {0, 0.5f, 0}, {r2, -r2, 0}, {r3, r3, r3},
         {r3, -r3, r3}, {r2, r2, 0}, false);
    quad({0, -0.5f, 0}, {0, 0.5f, 1}, {0, -0.5f, 1}, {0, 0.5f, 0}, {-r2, -r2, 0}, {-r3, r3, r3},
         {-r3, -r3, r3}, {-r2, r2, 0}, true);
    return p;
}

// A tapered, bumpy cylinder 1 high with capped ends: one trunk or branch
// segment.
Piece branchPiece(const TreeSpecies& sp, int lod)
{
    Piece p;
    const int slices = kSlices[lod];
    const float baseRadius = 0.65f;
    const float topRadius = baseRadius * sp.taper;
    const float angleStep = 360.0f / static_cast<float>(slices - 1);
    const float zStep = slices > 3 ? 1.0f / static_cast<float>(slices - 3) : 1.0f;
    constexpr float kCapNudge = 0.1f; // the caps peak a little
    for(int i = 0; i < slices; i++)
    {
        float z;
        float r0;
        if(i == 0)
        {
            z = -kCapNudge;
            r0 = 0.0f;
        }
        else if(i == slices - 1)
        {
            z = 1.0f + kCapNudge;
            r0 = 0.0f;
        }
        else
        {
            z = static_cast<float>(i - 1) * zStep;
            r0 = baseRadius + (topRadius - baseRadius) * z;
        }
        for(int j = 0; j < slices; j++)
        {
            const float angle = j == slices - 1 ? 0.0f : static_cast<float>(j) * angleStep;
            const float a = glm::radians(angle);
            const float start = r0 * (1.0f + 1.2f * std::abs(z - 0.66f));
            const glm::vec3 n(std::cos(a) * start * sp.noiseScale,
                              std::sin(a) * start * sp.noiseScale, z * 4.0f * sp.noiseScale);
            const float radius = start + turbulence(n, 5.0f) * sp.noiseMag;
            p.positions.emplace_back(std::cos(a) * radius, std::sin(a) * radius, z);
            p.normals.emplace_back(std::cos(a), std::sin(a), 0.0f);
            // Bark: the texture's left half, repeated up the trunk.
            const float u = j == slices - 1 ? 0.49f : angle / 360.0f * 0.5f;
            p.uvs.emplace_back(u, (1.0f - z / 2.0f) * sp.repeatZ);
        }
    }
    for(int i = 0; i < slices - 1; i++)
    {
        for(int j = 0; j < slices - 1; j++)
        {
            const auto at = [&](int row, int col)
            { return static_cast<uint32_t>(row * slices + col); };
            p.indices.insert(p.indices.end(), {at(i, j), at(i + 1, j + 1), at(i + 1, j), at(i, j),
                                               at(i, j + 1), at(i + 1, j + 1)});
        }
    }
    return p;
}

void append(volume::VolumeFace& out, const Piece& piece, const glm::mat4& m)
{
    const auto base = static_cast<uint32_t>(out.positions.size());
    const glm::mat3 normalMatrix = glm::inverseTranspose(glm::mat3(m));
    for(size_t i = 0; i < piece.positions.size(); i++)
    {
        out.positions.push_back(glm::vec3(m * glm::vec4(piece.positions[i], 1.0f)));
        out.normals.push_back(glm::normalize(normalMatrix * piece.normals[i]));
        out.texCoords.push_back(piece.uvs[i]);
    }
    for(const uint32_t index : piece.indices)
    {
        out.indices.push_back(base + index);
    }
}

// The trunk and branches, recursively: each segment carries `branches`
// branches (turned by the twist, bent down by the droop) and, while the
// trunk goes on, the next trunk segment (turned 70.5 degrees); the ends
// carry leaves.
struct TreeBuilder
{
    const TreeSpecies& sp;
    const Piece& branch;
    const Piece& leaves;
    float droop;
    volume::VolumeFace& out;

    void build(const glm::mat4& m, int depth, int trunkDepth, float scale)
    {
        if(depth <= 0)
        {
            append(out, leaves, m * glm::scale(glm::mat4(1.0f), glm::vec3(scale * sp.leafScale)));
            return;
        }
        const bool trunk = trunkDepth > 0 || scale == 1.0f;
        const float length = trunk ? sp.trunkLength : sp.branchLength;
        const float aspect = trunk ? sp.trunkAspect : sp.branchAspect;
        const float width = scale * length * aspect;
        append(out, branch,
               m * glm::scale(glm::mat4(1.0f), glm::vec3(width, width, scale * length)));

        const glm::mat4 top =
            m * glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, scale * length));
        const float spacing = 360.0f / sp.branches;
        const glm::vec3 z(0.0f, 0.0f, 1.0f);
        for(int i = 0; i < static_cast<int>(sp.branches); i++)
        {
            const float turn =
                (spacing + (i % 2 == 0 ? sp.twist : -sp.twist)) * static_cast<float>(i);
            const glm::mat4 child =
                top * glm::rotate(glm::mat4(1.0f), glm::radians(turn), z) *
                glm::rotate(glm::mat4(1.0f), glm::radians(droop), glm::vec3(0.0f, 1.0f, 0.0f)) *
                glm::rotate(glm::mat4(1.0f), glm::radians(20.0f), z);
            build(child, depth - 1, 0, scale * sp.scaleStep);
        }
        if(trunkDepth > 0)
        {
            build(top * glm::rotate(glm::mat4(1.0f), glm::radians(70.5f), z), depth, trunkDepth - 1,
                  scale * sp.scaleStep);
        }
    }
};

// Blades of grass: the same scatter for every grass object (as the official
// viewer: one random table per session; ours is seeded the same each time).
struct Blade
{
    glm::vec2 offset; // before scaling by the object's size
    glm::vec2 facing; // sin, cos of its turn
    glm::vec2 lean;   // its top's offset
    float strength;   // size factor
};

const std::array<Blade, 32>& blades()
{
    static const std::array<Blade, 32> table = []
    {
        std::array<Blade, 32> t{};
        std::mt19937 random(1234);
        std::uniform_real_distribution<float> unit(0.0001f, 1.0f);
        constexpr float kSpread = 0.15f; // standard deviation (llvograss.cpp)
        constexpr float kBladeBase = 0.25f;
        for(Blade& b : t)
        {
            const float u = std::sqrt(-2.0f * std::log(unit(random)));
            const float v = glm::two_pi<float>() * unit(random);
            b.offset = glm::vec2(u * std::sin(v), u * std::cos(v)) * kSpread;
            const float turn = unit(random) * glm::pi<float>();
            b.facing = glm::vec2(std::sin(turn), std::cos(turn));
            b.lean = glm::vec2(unit(random), unit(random)) * (kBladeBase * 0.25f);
            b.strength = 0.5f + unit(random);
        }
        return t;
    }();
    return table;
}

glm::vec3 regionOffset(uint64_t region, uint64_t origin)
{
    return glm::vec3(
        static_cast<float>(static_cast<int64_t>(region >> 32) - static_cast<int64_t>(origin >> 32)),
        static_cast<float>(static_cast<int64_t>(region & 0xFFFFFFFFu) -
                           static_cast<int64_t>(origin & 0xFFFFFFFFu)),
        0.0f);
}

render::BlinnPhongMaterial foliage(const render::GpuTexture* texture)
{
    render::BlinnPhongMaterial m;
    m.diffuse = texture;
    m.alphaCutoff = 0.5f;
    m.specularTint = glm::vec3(0.0f);
    return m;
}

} // namespace

TreeView::TreeView(render::Renderer& renderer) : renderer_(renderer) {}

void TreeView::clear()
{
    trees_.clear();
    grass_.clear();
}

const render::Mesh* TreeView::treeMesh(uint8_t species, int lod)
{
    const auto key = std::make_pair(species < std::size(kTrees) ? species : uint8_t{0}, lod);
    auto it = trees_.find(key);
    if(it == trees_.end())
    {
        if(buildsLeft_ <= 0)
        {
            // Over budget: any level of this species that's built.
            for(int other = 3; other >= 0; other--)
            {
                const auto ready = trees_.find(std::make_pair(key.first, other));
                if(ready != trees_.end() && ready->second)
                {
                    return &ready->second;
                }
            }
            return nullptr;
        }
        buildsLeft_--;
        it = trees_.try_emplace(key).first;
        const TreeSpecies& sp = treeSpecies(key.first);
        const Piece branch = branchPiece(sp, lod);
        const Piece leaves = leafPiece();
        volume::VolumeFace face;
        // Without wind, the droop is 25 degrees more (llvotree.cpp updateMesh).
        TreeBuilder{sp, branch, leaves, sp.droop + 25.0f, face}.build(glm::mat4(1.0f), sp.depth,
                                                                      sp.trunkDepth, 1.0f);
        auto meshes = uploadFaces(renderer_, {face});
        if(!meshes.empty())
        {
            it->second = std::move(meshes[0]);
        }
    }
    return it->second ? &it->second : nullptr;
}

void TreeView::addTo(render::Scene& scene, const world::World& world, uint64_t originHandle,
                     TextureManager& textures, float drawDistance)
{
    stats_ = {};
    constexpr int kMeshBuildsPerFrame = 2;
    buildsLeft_ = kMeshBuildsPerFrame;
    for(auto& [key, g] : grass_)
    {
        g.used = false;
    }
    const glm::vec3 camera = scene.camera.position;

    for(const auto& [key, object] : world.objects())
    {
        const bool tree =
            object.type == world::ObjectType::Tree || object.type == world::ObjectType::NewTree;
        const bool grass = object.type == world::ObjectType::Grass;
        if(!tree && !grass)
        {
            continue;
        }
        const glm::vec3 offset = regionOffset(object.region, originHandle);
        const glm::vec3 position = offset + object.position;
        const float size = glm::length(object.scale);
        const float distance = glm::length(position - camera);

        if(tree)
        {
            const float radius = std::max(size * 1.5f, 2.0f);
            const glm::vec3 center = position + glm::vec3(0.0f, 0.0f, radius * 0.5f);
            if(distance - radius > drawDistance)
            {
                continue;
            }
            const render::Visibility seen = scene.visibility(center, radius);
            if(seen == render::Visibility::Hidden)
            {
                continue;
            }
            const TreeSpecies& sp = treeSpecies(object.species);
            const render::GpuTexture* texture = textures.get(
                Uuid::parse(sp.texture).value_or(Uuid{}),
                seen == render::Visibility::ShadowOnly ? 0.0f : textures.texelsFor(size, distance));
            if(!texture)
            {
                continue; // not drawn white while its texture loads
            }
            // Level of detail by how big it looks (llvotree.cpp's angles).
            const float spans = glm::degrees(std::atan2(size, std::max(distance, 0.1f)));
            int lod = 3;
            for(int j = 0; j < 4; j++)
            {
                if(spans > kLodAngles[j])
                {
                    lod = j;
                    break;
                }
            }
            const render::Mesh* mesh = treeMesh(object.species, lod);
            if(!mesh)
            {
                continue;
            }
            // Planted a little into the ground, turned a quarter, scaled by
            // its size (llvotree.cpp updateMesh).
            const glm::mat4 transform =
                glm::translate(glm::mat4(1.0f), position - glm::vec3(0.0f, 0.0f, 0.1f)) *
                glm::mat4_cast(object.rotation) *
                glm::rotate(glm::mat4(1.0f), glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f)) *
                glm::scale(glm::mat4(1.0f), glm::vec3(size * 0.05f));
            render::ObjectDraw draw{mesh, transform, foliage(texture)};
            draw.shadowOnly = seen == render::Visibility::ShadowOnly;
            scene.objects.push_back(draw);
            stats_.trees++;
            continue;
        }

        // Grass: its blades stand on the terrain under them.
        const world::RegionTerrain* terrain = world.terrain(object.region);
        const float radius = std::max(object.scale.x, object.scale.y) + 2.0f;
        if(!terrain || distance - radius > std::min(drawDistance, 64.0f))
        {
            continue;
        }
        const render::Visibility seen = scene.visibility(position, radius);
        if(seen == render::Visibility::Hidden)
        {
            continue;
        }
        const GrassSpecies& sp = grassSpecies(object.species);
        const render::GpuTexture* texture = textures.get(
            Uuid::parse(sp.texture).value_or(Uuid{}),
            seen == render::Visibility::ShadowOnly ? 0.0f
                                                   : textures.texelsFor(2.0f * radius, distance));
        if(!texture)
        {
            continue;
        }
        GrassMesh& g = grass_[key];
        g.used = true;
        if(!g.mesh || g.objectRevision != object.revision || g.terrainRevision != terrain->revision)
        {
            constexpr float kBladeBase = 0.25f;
            constexpr float kBladeHeight = 0.5f;
            volume::VolumeFace face;
            for(const Blade& b : blades())
            {
                const glm::vec2 at =
                    glm::vec2(object.position) +
                    glm::vec2(b.offset.x * object.scale.x, b.offset.y * object.scale.y);
                const glm::vec2 side = b.facing * kBladeBase * sp.bladeWidth * b.strength;
                const float height = kBladeHeight * sp.bladeHeight * b.strength;
                // The official viewer's other base corner uses the x
                // offset twice; kept, so blades face the same ways.
                const glm::vec2 a2(at.x + side.x, at.y + side.y);
                const glm::vec2 b2(at.x - side.x, at.y - side.x);
                const glm::vec3 base1(a2, terrain->heightAt(a2.x, a2.y));
                const glm::vec3 base2(b2, terrain->heightAt(b2.x, b2.y));
                const glm::vec3 top1 = base1 + glm::vec3(b.lean, height);
                const glm::vec3 top2 = base2 + glm::vec3(b.lean, height);
                glm::vec3 front = glm::cross(base1 - top1, top1 - base2);
                front.z = 0.75f;
                front = glm::normalize(front);
                glm::vec3 back = -front;
                back.z = -back.z;
                for(const auto& [n, flip] : {std::pair{front, false}, std::pair{back, true}})
                {
                    const auto v = static_cast<uint32_t>(face.positions.size());
                    face.positions.insert(face.positions.end(), {base1, top1, base2, top2});
                    face.normals.insert(face.normals.end(), {n, n, n, n});
                    face.texCoords.insert(face.texCoords.end(),
                                          {glm::vec2(0.0f, 0.0f), glm::vec2(0.0f, 0.98f),
                                           glm::vec2(1.0f, 0.0f), glm::vec2(1.0f, 0.98f)});
                    if(flip)
                    {
                        face.indices.insert(face.indices.end(),
                                            {v, v + 2, v + 1, v + 1, v + 2, v + 3});
                    }
                    else
                    {
                        face.indices.insert(face.indices.end(),
                                            {v, v + 1, v + 2, v + 1, v + 3, v + 2});
                    }
                }
            }
            auto meshes = uploadFaces(renderer_, {face});
            g.mesh = meshes.empty() ? render::Mesh{} : std::move(meshes[0]);
            g.objectRevision = object.revision;
            g.terrainRevision = terrain->revision;
        }
        if(!g.mesh)
        {
            continue;
        }
        render::ObjectDraw draw{&g.mesh, glm::translate(glm::mat4(1.0f), offset), foliage(texture)};
        draw.shadowOnly = seen == render::Visibility::ShadowOnly;
        scene.objects.push_back(draw);
        stats_.grass++;
    }
    std::erase_if(grass_, [](const auto& entry) { return !entry.second.used; });
}

} // namespace viewer
