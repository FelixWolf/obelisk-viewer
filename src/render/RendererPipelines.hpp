#pragma once

// Internal to the render module: GPU objects shared by Renderer.cpp and
// RendererScene.cpp.

#include "viewer/render/Renderer.hpp"
#include "viewer/render/ShadowCascades.hpp"

#include <string_view>
#include <unordered_map>

namespace viewer::render
{

// What draws in the scene pass, by its samples a pixel (they're part of a
// pipeline). Reflection probes' faces always have one.
struct ScenePipelines
{
    uint32_t samples = 1;
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
    SDL_GPUGraphicsPipeline* sky = nullptr;
    SDL_GPUGraphicsPipeline* hudPanel = nullptr; // the HUD image on a panel
    SDL_GPUGraphicsPipeline* water = nullptr;
    // Particles: one per blend, made when first needed.
    std::unordered_map<uint16_t, SDL_GPUGraphicsPipeline*> particle;

    bool complete() const
    {
        return blinnPhong && blinnPhongBlend && pbr && blinnPhongSkinned &&
               blinnPhongSkinnedBlend && pbrSkinned && terrainBlinnPhong && terrainPbr && sky &&
               hudPanel && water;
    }

    void release(SDL_GPUDevice* device)
    {
        for(SDL_GPUGraphicsPipeline* p :
            {blinnPhong, blinnPhongBlend, pbr, blinnPhongSkinned, blinnPhongSkinnedBlend,
             pbrSkinned, terrainBlinnPhong, terrainPbr, sky, hudPanel, water})
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
        for(const auto& [key, pipeline] : particle)
        {
            if(pipeline)
            {
                SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
            }
        }
        *this = ScenePipelines{};
    }
};

// One, two, four and eight samples.
inline constexpr size_t kSampleCounts = 4;
inline size_t sampleIndex(uint32_t samples)
{
    return samples >= 8 ? 3 : samples >= 4 ? 2 : samples >= 2 ? 1 : 0;
}

struct Renderer::Pipelines
{
    SDL_GPUDevice* device = nullptr;
    ScenePipelines scene[kSampleCounts];
    // What they're made from (kept).
    std::unordered_map<std::string_view, SDL_GPUShader*> sceneShaders;
    SDL_GPUGraphicsPipeline* depthResolve = nullptr; // multisampled depth to one sample
    SDL_GPUGraphicsPipeline* tonemap = nullptr;
    SDL_GPUGraphicsPipeline* fxaa = nullptr; // the tonemapped view, smoothed, into the target
    // Dynamic exposure: the scene's luminance, then the exposure from it.
    SDL_GPUGraphicsPipeline* luminance = nullptr;
    SDL_GPUGraphicsPipeline* exposure = nullptr;
    // Reflection probes: filtering a capture into radiance and irradiance,
    // and making the split-sum table.
    SDL_GPUGraphicsPipeline* probeRadiance = nullptr;
    SDL_GPUGraphicsPipeline* probeIrradiance = nullptr;
    SDL_GPUGraphicsPipeline* brdfTable = nullptr;
    // HUD attachments into the HUD image (legacy and PBR, opaque and
    // blended), the HUD image over the view.
    SDL_GPUGraphicsPipeline* hudLegacy[2] = {};
    SDL_GPUGraphicsPipeline* hudPbr[2] = {};
    SDL_GPUGraphicsPipeline* hudComposite = nullptr;
    // pick.frag: what a ray hits; [0] front faces only, as drawn (and as
    // the official pick), [1] both sides (double-sided PBR materials).
    SDL_GPUGraphicsPipeline* pick[2] = {};
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
    SDL_GPUGraphicsPipeline* occlusionReduce = nullptr; // the depth made small
    SDL_GPUGraphicsPipeline* label = nullptr;
    SDL_GPUGraphicsPipeline* hudLabel = nullptr;
    SDL_GPUGraphicsPipeline* panelImage = nullptr; // a Panel's image, over the view
    // Shadow maps (depth only): opaque casters, and ones with see-through parts.
    SDL_GPUGraphicsPipeline* shadowObject = nullptr;
    SDL_GPUGraphicsPipeline* shadowObjectMasked = nullptr;
    SDL_GPUGraphicsPipeline* shadowSkinned = nullptr;
    SDL_GPUGraphicsPipeline* shadowSkinnedMasked = nullptr;
    SDL_GPUGraphicsPipeline* shadowTerrain = nullptr;
    // Particles (their pipelines: ScenePipelines), and their glow into the
    // glow mask.
    SDL_GPUShader* particleVert = nullptr;
    SDL_GPUShader* particleFrag = nullptr;
    SDL_GPUShader* particleGlowFrag = nullptr;
    SDL_GPUGraphicsPipeline* particleGlow = nullptr;
    // The water mask: the water's squares, and exclusion surfaces.
    SDL_GPUGraphicsPipeline* waterMask = nullptr;
    SDL_GPUGraphicsPipeline* waterMaskObject = nullptr;
    // An eye's image into a headset's (per its format, made when first
    // needed, from these shaders, kept).
    SDL_GPUShader* eyeCopyVert = nullptr;
    SDL_GPUShader* eyeCopyFrag = nullptr;
    std::unordered_map<int, SDL_GPUGraphicsPipeline*> eyeCopy;
    SDL_GPUSampler* repeatSampler = nullptr;
    SDL_GPUSampler* clampSampler = nullptr;
    SDL_GPUSampler* shadowSampler = nullptr; // compares depth, filters the results

    ~Pipelines()
    {
        for(SDL_GPUGraphicsPipeline* p : {depthResolve,
                                          tonemap,
                                          fxaa,
                                          luminance,
                                          exposure,
                                          probeRadiance,
                                          probeIrradiance,
                                          brdfTable,
                                          hudLegacy[0],
                                          hudLegacy[1],
                                          hudPbr[0],
                                          hudPbr[1],
                                          hudComposite,
                                          pick[0],
                                          pick[1],
                                          outlineMask,
                                          outline,
                                          label,
                                          occlusionReduce,
                                          glowMask,
                                          glowMaskSkinned,
                                          glowExtract,
                                          glowBlur,
                                          hudLabel,
                                          panelImage,
                                          shadowObject,
                                          shadowObjectMasked,
                                          shadowSkinned,
                                          shadowSkinnedMasked,
                                          shadowTerrain,
                                          particleGlow,
                                          waterMask,
                                          waterMaskObject})
        {
            if(p)
            {
                SDL_ReleaseGPUGraphicsPipeline(device, p);
            }
        }
        for(ScenePipelines& pipelines : scene)
        {
            pipelines.release(device);
        }
        for(const auto& [name, shader] : sceneShaders)
        {
            if(shader)
            {
                SDL_ReleaseGPUShader(device, shader);
            }
        }
        for(const auto& [format, pipeline] : eyeCopy)
        {
            SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
        }
        for(SDL_GPUShader* s :
            {particleVert, particleFrag, particleGlowFrag, eyeCopyVert, eyeCopyFrag})
        {
            if(s)
            {
                SDL_ReleaseGPUShader(device, s);
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
