#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <utility>

namespace viewer::render
{

// Owning handle to a GPU buffer. Move-only; must not outlive its Renderer.
// Releasing while the GPU still uses it is safe (SDL defers destruction).
class GpuBuffer
{
public:
    GpuBuffer() = default;
    GpuBuffer(SDL_GPUDevice* device, SDL_GPUBuffer* buffer, uint32_t size)
        : device_(device), buffer_(buffer), size_(size)
    {
    }
    ~GpuBuffer() { reset(); }

    GpuBuffer(GpuBuffer&& other) noexcept { *this = std::move(other); }
    GpuBuffer& operator=(GpuBuffer&& other) noexcept
    {
        if(this != &other)
        {
            reset();
            device_ = std::exchange(other.device_, nullptr);
            buffer_ = std::exchange(other.buffer_, nullptr);
            size_ = other.size_;
        }
        return *this;
    }
    GpuBuffer(const GpuBuffer&) = delete;
    GpuBuffer& operator=(const GpuBuffer&) = delete;

    void reset()
    {
        if(buffer_)
        {
            SDL_ReleaseGPUBuffer(device_, buffer_);
            buffer_ = nullptr;
        }
    }

    SDL_GPUBuffer* get() const { return buffer_; }
    explicit operator bool() const { return buffer_ != nullptr; }
    uint32_t size() const { return size_; }

private:
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUBuffer* buffer_ = nullptr;
    uint32_t size_ = 0;
};

} // namespace viewer::render
