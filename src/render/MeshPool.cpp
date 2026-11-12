#include "viewer/render/MeshPool.hpp"

#include "viewer/core/Log.hpp"

#include <algorithm>
#include <cstring>

namespace viewer::render
{

namespace
{

constexpr uint32_t kBlockVertexBytes = 64u << 20;
constexpr uint32_t kBlockIndices = 8u << 20; // 32 MiB

} // namespace

uint32_t vertexStride(VertexLayout layout)
{
    switch(layout)
    {
    case VertexLayout::Object:
        return sizeof(ObjectVertex);
    case VertexLayout::Skinned:
        return sizeof(SkinnedVertex);
    case VertexLayout::Terrain:
        return sizeof(TerrainVertex);
    }
    return sizeof(ObjectVertex);
}

// --- RangeAllocator -----------------------------------------------------------

RangeAllocator::RangeAllocator(uint32_t size) : size_(size), free_(0)
{
    if(size > 0)
    {
        insert(0, size);
    }
}

void RangeAllocator::insert(uint32_t offset, uint32_t size)
{
    byOffset_.emplace(offset, size);
    bySize_.emplace(size, offset);
    free_ += size;
}

void RangeAllocator::erase(std::map<uint32_t, uint32_t>::iterator it)
{
    auto [first, last] = bySize_.equal_range(it->second);
    for(auto s = first; s != last; ++s)
    {
        if(s->second == it->first)
        {
            bySize_.erase(s);
            break;
        }
    }
    free_ -= it->second;
    byOffset_.erase(it);
}

std::optional<uint32_t> RangeAllocator::allocate(uint32_t size)
{
    if(size == 0)
    {
        return std::nullopt;
    }
    const auto fit = bySize_.lower_bound(size);
    if(fit == bySize_.end())
    {
        return std::nullopt;
    }
    const uint32_t offset = fit->second;
    const uint32_t rangeSize = fit->first;
    erase(byOffset_.find(offset));
    if(rangeSize > size)
    {
        insert(offset + size, rangeSize - size);
    }
    return offset;
}

void RangeAllocator::free(uint32_t offset, uint32_t size)
{
    if(size == 0)
    {
        return;
    }
    // Merge with the free ranges right after and right before.
    if(auto next = byOffset_.find(offset + size); next != byOffset_.end())
    {
        size += next->second;
        erase(next);
    }
    if(auto next = byOffset_.lower_bound(offset); next != byOffset_.begin())
    {
        auto previous = std::prev(next);
        if(previous->first + previous->second == offset)
        {
            offset = previous->first;
            size += previous->second;
            erase(previous);
        }
    }
    insert(offset, size);
}

// --- Mesh ---------------------------------------------------------------------

Mesh& Mesh::operator=(Mesh&& other) noexcept
{
    if(this != &other)
    {
        reset();
        pool = std::exchange(other.pool, nullptr);
        block = std::exchange(other.block, nullptr);
        firstVertex = other.firstVertex;
        vertexCount = other.vertexCount;
        firstIndex = other.firstIndex;
        indexCount = std::exchange(other.indexCount, 0);
        layout = other.layout;
        boundsMin = other.boundsMin;
        boundsMax = other.boundsMax;
    }
    return *this;
}

void Mesh::reset()
{
    if(pool && block)
    {
        pool->release(*this);
    }
    pool = nullptr;
    block = nullptr;
    indexCount = 0;
}

SDL_GPUBuffer* Mesh::vertexBuffer() const
{
    return block ? block->vertices : nullptr;
}

SDL_GPUBuffer* Mesh::indexBuffer() const
{
    return block ? block->indices : nullptr;
}

// --- MeshPool -----------------------------------------------------------------

MeshPool::MeshPool(SDL_GPUDevice* device) : device_(device) {}

MeshPool::~MeshPool()
{
    for(const auto& block : blocks_)
    {
        SDL_ReleaseGPUBuffer(device_, block->vertices);
        SDL_ReleaseGPUBuffer(device_, block->indices);
    }
    if(transfer_)
    {
        SDL_ReleaseGPUTransferBuffer(device_, transfer_);
    }
}

MeshBlock* MeshPool::newBlock(VertexLayout layout, uint32_t vertexCount, uint32_t indexCount)
{
    const uint32_t stride = vertexStride(layout);
    // Usually the standard size; a mesh larger than that gets a block of its
    // own size.
    const uint32_t vertices = std::max(kBlockVertexBytes / stride, vertexCount);
    const uint32_t indices = std::max(kBlockIndices, indexCount);
    if(static_cast<uint64_t>(vertices) * stride > UINT32_MAX ||
       static_cast<uint64_t>(indices) * sizeof(uint32_t) > UINT32_MAX)
    {
        return nullptr;
    }

    SDL_GPUBufferCreateInfo info = {};
    info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    info.size = vertices * stride;
    SDL_GPUBuffer* vertexBuffer = SDL_CreateGPUBuffer(device_, &info);
    info.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    info.size = indices * static_cast<uint32_t>(sizeof(uint32_t));
    SDL_GPUBuffer* indexBuffer = SDL_CreateGPUBuffer(device_, &info);
    if(!vertexBuffer || !indexBuffer)
    {
        log::error("creating mesh buffers failed: {}", SDL_GetError());
        SDL_ReleaseGPUBuffer(device_, vertexBuffer);
        SDL_ReleaseGPUBuffer(device_, indexBuffer);
        return nullptr;
    }
    blocks_.push_back(std::make_unique<MeshBlock>(
        MeshBlock{layout, static_cast<uint32_t>(blocks_.size()), vertexBuffer, indexBuffer,
                  RangeAllocator(vertices), RangeAllocator(indices)}));
    return blocks_.back().get();
}

Mesh MeshPool::create(VertexLayout layout, const void* vertices, uint32_t vertexCount,
                      std::span<const uint32_t> indices)
{
    Mesh mesh;
    const auto indexCount = static_cast<uint32_t>(indices.size());
    if(vertexCount == 0 || indexCount == 0)
    {
        return mesh;
    }
    {
        std::lock_guard lock(mutex_);
        MeshBlock* chosen = nullptr;
        std::optional<uint32_t> firstVertex;
        std::optional<uint32_t> firstIndex;
        for(const auto& block : blocks_)
        {
            if(block->layout != layout || block->vertexRanges.freeSpace() < vertexCount ||
               block->indexRanges.freeSpace() < indexCount)
            {
                continue;
            }
            firstVertex = block->vertexRanges.allocate(vertexCount);
            if(!firstVertex)
            {
                continue;
            }
            firstIndex = block->indexRanges.allocate(indexCount);
            if(!firstIndex)
            {
                block->vertexRanges.free(*firstVertex, vertexCount);
                continue;
            }
            chosen = block.get();
            break;
        }
        if(!chosen)
        {
            chosen = newBlock(layout, vertexCount, indexCount);
            if(!chosen)
            {
                return mesh;
            }
            firstVertex = chosen->vertexRanges.allocate(vertexCount);
            firstIndex = chosen->indexRanges.allocate(indexCount);
        }
        mesh.pool = this;
        mesh.block = chosen;
        mesh.firstVertex = *firstVertex;
        mesh.vertexCount = vertexCount;
        mesh.firstIndex = *firstIndex;
        mesh.indexCount = indexCount;
        mesh.layout = layout;
    }

    // Staged; flush() uploads everything staged this frame at once. The
    // ranges were free for at least kFramesInFlight frames, so nothing
    // draws them while they're written.
    const uint32_t stride = vertexStride(layout);
    const uint32_t vertexBytes = vertexCount * stride;
    const uint32_t indexBytes = indexCount * static_cast<uint32_t>(sizeof(uint32_t));
    std::lock_guard lock(mutex_);
    const auto stage = [&](SDL_GPUBuffer* buffer, uint32_t offset, const void* data, uint32_t size)
    {
        copies_.push_back({buffer, offset, static_cast<uint32_t>(staging_.size()), size});
        const auto* bytes = static_cast<const uint8_t*>(data);
        staging_.insert(staging_.end(), bytes, bytes + size);
    };
    stage(mesh.block->vertices, mesh.firstVertex * stride, vertices, vertexBytes);
    stage(mesh.block->indices, mesh.firstIndex * static_cast<uint32_t>(sizeof(uint32_t)),
          indices.data(), indexBytes);
    return mesh;
}

void MeshPool::flush()
{
    std::lock_guard lock(mutex_);
    if(copies_.empty())
    {
        return;
    }
    const auto bytes = static_cast<uint32_t>(staging_.size());
    if(transferSize_ < bytes)
    {
        if(transfer_)
        {
            SDL_ReleaseGPUTransferBuffer(device_, transfer_);
        }
        uint32_t size = 1u << 20;
        while(size < bytes)
        {
            size *= 2;
        }
        SDL_GPUTransferBufferCreateInfo info = {};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = size;
        transfer_ = SDL_CreateGPUTransferBuffer(device_, &info);
        transferSize_ = transfer_ ? size : 0;
    }
    SDL_GPUCommandBuffer* commands = transfer_ ? SDL_AcquireGPUCommandBuffer(device_) : nullptr;
    if(!commands)
    {
        log::error("uploading meshes failed: {}", SDL_GetError());
        copies_.clear();
        staging_.clear();
        return;
    }
    // Cycled: the GPU may still be reading the previous frame's batch.
    auto* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device_, transfer_, true));
    std::memcpy(mapped, staging_.data(), bytes);
    SDL_UnmapGPUTransferBuffer(device_, transfer_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    for(const Copy& c : copies_)
    {
        const SDL_GPUTransferBufferLocation source = {transfer_, c.source};
        const SDL_GPUBufferRegion destination = {c.buffer, c.offset, c.size};
        SDL_UploadToGPUBuffer(copy, &source, &destination, false);
    }
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(commands);
    copies_.clear();
    staging_.clear();
}

void MeshPool::release(const Mesh& mesh)
{
    std::lock_guard lock(mutex_);
    released_.push_back(
        {mesh.block, mesh.firstVertex, mesh.vertexCount, mesh.firstIndex, mesh.indexCount, frame_});
}

void MeshPool::endFrame()
{
    std::lock_guard lock(mutex_);
    frame_++;
    std::erase_if(released_,
                  [&](const Released& r)
                  {
                      if(r.frame + kFramesInFlight > frame_)
                      {
                          return false;
                      }
                      r.block->vertexRanges.free(r.firstVertex, r.vertexCount);
                      r.block->indexRanges.free(r.firstIndex, r.indexCount);
                      return true;
                  });
}

MeshPool::Stats MeshPool::stats() const
{
    std::lock_guard lock(mutex_);
    Stats s;
    s.blocks = blocks_.size();
    for(const auto& block : blocks_)
    {
        const uint64_t stride = vertexStride(block->layout);
        s.bytes += block->vertexRanges.size() * stride + block->indexRanges.size() * 4ull;
        s.freeBytes +=
            block->vertexRanges.freeSpace() * stride + block->indexRanges.freeSpace() * 4ull;
    }
    return s;
}

} // namespace viewer::render
