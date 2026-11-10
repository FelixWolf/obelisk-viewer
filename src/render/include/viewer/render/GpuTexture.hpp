#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <utility>

namespace viewer::render
{

// Owning handle to a sampled 2D GPU texture. Move-only. Must not outlive the
// Renderer that created it. Releasing is safe while the GPU may still be using
// the texture; SDL defers the actual destruction.
class GpuTexture
{
public:
    GpuTexture() = default;
    GpuTexture(SDL_GPUDevice* device, SDL_GPUTexture* texture, uint32_t width, uint32_t height,
               uint32_t levels)
        : device_(device), texture_(texture), width_(width), height_(height), levels_(levels)
    {
    }
    ~GpuTexture() { reset(); }

    GpuTexture(GpuTexture&& other) noexcept { *this = std::move(other); }
    GpuTexture& operator=(GpuTexture&& other) noexcept
    {
        if(this != &other)
        {
            reset();
            device_ = std::exchange(other.device_, nullptr);
            texture_ = std::exchange(other.texture_, nullptr);
            width_ = other.width_;
            height_ = other.height_;
            levels_ = other.levels_;
        }
        return *this;
    }
    GpuTexture(const GpuTexture&) = delete;
    GpuTexture& operator=(const GpuTexture&) = delete;

    void reset()
    {
        if(texture_)
        {
            SDL_ReleaseGPUTexture(device_, texture_);
            texture_ = nullptr;
        }
    }

    SDL_GPUTexture* get() const { return texture_; }
    explicit operator bool() const { return texture_ != nullptr; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t levels() const { return levels_; }

private:
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUTexture* texture_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t levels_ = 0;
};

} // namespace viewer::render
