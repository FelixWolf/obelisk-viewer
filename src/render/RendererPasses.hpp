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
    ScenePass(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const Renderer::Pipelines& p,
              const FrameVS& frameVs, const FrameFS& frameFs, SDL_GPUBuffer* lights,
              SDL_GPUBuffer* probes, bool allAround = false)
        : commands_(commands), pass_(pass), p_(p), frameVs_(frameVs), frameFs_(frameFs),
          lights_(lights), probes_(probes), allAround_(allAround)
    {
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
            SDL_GPUBuffer* storage[] = {lights_, probes_};
            SDL_BindGPUFragmentStorageBuffers(pass_, 0, storage, 2);
        }
    }

    void bindSamplers(std::span<const SDL_GPUTextureSamplerBinding> samplers)
    {
        if(samplers.size() == boundSamplerCount_ &&
           std::equal(samplers.begin(), samplers.end(), boundSamplers_.begin(),
                      [](const auto& a, const auto& b)
                      { return a.texture == b.texture && a.sampler == b.sampler; }))
        {
            return;
        }
        std::ranges::copy(samplers, boundSamplers_.begin());
        boundSamplerCount_ = samplers.size();
        SDL_BindGPUFragmentSamplers(pass_, 0, samplers.data(),
                                    static_cast<Uint32>(samplers.size()));
    }

    void drawMesh(const Mesh& mesh, uint32_t instance = 0)
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
        SDL_DrawGPUIndexedPrimitives(pass_, mesh.indexCount, 1, mesh.firstIndex,
                                     static_cast<Sint32>(mesh.firstVertex), instance);
    }

    // Something else was bound behind this pass's back (the sky's pipeline).
    void forgetPipeline() { boundPipeline_ = nullptr; }

private:
    SDL_GPUCommandBuffer* commands_;
    SDL_GPURenderPass* pass_;
    const Renderer::Pipelines& p_;
    const FrameVS& frameVs_;
    const FrameFS& frameFs_;
    SDL_GPUBuffer* lights_;
    SDL_GPUBuffer* probes_;
    bool allAround_;
    SDL_GPUBuffer* boundVertices_ = nullptr;
    SDL_GPUBuffer* boundIndices_ = nullptr;
    SDL_GPUGraphicsPipeline* boundPipeline_ = nullptr;
    std::array<SDL_GPUTextureSamplerBinding, 16> boundSamplers_{};
    size_t boundSamplerCount_ = 0;
};

} // namespace viewer::render
