#pragma once

// Meshes share a few large vertex and index buffers instead of owning two
// buffers each: binding thousands of separate buffers in a frame is slow
// (SDL tracks every buffer a command buffer uses in a list it searches on
// each bind), and draws that share buffers needn't rebind them.
//
// One set of blocks per vertex layout (their strides differ); a block is a
// vertex buffer and an index buffer, each handed out in ranges. A range a
// mesh lets go of is reused only once the frames that may still draw it are
// done (kFramesInFlight frames later).

#include "viewer/render/Mesh.hpp"

#include <SDL3/SDL_gpu.h>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace viewer::render
{

// Hands out ranges of [0, size): the smallest free range that fits, merging
// neighbours when ranges come back.
class RangeAllocator
{
public:
    explicit RangeAllocator(uint32_t size);

    std::optional<uint32_t> allocate(uint32_t size);
    void free(uint32_t offset, uint32_t size);

    uint32_t size() const { return size_; }
    uint32_t freeSpace() const { return free_; }

private:
    void insert(uint32_t offset, uint32_t size);
    void erase(std::map<uint32_t, uint32_t>::iterator it);

    uint32_t size_;
    uint32_t free_;
    std::map<uint32_t, uint32_t> byOffset_;    // offset -> size
    std::multimap<uint32_t, uint32_t> bySize_; // size -> offset
};

struct MeshBlock
{
    VertexLayout layout = VertexLayout::Object;
    SDL_GPUBuffer* vertices = nullptr;
    SDL_GPUBuffer* indices = nullptr;
    RangeAllocator vertexRanges; // in vertices
    RangeAllocator indexRanges;  // in indices
};

class MeshPool
{
public:
    // How many frames may still be drawing a mesh after it's released.
    static constexpr uint64_t kFramesInFlight = 4;

    explicit MeshPool(SDL_GPUDevice* device);
    ~MeshPool();

    MeshPool(const MeshPool&) = delete;
    MeshPool& operator=(const MeshPool&) = delete;

    // Room for a mesh and its contents uploaded; an empty mesh on failure.
    Mesh create(VertexLayout layout, const void* vertices, uint32_t vertexCount,
                std::span<const uint32_t> indices);

    // Called by ~Mesh.
    void release(const Mesh& mesh);
    // Once a frame (after submitting it): ranges released long enough ago
    // become free.
    void endFrame();

    struct Stats
    {
        size_t blocks = 0;
        uint64_t bytes = 0;     // in all blocks
        uint64_t freeBytes = 0; // not handed out
    };
    Stats stats() const;

private:
    struct Released
    {
        MeshBlock* block;
        uint32_t firstVertex, vertexCount, firstIndex, indexCount;
        uint64_t frame;
    };

    MeshBlock* newBlock(VertexLayout layout, uint32_t vertexCount, uint32_t indexCount);

    SDL_GPUDevice* device_;
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<MeshBlock>> blocks_;
    std::vector<Released> released_;
    uint64_t frame_ = 0;
};

uint32_t vertexStride(VertexLayout layout);

} // namespace viewer::render
