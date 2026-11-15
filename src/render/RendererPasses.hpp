#pragma once

// Internal to the render module: the uniform blocks the scene shaders share
// (std140, must match the shaders) and ScenePass, the draw state of one
// render pass. Shared by RendererScene.cpp (the view) and RendererProbes.cpp
// (reflection probe faces).

#include "RendererPipelines.hpp"
#include "viewer/render/Mesh.hpp"

#include "viewer/core/Log.hpp"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_gpu.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <span>

namespace viewer::render
{

// The scene's color (HDR, linear), as rendered and in reflection probes.
inline constexpr SDL_GPUTextureFormat kHdrFormat = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
// Glow: how much each pixel glows (added up), and the small glow images
// (8 bits, as the official viewer's when RenderGlowHDR is off).
inline constexpr SDL_GPUTextureFormat kGlowMaskFormat = SDL_GPU_TEXTUREFORMAT_R16_FLOAT;
inline constexpr SDL_GPUTextureFormat kGlowFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
// Occlusion culling: the depth made small (OcclusionMap).
inline constexpr SDL_GPUTextureFormat kOcclusionFormat = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
// The HUD image: sRGB-encoded values (as the official viewer's HUDs, drawn
// after tonemapping), premultiplied by coverage.
inline constexpr SDL_GPUTextureFormat kHudFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

struct FrameVS
{
    glm::mat4 viewProjection;
    glm::vec4 cameraPosition;
};
struct TerrainVS
{
    glm::mat4 model;
    glm::vec4 textureScale;
};
// common.glsl FRAME_FS_BLOCK.
struct FrameFS
{
    glm::vec4 cameraPosition;
    glm::vec4 sunDirection;
    glm::vec4 sunColor;
    glm::vec4 skyAmbient;
    glm::vec4 groundAmbient;
    // The atmosphere (atmosphere.glsl).
    glm::vec4 atmoLight;
    glm::vec4 atmoSunlight;
    glm::vec4 atmoAmbient;
    glm::vec4 blueHorizon;
    glm::vec4 blueDensity;
    glm::vec4 atmoParams;
    glm::vec4 glow;
    glm::vec4 sunDisc;
    glm::vec4 moonDisc;
    glm::vec4 moonInfo;
    glm::vec4 cloudColor;
    glm::vec4 cloudPos1;
    glm::vec4 cloudPos2;
    glm::vec4 cloudDome;
    glm::mat4 shadowMatrices[4];
    glm::vec4 shadowSplits;
    glm::vec4 shadowTexels;
    glm::vec4 shadowInfo;
    glm::vec4 viewAxis;
    glm::vec4 lightInfo;
    // Reflection probes (probes.glsl): x the sky's probe ambiance, y the
    // radiance's last mip level, z ready (1), w classic mode (1).
    glm::vec4 probeInfo;
    glm::vec4 clusterGrid{0.0f};  // x, y a tile's size in pixels, z, w tiles across, down
    glm::vec4 clusterDepth{0.0f}; // x slices, y near, z log(far / near), w on (1)
    glm::vec4 waterInfo{0.0f};    // x height, y on (1), z fog density, w fog ks
    glm::vec4 waterFog{0.0f};     // rgb fog color (as the settings)
};
struct SkyFS
{
    glm::mat4 inverseViewProjection;
};
struct TerrainPbrFS
{
    glm::vec4 baseColorFactor[4];
    glm::vec4 emissiveTint[4];
    glm::vec4 factors[4];
};

struct ShaderDesc
{
    std::span<const uint8_t> code;
    SDL_GPUShaderStage stage;
    uint32_t samplers;
    uint32_t uniformBuffers;
    uint32_t storageBuffers = 0;
};

inline SDL_GPUShader* createShader(SDL_GPUDevice* device, const ShaderDesc& desc, const char* name)
{
    SDL_GPUShaderCreateInfo info = {};
    info.code_size = desc.code.size();
    info.code = desc.code.data();
    info.entrypoint = "main";
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    info.stage = desc.stage;
    info.num_samplers = desc.samplers;
    info.num_uniform_buffers = desc.uniformBuffers;
    info.num_storage_buffers = desc.storageBuffers;
    SDL_GPUShader* shader = SDL_CreateGPUShader(device, &info);
    if(!shader)
    {
        log::error("creating shader {} failed: {}", name, SDL_GetError());
    }
    return shader;
}

template <typename T>
void pushVertexUniforms(SDL_GPUCommandBuffer* commands, uint32_t slot, const T& data)
{
    SDL_PushGPUVertexUniformData(commands, slot, &data, sizeof(T));
}

template <typename T>
void pushFragmentUniforms(SDL_GPUCommandBuffer* commands, uint32_t slot, const T& data)
{
    SDL_PushGPUFragmentUniformData(commands, slot, &data, sizeof(T));
}

// One render pass's draw state. Sets state only when it changes: meshes
// share buffers (MeshPool) and draws are sorted so that neighbours share
// pipelines and textures. Binding a pipeline pushes the frame's uniforms
// (slot 0 of each stage), and terrain's binds the light and probe buffers. A pass that
// sees all around (a probe face) also draws what's outside the view's
// frustum (the scene's shadow-only draws).
class ScenePass
{
public:
    // `clusters`: the view's cluster lists (Clusters.hpp), or any buffer if
    // the pass has none (FrameFS::clusterDepth.w 0).
    ScenePass(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const Renderer::Pipelines& p,
              const FrameVS& frameVs, const FrameFS& frameFs, SDL_GPUBuffer* lights,
              SDL_GPUBuffer* probes, SDL_GPUBuffer* clusters, bool allAround = false)
        : commands_(commands), pass_(pass), p_(p), frameVs_(frameVs), frameFs_(frameFs),
          lights_(lights), probes_(probes), clusters_(clusters), allAround_(allAround)
    {
    }

