// Water (Scene::water), as the official viewer draws it (lldrawpoolwater.cpp,
// waterF.glsl): after the opaque scene, which it shows through itself
// (copied first), before what's blended. Water fog for what's under it is in
// every surface's shader (atmosphere.glsl).

#include "RendererPasses.hpp"
#include "RendererPipelines.hpp"
#include "viewer/render/Renderer.hpp"

#include "viewer/core/Log.hpp"

#include <SDL3/SDL_error.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace viewer::render
{

namespace
{

struct WaterFS
{
    glm::vec4 waves;    // xy wave 1 direction, zw wave 2
    glm::vec4 params;   // x time, y normal blend, z fresnel scale, w fresnel offset
    glm::vec4 scale;    // xyz normal scale, w refraction scale
    glm::vec4 viewInfo; // x roughness, y 1 / width, z 1 / height, w camera under (1)
};

} // namespace

void Renderer::uploadWater(SDL_GPUCommandBuffer* commands, const Scene& scene)
{
    waterVertexCount_ = 0;
    if(!scene.water.enabled || scene.water.patches.empty())
    {
        return;
    }
    std::vector<glm::vec3> vertices;
    vertices.reserve(scene.water.patches.size() * 6);
    for(const Water::Patch& patch : scene.water.patches)
    {
        const glm::vec3 a(patch.corner, patch.height);
        const glm::vec3 b = a + glm::vec3(patch.size, 0.0f, 0.0f);
        const glm::vec3 c = a + glm::vec3(patch.size, patch.size, 0.0f);
        const glm::vec3 d = a + glm::vec3(0.0f, patch.size, 0.0f);
        vertices.insert(vertices.end(), {a, b, c, a, c, d});
    }
    const auto bytes = static_cast<uint32_t>(vertices.size() * sizeof(glm::vec3));
    if(!reserve(waterVertices_, bytes, SDL_GPU_BUFFERUSAGE_VERTEX))
    {
        return;
    }
    std::memcpy(SDL_MapGPUTransferBuffer(device_, waterVertices_.transfer, true), vertices.data(),
                bytes);
    SDL_UnmapGPUTransferBuffer(device_, waterVertices_.transfer);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    const SDL_GPUTransferBufferLocation from = {waterVertices_.transfer, 0};
    const SDL_GPUBufferRegion to = {waterVertices_.buffer.get(), 0, bytes};
    SDL_UploadToGPUBuffer(copy, &from, &to, true);
    SDL_EndGPUCopyPass(copy);
    waterVertexCount_ = static_cast<uint32_t>(vertices.size());
}

void Renderer::drawWaterMask(SDL_GPUCommandBuffer* commands, const Scene& scene,
                             const FrameVS& frameVs, uint32_t width, uint32_t height)
{
    const Pipelines& p = *pipelines_;
    // Nothing excludes water: all water (a white texel).
    waterMaskBinding_ = {white_.get(), p.clampSampler};
    if(!scene.water.enabled || waterVertexCount_ == 0 || !p.waterMask || !p.waterMaskObject ||
       std::ranges::none_of(scene.objects, &ObjectDraw::waterExclusion))
    {
        return;
    }
    if(!waterMask_ || waterMaskWidth_ != width || waterMaskHeight_ != height)
    {
        for(SDL_GPUTexture* t : {waterMask_, waterMaskDepth_})
        {
            if(t)
            {
                SDL_ReleaseGPUTexture(device_, t);
            }
        }
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        waterMask_ = SDL_CreateGPUTexture(device_, &info);
        info.format = depthFormat_;
        info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        waterMaskDepth_ = SDL_CreateGPUTexture(device_, &info);
        waterMaskWidth_ = width;
        waterMaskHeight_ = height;
        if(!waterMask_ || !waterMaskDepth_)
        {
            log::error("creating the water mask failed: {}", SDL_GetError());
            return;
        }
    }
    SDL_GPUColorTargetInfo color = {};
    color.texture = waterMask_;
    color.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;
    color.cycle = true;
    SDL_GPUDepthStencilTargetInfo depth = {};
    depth.texture = waterMaskDepth_;
    depth.clear_depth = 1.0f;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.cycle = true;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color, 1, &depth);
    pushVertexUniforms(commands, 0, frameVs);

    // The water: 1.
    SDL_BindGPUGraphicsPipeline(pass, p.waterMask);
    pushFragmentUniforms(commands, 0, glm::vec4(1.0f, scene.water.height, 0.0f, 0.0f));
    const SDL_GPUBufferBinding squares = {waterVertices_.buffer.get(), 0};
    SDL_BindGPUVertexBuffers(pass, 0, &squares, 1);
    SDL_DrawGPUPrimitives(pass, waterVertexCount_, 1, 0, 0);

    // Exclusion surfaces in front of it, above it: 0.
    SDL_BindGPUGraphicsPipeline(pass, p.waterMaskObject);
    pushFragmentUniforms(commands, 0, glm::vec4(0.0f, scene.water.height, 1.0f, 0.0f));
    for(const ObjectDraw& draw : scene.objects)
    {
        if(!draw.waterExclusion || draw.impostor >= 0 || !draw.mesh || !*draw.mesh ||
           draw.mesh->layout != VertexLayout::Object)
        {
            continue;
        }
        pushVertexUniforms(commands, 1, draw.transform);
        const SDL_GPUBufferBinding vertices = {draw.mesh->vertexBuffer(), 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
        const SDL_GPUBufferBinding indices = {draw.mesh->indexBuffer(), 0};
        SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        SDL_DrawGPUIndexedPrimitives(pass, draw.mesh->indexCount, 1, draw.mesh->firstIndex,
                                     static_cast<Sint32>(draw.mesh->firstVertex), 0);
    }
    SDL_EndGPURenderPass(pass);
    waterMaskBinding_ = {waterMask_, p.clampSampler};
}

void Renderer::copyForWater(SDL_GPUCommandBuffer* commands, uint32_t width, uint32_t height)
{
    if(!waterScene_ || waterSceneWidth_ != width || waterSceneHeight_ != height)
    {
        if(waterScene_)
        {
            SDL_ReleaseGPUTexture(device_, waterScene_);
        }
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = kHdrFormat;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        waterScene_ = SDL_CreateGPUTexture(device_, &info);
        waterSceneWidth_ = width;
        waterSceneHeight_ = height;
        if(!waterScene_)
        {
            log::error("creating the water's view copy failed: {}", SDL_GetError());
            return;
        }
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    const SDL_GPUTextureLocation from = {hdrTarget_, 0, 0, 0, 0, 0};
    const SDL_GPUTextureLocation to = {waterScene_, 0, 0, 0, 0, 0};
    SDL_CopyGPUTextureToTexture(copy, &from, &to, width, height, 1, false);
    SDL_EndGPUCopyPass(copy);
}

void Renderer::drawWater(ScenePass& pass, const Scene& scene, const Camera& camera, uint32_t width,
                         uint32_t height)
{
    const Pipelines& p = *pipelines_;
    if(!pass.pipelines().water || waterVertexCount_ == 0 || !waterScene_)
    {
        return;
    }
    const Water& w = scene.water;
    const bool under = camera.position.z < w.height;
    WaterFS fs;
    fs.waves = glm::vec4(w.wave1, w.wave2);
    fs.params = glm::vec4(w.time, w.normalBlend, w.fresnelScale, w.fresnelOffset);
    fs.scale = glm::vec4(w.normalScale, under ? w.scaleBelow : w.scaleAbove);
    fs.viewInfo = glm::vec4(w.roughness, 1.0f / static_cast<float>(width),
                            1.0f / static_cast<float>(height), under ? 1.0f : 0.0f);

    SDL_GPURenderPass* rp = pass.pass();
    pass.bindPipeline(pass.pipelines().water);
    pushFragmentUniforms(pass.commands(), 1, fs);
    auto texture = [](const GpuTexture* t, const GpuTexture& fallback)
    { return t && *t ? t->get() : fallback.get(); };
    const auto probes = probeSamplers();
    const SDL_GPUTextureSamplerBinding samplers[] = {
        {texture(w.normalMap, flatNormal_), p.repeatSampler},
        {texture(w.nextNormalMap ? w.nextNormalMap : w.normalMap, flatNormal_), p.repeatSampler},
        {waterScene_, p.clampSampler},
        {shadowMap_, p.shadowSampler},
        probes[0],
        probes[1],
        probes[2],
        waterMaskBinding_,
    };
    pass.bindSamplers(samplers);
    SDL_GPUBuffer* storage[] = {pass.probes(), pass.clusters()};
    SDL_BindGPUFragmentStorageBuffers(rp, 0, storage, 2);
    const SDL_GPUBufferBinding vertices = {waterVertices_.buffer.get(), 0};
    SDL_BindGPUVertexBuffers(rp, 0, &vertices, 1);
    pass.forgetBuffers();
    SDL_DrawGPUPrimitives(rp, waterVertexCount_, 1, 0, 0);
}

} // namespace viewer::render
