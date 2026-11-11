#pragma once

// Mesh assets: a binary-LLSD header listing where each part sits, followed by
// zlib-compressed binary-LLSD blocks (four levels of detail, skin, physics).
// Format as the official viewer reads it (llmeshrepository.cpp headerReceived,
// llvolume.cpp unpackVolumeFaces); our own implementation.

#include "viewer/volume/Volume.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace viewer::assets
{

// The header lives in the first 4096 bytes of the asset.
inline constexpr size_t kMeshHeaderFetchSize = 4096;

struct MeshBlock
{
    int64_t offset = -1; // from the end of the header
    int64_t size = 0;
    bool present() const { return offset >= 0 && size > 0; }
};

struct MeshHeader
{
    int version = 0;
    size_t headerSize = 0; // bytes the header occupies; block offsets start here
    // Lowest to highest detail ("lowest_lod", "low_lod", "medium_lod",
    // "high_lod").
    std::array<MeshBlock, 4> lods{};
    MeshBlock skin{};
    MeshBlock physicsConvex{};
    MeshBlock physicsMesh{};

    // The available level of detail closest to `wanted`, preferring higher
    // detail; -1 if there is none.
    int closestLod(int wanted) const;
};

std::expected<MeshHeader, std::string> parseMeshHeader(std::span<const uint8_t> data);

// A rigged mesh's skin block (llmodel.cpp LLMeshSkinInfo). Matrices are
// converted to column vectors: a vertex goes to
// joint * inverseBind[j] * bindShape * position.
struct MeshSkin
{
    std::vector<std::string> jointNames;
    std::vector<glm::mat4> inverseBind; // per joint
    glm::mat4 bindShape{1.0f};
    // Optional: per joint, a transform whose translation is where the mesh
    // wants that joint (relative to its parent); empty if none.
    std::vector<glm::mat4> alternateBind;
    float pelvisOffset = 0.0f;
    bool lockScaleIfJointPosition = false;
};

// Decodes the skin block (still compressed).
std::expected<MeshSkin, std::string> decodeMeshSkin(std::span<const uint8_t> compressed);

// Inflates zlib data (refusing to grow beyond maxSize).
std::expected<std::vector<uint8_t>, std::string> inflateZlib(std::span<const uint8_t> data,
                                                             size_t maxSize = 128u << 20);

// Decodes one level of detail block (still compressed) into faces. Positions
// are in the unit prim (-0.5..0.5), like generated prims; face i is texture
// entry i.
std::expected<std::vector<volume::VolumeFace>, std::string>
decodeMeshLod(std::span<const uint8_t> compressed);

} // namespace viewer::assets
