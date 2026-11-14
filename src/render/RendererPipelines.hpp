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
    // HUD attachments into the HUD image (legacy and PBR, opaque and
    // blended), the HUD image over the view, and on a panel in the scene.
    SDL_GPUGraphicsPipeline* hudLegacy[2] = {};
    SDL_GPUGraphicsPipeline* hudPbr[2] = {};
    SDL_GPUGraphicsPipeline* hudComposite = nullptr;
    SDL_GPUGraphicsPipeline* hudPanel = nullptr;
    SDL_GPUGraphicsPipeline* pick = nullptr; // pick.frag: what a ray hits
    // The highlighted prim: its visible pixels, then an outline around them.
    SDL_GPUGraphicsPipeline* outlineMask = nullptr;
    SDL_GPUGraphicsPipeline* outline = nullptr;
    // Labels: over the view (depth tested), and into the HUD image.
    // Glow: the glowing faces into the mask (unskinned and skinned), the view
    // times it into the glow image, and blurring that.
    SDL_GPUGraphicsPipeline* glowMask = nullptr;
    SDL_GPUGraphicsPipeline* glowMaskSkinned = nullptr;
    SDL_GPUGraphicsPipeline* glowExtract = nullptr;
    SDL_GPUGraphicsPipeline* glowBlur = nullptr;
    SDL_GPUGraphicsPipeline* label = nullptr;
    SDL_GPUGraphicsPipeline* hudLabel = nullptr;
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
                                          hudLegacy[0],
                                          hudLegacy[1],
                                          hudPbr[0],
                                          hudPbr[1],
                                          hudComposite,
                                          hudPanel,
                                          pick,
                                          outlineMask,
                                          outline,
                                          label,
                                          glowMask,
                                          glowMaskSkinned,
                                          glowExtract,
                                          glowBlur,
                                          hudLabel,
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