    SDL_GPUBuffer* clusters() const { return clusters_; }
    SDL_GPUBuffer* lights() const { return lights_; }
    SDL_GPUBuffer* probes() const { return probes_; }
    // After binding buffers directly: drawMesh binds its own again.
    void forgetBuffers()
    {
        boundVertices_ = nullptr;
        boundIndices_ = nullptr;
    }

    bool allAround() const { return allAround_; }

    SDL_GPUCommandBuffer* commands() const { return commands_; }
    SDL_GPURenderPass* pass() const { return pass_; }
    const Renderer::Pipelines& pipelines() const { return p_; }
    const FrameFS& frameFs() const { return frameFs_; }
    SDL_GPUGraphicsPipeline* boundPipeline() const { return boundPipeline_; }

    void bindPipeline(SDL_GPUGraphicsPipeline* pipeline)
    {
        if(pipeline == boundPipeline_)
        {
            return;
        }
        boundPipeline_ = pipeline;
        SDL_BindGPUGraphicsPipeline(pass_, pipeline);
        pushVertexUniforms(commands_, 0, frameVs_);
        pushFragmentUniforms(commands_, 0, frameFs_);
        if(pipeline == p_.terrainBlinnPhong || pipeline == p_.terrainPbr)
        {
            SDL_GPUBuffer* storage[] = {lights_, probes_, clusters_};
            SDL_BindGPUFragmentStorageBuffers(pass_, 0, storage, 3);
        }
    }

    // Binds only the slots that changed (the span from the first to the
    // last): SDL tracks each texture bound in a list it searches on every
    // bind, so the slots every draw shares (shadows, probes) shouldn't go
    // again with each material.
    void bindSamplers(std::span<const SDL_GPUTextureSamplerBinding> samplers)
    {
        size_t first = samplers.size();
        size_t last = 0;
        for(size_t i = 0; i < samplers.size(); i++)
        {
            const bool same = i < boundSamplerCount_ &&
                              boundSamplers_[i].texture == samplers[i].texture &&
                              boundSamplers_[i].sampler == samplers[i].sampler;
            if(!same)
            {
                first = std::min(first, i);
                last = i;
            }
        }
        if(first == samplers.size())
        {
            return;
        }
        std::ranges::copy(samplers, boundSamplers_.begin());
        boundSamplerCount_ = std::max(boundSamplerCount_, samplers.size());
        SDL_BindGPUFragmentSamplers(pass_, static_cast<Uint32>(first), samplers.data() + first,
                                    static_cast<Uint32>(last - first + 1));
    }

    void drawMesh(const Mesh& mesh, uint32_t instance = 0, uint32_t instances = 1)
    {
        if(mesh.vertexBuffer() != boundVertices_)
        {
            boundVertices_ = mesh.vertexBuffer();
            const SDL_GPUBufferBinding binding = {boundVertices_, 0};
            SDL_BindGPUVertexBuffers(pass_, 0, &binding, 1);
        }
        if(mesh.indexBuffer() != boundIndices_)
        {
            boundIndices_ = mesh.indexBuffer();
            const SDL_GPUBufferBinding binding = {boundIndices_, 0};
            SDL_BindGPUIndexBuffer(pass_, &binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        }
        SDL_DrawGPUIndexedPrimitives(pass_, mesh.indexCount, instances, mesh.firstIndex,
                                     static_cast<Sint32>(mesh.firstVertex), instance);
    }

    // Something else was bound behind this pass's back (the sky's pipeline).
    void forgetPipeline()
    {
        boundPipeline_ = nullptr;
        boundSamplerCount_ = 0;
    }

private:
    SDL_GPUCommandBuffer* commands_;
    SDL_GPURenderPass* pass_;
    const Renderer::Pipelines& p_;
    const FrameVS& frameVs_;
    const FrameFS& frameFs_;
    SDL_GPUBuffer* lights_;
    SDL_GPUBuffer* probes_;
    SDL_GPUBuffer* clusters_;
    bool allAround_;
    SDL_GPUBuffer* boundVertices_ = nullptr;
    SDL_GPUBuffer* boundIndices_ = nullptr;
    SDL_GPUGraphicsPipeline* boundPipeline_ = nullptr;
    std::array<SDL_GPUTextureSamplerBinding, 16> boundSamplers_{};
    size_t boundSamplerCount_ = 0;
};

} // namespace viewer::render
