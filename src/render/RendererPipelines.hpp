#pragma once

// Internal to the render module: GPU objects shared by Renderer.cpp and
// RendererScene.cpp.

#include "viewer/render/Renderer.hpp"
#include "viewer/render/ShadowCascades.hpp"

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
    // PBR variants: [blend][double sided][skinned], [0][0][x] being the two above.
    SDL_GPUGraphicsPipeline* pbrVariants[2][2][2] = {};
    SDL_GPUGraphicsPipeline* terrainBlinnPhong = nullptr;
    SDL_GPUGraphicsPipeline* terrainPbr = nullptr;
    SDL_GPUGraphicsPipeline* tonemap = nullptr;
    // Dynamic exposure: the scene's luminance, then the exposure from it.
    SDL_GPUGraphicsPipeline* luminance = nullptr;
    SDL_GPUGraphicsPipeline* exposure = nullptr;
    SDL_GPUGraphicsPipeline* sky = nullptr;
    // Reflection probes: filtering a capture into radiance and irradiance,
    // and making the split-sum table.
    SDL_GPUGraphicsPipeline* probeRadiance = nullptr;
    SDL_GPUGraphicsPipeline* probeIrradiance = nullptr;
    SDL_GPUGraphicsPipeline* brdfTable = nullptr;
    // Shadow maps (depth only): opaque casters, and ones with see-through parts.
    SDL_GPUGraphicsPipeline* shadowObject = nullptr;
    SDL_GPUGraphicsPipeline* shadowObjectMasked = nullptr;
    SDL_GPUGraphicsPipeline* shadowSkinned = nullptr;
    SDL_GPUGraphicsPipeline* shadowSkinnedMasked = nullptr;
    SDL_GPUGraphicsPipeline* shadowTerrain = nullptr;
    SDL_GPUSampler* repeatSampler = nullptr;
    SDL_GPUSampler* clampSampler = nullptr;
    SDL_GPUSampler* shadowSampler = nullptr; // compares depth, filters the results

    ~Pipelines()
    {
        for(SDL_GPUGraphicsPipeline* p : {blinnPhong,
                                          blinnPhongBlend,
                                          pbr,
                                          blinnPhongSkinned,
                                          blinnPhongSkinnedBlend,
                                          pbrSkinned,
                                          terrainBlinnPhong,
                                          terrainPbr,
                                          tonemap,
                                          luminance,
                                          exposure,
                                          sky,
                                          probeRadiance,
                                          probeIrradiance,
                                          brdfTable,
                                          shadowObject,
                                          shadowObjectMasked,
                                          shadowSkinned,
                                          shadowSkinnedMasked,
                                          shadowTerrain})
        {
            if(p)
            {
                SDL_ReleaseGPUGraphicsPipeline(device, p);
            }
        }
        for(auto& byBlend : pbrVariants)
        {
            for(auto& bySides : byBlend)
            {
                for(SDL_GPUGraphicsPipeline* v : bySides)
                {
                    if(v && v != pbr && v != pbrSkinned)
                    {
                        SDL_ReleaseGPUGraphicsPipeline(device, v);
                    }
                }
            }
        }
        for(SDL_GPUSampler* s : {repeatSampler, clampSampler, shadowSampler})
        {
            if(s)
            {
                SDL_ReleaseGPUSampler(device, s);
            }
        }
    }
};

} // namespace viewer::render
