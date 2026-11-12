#pragma once

// Internal to the render module: GPU objects shared by Renderer.cpp and
// RendererScene.cpp.

#include "viewer/render/Renderer.hpp"

namespace viewer::render
{

struct Renderer::Pipelines
{
    SDL_GPUDevice* device = nullptr;
    SDL_GPUGraphicsPipeline* blinnPhong = nullptr;
    SDL_GPUGraphicsPipeline* blinnPhongBlend = nullptr;
    SDL_GPUGraphicsPipeline* pbr = nullptr;
    SDL_GPUGraphicsPipeline* blinnPhongSkinned = nullptr;
    SDL_GPUGraphicsPipeline* blinnPhongSkinnedBlend = nullptr;
    SDL_GPUGraphicsPipeline* pbrSkinned = nullptr;
    SDL_GPUGraphicsPipeline* terrainBlinnPhong = nullptr;
    SDL_GPUGraphicsPipeline* terrainPbr = nullptr;
    SDL_GPUGraphicsPipeline* tonemap = nullptr;
    SDL_GPUGraphicsPipeline* sky = nullptr;
    SDL_GPUSampler* repeatSampler = nullptr;
    SDL_GPUSampler* clampSampler = nullptr;

    ~Pipelines()
    {
        for(SDL_GPUGraphicsPipeline* p :
            {blinnPhong, blinnPhongBlend, pbr, blinnPhongSkinned, blinnPhongSkinnedBlend,
             pbrSkinned, terrainBlinnPhong, terrainPbr, tonemap, sky})
        {
            if(p)
            {
                SDL_ReleaseGPUGraphicsPipeline(device, p);
            }
        }
        for(SDL_GPUSampler* s : {repeatSampler, clampSampler})
        {
            if(s)
            {
                SDL_ReleaseGPUSampler(device, s);
            }
        }
    }
};

} // namespace viewer::render
