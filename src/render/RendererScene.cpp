// Scene rendering: pipelines for the four material families, the HDR + depth
// pass, tonemapping into the swapchain, and screenshots.

#include "RendererPipelines.hpp"
#include "viewer/render/Renderer.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/render/ShaderBytecode.hpp"

#include <SDL3/SDL_error.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstring>
#include <span>

namespace viewer::render
{

namespace
{

constexpr SDL_GPUTextureFormat kHdrFormat = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;

// Uniform blocks, std140. Must match the shaders.
struct FrameVS
{
    glm::mat4 viewProjection;
    glm::vec4 cameraPosition;
};
struct ObjectVS
{
    glm::mat4 model;
    glm::mat4 normalMatrix;
};
struct TerrainVS
{
    glm::mat4 model;
    glm::vec4 textureScale;
};
struct FrameFS
{
    glm::vec4 cameraPosition;
    glm::vec4 sunDirection;
    glm::vec4 sunColor;
    glm::vec4 skyAmbient;
    glm::vec4 groundAmbient;
};
struct BlinnPhongFS
{
    glm::vec4 diffuseTint;
    glm::vec4 specularTint;
    glm::vec4 options;
};
struct PbrFS
{
    glm::vec4 baseColorFactor;
    glm::vec4 emissiveTint;
    glm::vec4 factors;
};
struct TerrainPbrFS
{
    glm::vec4 baseColorFactor[4];
    glm::vec4 emissiveTint[4];
    glm::vec4 factors[4];
};
struct TonemapFS
{
    glm::vec4 settings;
};

struct ShaderDesc
{
    std::span<const uint8_t> code;
    SDL_GPUShaderStage stage;
    uint32_t samplers;
    uint32_t uniformBuffers;
};

SDL_GPUShader* createShader(SDL_GPUDevice* device, const ShaderDesc& desc, const char* name)
{
    SDL_GPUShaderCreateInfo info = {};
    info.code_size = desc.code.size();
    info.code = desc.code.data();
    info.entrypoint = "main";
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    info.stage = desc.stage;
    info.num_samplers = desc.samplers;
    info.num_uniform_buffers = desc.uniformBuffers;
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

} // namespace

bool Renderer::createPipelines()
{
    pipelines_ = std::make_unique<Pipelines>();
    Pipelines& p = *pipelines_;
    p.device = device_;

    depthFormat_ = SDL_GPUTextureSupportsFormat(device_, SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
                                                SDL_GPU_TEXTURETYPE_2D,
                                                SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
                       ? SDL_GPU_TEXTUREFORMAT_D32_FLOAT
                       : SDL_GPU_TEXTUREFORMAT_D24_UNORM;

    using namespace shaders;
    SDL_GPUShader* objectVert =
        createShader(device_, {kObjectVert, SDL_GPU_SHADERSTAGE_VERTEX, 0, 2}, "object.vert");
    SDL_GPUShader* blinnPhongFrag = createShader(
        device_, {kBlinnphongFrag, SDL_GPU_SHADERSTAGE_FRAGMENT, 3, 2}, "blinnphong.frag");
    SDL_GPUShader* pbrFrag =
        createShader(device_, {kPbrFrag, SDL_GPU_SHADERSTAGE_FRAGMENT, 4, 2}, "pbr.frag");
    SDL_GPUShader* terrainVert =
        createShader(device_, {kTerrainVert, SDL_GPU_SHADERSTAGE_VERTEX, 0, 2}, "terrain.vert");
    SDL_GPUShader* terrainBlinnPhongFrag =
        createShader(device_, {kTerrainBlinnphongFrag, SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1},
                     "terrain_blinnphong.frag");
    SDL_GPUShader* terrainPbrFrag = createShader(
        device_, {kTerrainPbrFrag, SDL_GPU_SHADERSTAGE_FRAGMENT, 5, 2}, "terrain_pbr.frag");
    SDL_GPUShader* fullscreenVert = createShader(
        device_, {kFullscreenVert, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0}, "fullscreen.vert");
    SDL_GPUShader* tonemapFrag =
        createShader(device_, {kTonemapFrag, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1}, "tonemap.frag");

    // Vertex layouts.
    const SDL_GPUVertexBufferDescription objectBuffer = {0, sizeof(ObjectVertex),
                                                         SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
    const SDL_GPUVertexAttribute objectAttributes[] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(ObjectVertex, position)},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(ObjectVertex, normal)},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(ObjectVertex, tangent)},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(ObjectVertex, uv)},
    };
    const SDL_GPUVertexBufferDescription terrainBuffer = {0, sizeof(TerrainVertex),
                                                          SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
    const SDL_GPUVertexAttribute terrainAttributes[] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(TerrainVertex, position)},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(TerrainVertex, normal)},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(TerrainVertex, blend)},
    };

    auto scenePipeline = [&](SDL_GPUShader* vert, SDL_GPUShader* frag, bool terrain,
                             const char* name) -> SDL_GPUGraphicsPipeline*
    {
        if(!vert || !frag)
        {
            return nullptr;
        }
        SDL_GPUColorTargetDescription color = {};
        color.format = kHdrFormat;

        SDL_GPUGraphicsPipelineCreateInfo info = {};
        info.vertex_shader = vert;
        info.fragment_shader = frag;
        info.vertex_input_state.vertex_buffer_descriptions =
            terrain ? &terrainBuffer : &objectBuffer;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_attributes = terrain ? terrainAttributes : objectAttributes;
        info.vertex_input_state.num_vertex_attributes =
            terrain ? static_cast<Uint32>(std::size(terrainAttributes))
                    : static_cast<Uint32>(std::size(objectAttributes));
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
        info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = true;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1;
        info.target_info.has_depth_stencil_target = true;
        info.target_info.depth_stencil_format = depthFormat_;
        SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device_, &info);
        if(!pipeline)
        {
            log::error("creating the {} pipeline failed: {}", name, SDL_GetError());
        }
        return pipeline;
    };

    p.blinnPhong = scenePipeline(objectVert, blinnPhongFrag, false, "Blinn-Phong");
    p.pbr = scenePipeline(objectVert, pbrFrag, false, "PBR");
    p.terrainBlinnPhong =
        scenePipeline(terrainVert, terrainBlinnPhongFrag, true, "terrain Blinn-Phong");
    p.terrainPbr = scenePipeline(terrainVert, terrainPbrFrag, true, "terrain PBR");

    if(fullscreenVert && tonemapFrag)
    {
        SDL_GPUColorTargetDescription color = {};
        color.format = swapchainFormat();
        SDL_GPUGraphicsPipelineCreateInfo info = {};
        info.vertex_shader = fullscreenVert;
        info.fragment_shader = tonemapFrag;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1;
        p.tonemap = SDL_CreateGPUGraphicsPipeline(device_, &info);
        if(!p.tonemap)
        {
            log::error("creating the tonemap pipeline failed: {}", SDL_GetError());
        }
    }

    for(SDL_GPUShader* s : {objectVert, blinnPhongFrag, pbrFrag, terrainVert, terrainBlinnPhongFrag,
                            terrainPbrFrag, fullscreenVert, tonemapFrag})
    {
        if(s)
        {
            SDL_ReleaseGPUShader(device_, s);
        }
    }

    SDL_GPUSamplerCreateInfo sampler = {};
    sampler.min_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sampler.enable_anisotropy = true;
    sampler.max_anisotropy = 8.0f;
    sampler.max_lod = 1000.0f;
    p.repeatSampler = SDL_CreateGPUSampler(device_, &sampler);
    sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler.enable_anisotropy = false;
    p.clampSampler = SDL_CreateGPUSampler(device_, &sampler);

    return p.blinnPhong && p.pbr && p.terrainBlinnPhong && p.terrainPbr && p.tonemap &&
           p.repeatSampler && p.clampSampler;
}

bool Renderer::createDefaultTextures()
{
    auto solid = [](uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        image::TextureData t;
        t.levels.push_back(image::MipLevel{1, 1, {r, g, b, a}});
        return t;
    };
    const image::TextureData white = solid(255, 255, 255, 255);
    const image::TextureData black = solid(0, 0, 0, 255);
    const image::TextureData flat = solid(128, 128, 255, 255); // +Z, full glossiness

    const image::TextureData* singles[] = {&white, &black, &flat};
    auto textures = uploadTextures(singles, false);
    white_ = std::move(textures[0]);
    black_ = std::move(textures[1]);
    flatNormal_ = std::move(textures[2]);

    const image::TextureData* whites[] = {&white, &white, &white, &white};
    const image::TextureData* blacks[] = {&black, &black, &black, &black};
    const image::TextureData* flats[] = {&flat, &flat, &flat, &flat};
    whiteLayers_ = uploadTextureArray(whites, false);

    // Terrain blend ramp: rows (v = blend noise) of falloffs from ~1 to ~0
    // across u. Stands in for the official viewer's alpha_gradient_2d image,
    // with transitions of varying position and sharpness.
    image::TextureData ramp;
    image::MipLevel rampLevel{256, 256, std::vector<uint8_t>(256 * 256 * 4)};
    for(uint32_t y = 0; y < 256; y++)
    {
        const float v = static_cast<float>(y) / 255.0f;
        const float center =
            0.5f + 0.15f * std::sin(v * 12.566f) + 0.05f * std::sin(v * 43.98f + 1.3f);
        const float width = 0.3f + 0.12f * std::sin(v * 18.85f + 0.7f);
        for(uint32_t x = 0; x < 256; x++)
        {
            const float u = static_cast<float>(x) / 255.0f;
            const float t = glm::clamp((u - (center - width)) / (2.0f * width), 0.0f, 1.0f);
            const float alpha = 1.0f - t * t * (3.0f - 2.0f * t);
            const auto value = static_cast<uint8_t>(8.0f + alpha * 246.0f);
            uint8_t* px = &rampLevel.pixels[(y * 256 + x) * 4];
            px[0] = px[1] = px[2] = value;
            px[3] = 255;
        }
    }
    ramp.levels.push_back(std::move(rampLevel));
    const image::TextureData* rampPointer = &ramp;
    terrainBlendRamp_ = std::move(uploadTextures({&rampPointer, 1}, false)[0]);
    blackLayers_ = uploadTextureArray(blacks, false);
    flatNormalLayers_ = uploadTextureArray(flats, false);
    return white_ && black_ && flatNormal_ && whiteLayers_ && blackLayers_ && flatNormalLayers_ &&
           terrainBlendRamp_;
}

bool Renderer::ensureTargets(uint32_t width, uint32_t height)
{
    if(hdrTarget_ && width == targetWidth_ && height == targetHeight_)
    {
        return true;
    }
    if(hdrTarget_)
    {
        SDL_ReleaseGPUTexture(device_, hdrTarget_);
        hdrTarget_ = nullptr;
    }
    if(depthTarget_)
    {
        SDL_ReleaseGPUTexture(device_, depthTarget_);
        depthTarget_ = nullptr;
    }

    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;

    info.format = kHdrFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    hdrTarget_ = SDL_CreateGPUTexture(device_, &info);

    info.format = depthFormat_;
    info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    depthTarget_ = SDL_CreateGPUTexture(device_, &info);

    if(!hdrTarget_ || !depthTarget_)
    {
        log::error("creating {}x{} render targets failed: {}", width, height, SDL_GetError());
        return false;
    }
    targetWidth_ = width;
    targetHeight_ = height;
    return true;
}

void Renderer::drawScene(const Frame& frame, const Scene& scene)
{
    if(!ensureTargets(frame.width, frame.height))
    {
        return;
    }
    const Pipelines& p = *pipelines_;
    SDL_GPUCommandBuffer* commands = frame.commands;

    const Camera& camera = scene.camera;
    const float aspect = static_cast<float>(frame.width) / static_cast<float>(frame.height);
    const glm::mat4 view = glm::lookAt(camera.position, camera.target, camera.up);
    const glm::mat4 projection =
        glm::perspective(camera.verticalFov, aspect, camera.nearPlane, camera.farPlane);

    const FrameVS frameVs{projection * view, glm::vec4(camera.position, 1.0f)};
    const Lighting& light = scene.lighting;
    const FrameFS frameFs{
        glm::vec4(camera.position, 1.0f),     glm::vec4(glm::normalize(light.sunDirection), 0.0f),
        glm::vec4(light.sunColor, 1.0f),      glm::vec4(light.skyAmbient, 1.0f),
        glm::vec4(light.groundAmbient, 1.0f),
    };

    SDL_GPUColorTargetInfo color = {};
    color.texture = hdrTarget_;
    color.clear_color = SDL_FColor{light.skyColor.r, light.skyColor.g, light.skyColor.b, 1.0f};
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo depth = {};
    depth.texture = depthTarget_;
    depth.clear_depth = 1.0f;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.cycle = true;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color, 1, &depth);

    auto texture = [](const GpuTexture* t, const GpuTexture& fallback)
    { return t && *t ? t->get() : fallback.get(); };
    auto bindMesh = [&](const Mesh& mesh)
    {
        const SDL_GPUBufferBinding vertices = {mesh.vertices.get(), 0};
        const SDL_GPUBufferBinding indices = {mesh.indices.get(), 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
        SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    };
    auto bindPipeline = [&](SDL_GPUGraphicsPipeline* pipeline)
    {
        SDL_BindGPUGraphicsPipeline(pass, pipeline);
        pushVertexUniforms(commands, 0, frameVs);
        pushFragmentUniforms(commands, 0, frameFs);
    };

    // Texture position of the scene origin, so terrain textures follow
    // global coordinates. Computed in double precision: global coordinates
    // are large.
    auto textureOffset = [&](float repeatsPerMeter)
    {
        const double period = 1.0 / static_cast<double>(repeatsPerMeter);
        return glm::vec2(static_cast<float>(std::fmod(scene.worldOrigin.x, period) / period),
                         static_cast<float>(std::fmod(scene.worldOrigin.y, period) / period));
    };
    auto terrainVs = [&](const glm::mat4& model, float repeatsPerMeter)
    {
        const glm::vec2 offset = textureOffset(repeatsPerMeter);
        return TerrainVS{model, glm::vec4(repeatsPerMeter, 0.0f, offset.x, offset.y)};
    };

    // Terrain first: it covers the most screen, so it fills depth early.
    for(const TerrainDraw& draw : scene.terrain)
    {
        if(!draw.mesh || !*draw.mesh || draw.mesh->layout != VertexLayout::Terrain)
        {
            continue;
        }
        if(const auto* m = std::get_if<TerrainBlinnPhongMaterial>(&draw.material))
        {
            bindPipeline(p.terrainBlinnPhong);
            const SDL_GPUTextureSamplerBinding samplers[] = {
                {texture(m->diffuseLayers, whiteLayers_), p.repeatSampler},
                {terrainBlendRamp_.get(), p.clampSampler}};
            SDL_BindGPUFragmentSamplers(pass, 0, samplers, 2);
            pushVertexUniforms(commands, 1, terrainVs(draw.transform, m->textureRepeatsPerMeter));
        }
        else if(const auto* t = std::get_if<TerrainPbrMaterial>(&draw.material))
        {
            bindPipeline(p.terrainPbr);
            const SDL_GPUTextureSamplerBinding samplers[] = {
                {texture(t->albedoLayers, whiteLayers_), p.repeatSampler},
                {texture(t->ormLayers, whiteLayers_), p.repeatSampler},
                {texture(t->emissiveLayers, blackLayers_), p.repeatSampler},
                {texture(t->normalLayers, flatNormalLayers_), p.repeatSampler},
                {terrainBlendRamp_.get(), p.clampSampler},
            };
            SDL_BindGPUFragmentSamplers(pass, 0, samplers, 5);
            pushVertexUniforms(commands, 1, terrainVs(draw.transform, t->textureRepeatsPerMeter));
            TerrainPbrFS fs{};
            for(size_t i = 0; i < 4; i++)
            {
                const auto& layer = t->layers[i];
                fs.baseColorFactor[i] = layer.baseColorFactor;
                fs.emissiveTint[i] = glm::vec4(layer.emissiveTint, layer.emissiveStrength);
                fs.factors[i] =
                    glm::vec4(layer.roughness, layer.metallic, layer.occlusionStrength, 0.0f);
            }
            pushFragmentUniforms(commands, 1, fs);
        }
        bindMesh(*draw.mesh);
        SDL_DrawGPUIndexedPrimitives(pass, draw.mesh->indexCount, 1, 0, 0, 0);
    }

    for(const ObjectDraw& draw : scene.objects)
    {
        if(!draw.mesh || !*draw.mesh || draw.mesh->layout != VertexLayout::Object)
        {
            continue;
        }
        const ObjectVS objectVs{draw.transform, glm::transpose(glm::inverse(draw.transform))};
        if(const auto* m = std::get_if<BlinnPhongMaterial>(&draw.material))
        {
            bindPipeline(p.blinnPhong);
            const SDL_GPUTextureSamplerBinding samplers[] = {
                {texture(m->diffuse, white_), p.repeatSampler},
                {texture(m->normal, flatNormal_), p.repeatSampler},
                {texture(m->specular, white_), p.repeatSampler},
            };
            SDL_BindGPUFragmentSamplers(pass, 0, samplers, 3);
            pushFragmentUniforms(commands, 1,
                                 BlinnPhongFS{m->diffuseTint,
                                              glm::vec4(m->specularTint, m->maxExponent),
                                              glm::vec4(m->alphaCutoff, 0.0f, 0.0f, 0.0f)});
        }
        else if(const auto* t = std::get_if<PbrMaterial>(&draw.material))
        {
            bindPipeline(p.pbr);
            const SDL_GPUTextureSamplerBinding samplers[] = {
                {texture(t->albedo, white_), p.repeatSampler},
                {texture(t->orm, white_), p.repeatSampler},
                {texture(t->emissive, black_), p.repeatSampler},
                {texture(t->normal, flatNormal_), p.repeatSampler},
            };
            SDL_BindGPUFragmentSamplers(pass, 0, samplers, 4);
            pushFragmentUniforms(
                commands, 1,
                PbrFS{t->baseColorFactor, glm::vec4(t->emissiveTint, t->emissiveStrength),
                      glm::vec4(t->roughness, t->metallic, t->occlusionStrength, t->alphaCutoff)});
        }
        pushVertexUniforms(commands, 1, objectVs);
        bindMesh(*draw.mesh);
        SDL_DrawGPUIndexedPrimitives(pass, draw.mesh->indexCount, 1, 0, 0, 0);
    }
    SDL_EndGPURenderPass(pass);

    drawTonemap(commands, frame.target, light.exposure, true);

    if(screenshotRequested_)
    {
        screenshotRequested_ = false;
        if(screenshotTexture_ &&
           (screenshotWidth_ != frame.width || screenshotHeight_ != frame.height))
        {
            SDL_ReleaseGPUTexture(device_, screenshotTexture_);
            SDL_ReleaseGPUTransferBuffer(device_, screenshotTransfer_);
            screenshotTexture_ = nullptr;
            screenshotTransfer_ = nullptr;
        }
        if(!screenshotTexture_)
        {
            SDL_GPUTextureCreateInfo info = {};
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = swapchainFormat();
            info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = frame.width;
            info.height = frame.height;
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            screenshotTexture_ = SDL_CreateGPUTexture(device_, &info);
            SDL_GPUTransferBufferCreateInfo transfer = {};
            transfer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
            transfer.size = frame.width * frame.height * 4;
            screenshotTransfer_ = SDL_CreateGPUTransferBuffer(device_, &transfer);
            screenshotWidth_ = frame.width;
            screenshotHeight_ = frame.height;
        }
        if(screenshotTexture_ && screenshotTransfer_)
        {
            drawTonemap(commands, screenshotTexture_, light.exposure, true);
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
            SDL_GPUTextureRegion source = {};
            source.texture = screenshotTexture_;
            source.w = frame.width;
            source.h = frame.height;
            source.d = 1;
            SDL_GPUTextureTransferInfo destination = {};
            destination.transfer_buffer = screenshotTransfer_;
            SDL_DownloadFromGPUTexture(copy, &source, &destination);
            SDL_EndGPUCopyPass(copy);
            screenshotPending_ = true;
        }
    }
}

void Renderer::drawTonemap(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target, float exposure,
                           bool clear)
{
    SDL_GPUColorTargetInfo color = {};
    color.texture = target;
    color.load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, pipelines_->tonemap);
    const SDL_GPUTextureSamplerBinding hdr = {hdrTarget_, pipelines_->clampSampler};
    SDL_BindGPUFragmentSamplers(pass, 0, &hdr, 1);
    pushFragmentUniforms(commands, 0, TonemapFS{glm::vec4(exposure, 0.0f, 0.0f, 0.0f)});
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

void Renderer::finishScreenshot()
{
    // Called after the frame's command buffer was submitted and waited on.
    screenshotPending_ = false;
    const auto* mapped =
        static_cast<const uint8_t*>(SDL_MapGPUTransferBuffer(device_, screenshotTransfer_, false));
    if(!mapped)
    {
        log::error("mapping the screenshot failed: {}", SDL_GetError());
        return;
    }
    image::MipLevel shot;
    shot.width = screenshotWidth_;
    shot.height = screenshotHeight_;
    shot.pixels.assign(mapped, mapped + static_cast<size_t>(shot.width) * shot.height * 4);
    SDL_UnmapGPUTransferBuffer(device_, screenshotTransfer_);
    const SDL_GPUTextureFormat format = swapchainFormat();
    if(format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
       format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB)
    {
        for(size_t i = 0; i < shot.pixels.size(); i += 4)
        {
            std::swap(shot.pixels[i], shot.pixels[i + 2]);
        }
    }
    screenshot_ = std::move(shot);
}

// --- resources -------------------------------------------------------------------

void Renderer::setTerrainBlendRamp(const image::TextureData& ramp)
{
    const image::TextureData* pointer = &ramp;
    if(GpuTexture texture = std::move(uploadTextures({&pointer, 1}, false)[0]))
    {
        terrainBlendRamp_ = std::move(texture);
    }
}

Mesh Renderer::createMesh(std::span<const ObjectVertex> vertices, std::span<const uint32_t> indices)
{
    return createMesh(vertices.data(), vertices.size_bytes(), indices, VertexLayout::Object);
}

Mesh Renderer::createMesh(std::span<const TerrainVertex> vertices,
                          std::span<const uint32_t> indices)
{
    return createMesh(vertices.data(), vertices.size_bytes(), indices, VertexLayout::Terrain);
}

Mesh Renderer::createMesh(const void* vertices, size_t vertexBytes,
                          std::span<const uint32_t> indices, VertexLayout layout)
{
    Mesh mesh;
    const size_t indexBytes = indices.size_bytes();
    if(vertexBytes == 0 || indexBytes == 0 || vertexBytes + indexBytes > UINT32_MAX)
    {
        return mesh;
    }

    auto makeBuffer = [&](SDL_GPUBufferUsageFlags usage, size_t size)
    {
        SDL_GPUBufferCreateInfo info = {};
        info.usage = usage;
        info.size = static_cast<Uint32>(size);
        return GpuBuffer(device_, SDL_CreateGPUBuffer(device_, &info), static_cast<uint32_t>(size));
    };
    mesh.vertices = makeBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, vertexBytes);
    mesh.indices = makeBuffer(SDL_GPU_BUFFERUSAGE_INDEX, indexBytes);
    if(!mesh.vertices || !mesh.indices)
    {
        log::error("creating mesh buffers failed: {}", SDL_GetError());
        return {};
    }

    SDL_GPUTransferBufferCreateInfo transferInfo = {};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = static_cast<Uint32>(vertexBytes + indexBytes);
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &transferInfo);
    auto* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device_, transfer, false));
    std::memcpy(mapped, vertices, vertexBytes);
    std::memcpy(mapped + vertexBytes, indices.data(), indexBytes);
    SDL_UnmapGPUTransferBuffer(device_, transfer);

    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTransferBufferLocation source = {transfer, 0};
    SDL_GPUBufferRegion destination = {mesh.vertices.get(), 0, static_cast<Uint32>(vertexBytes)};
    SDL_UploadToGPUBuffer(copy, &source, &destination, false);
    source.offset = static_cast<Uint32>(vertexBytes);
    destination = {mesh.indices.get(), 0, static_cast<Uint32>(indexBytes)};
    SDL_UploadToGPUBuffer(copy, &source, &destination, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(commands);
    SDL_ReleaseGPUTransferBuffer(device_, transfer);

    mesh.indexCount = static_cast<uint32_t>(indices.size());
    mesh.layout = layout;
    return mesh;
}

GpuTexture Renderer::uploadTextureArray(std::span<const image::TextureData* const> layers,
                                        bool srgb)
{
    if(layers.empty() || layers[0]->levels.empty())
    {
        return {};
    }
    const image::TextureData& first = *layers[0];
    size_t totalBytes = 0;
    for(const image::TextureData* layer : layers)
    {
        if(layer->levels.size() != first.levels.size() || layer->width() != first.width() ||
           layer->height() != first.height())
        {
            log::error("uploadTextureArray: layers differ in size or mip count");
            return {};
        }
        for(const auto& level : layer->levels)
        {
            totalBytes += level.byteSize();
        }
    }

    SDL_GPUTextureCreateInfo createInfo = {};
    createInfo.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    createInfo.format =
        srgb ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    createInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    createInfo.width = first.width();
    createInfo.height = first.height();
    createInfo.layer_count_or_depth = static_cast<Uint32>(layers.size());
    createInfo.num_levels = static_cast<Uint32>(first.levels.size());
    SDL_GPUTexture* texture = SDL_CreateGPUTexture(device_, &createInfo);
    if(!texture)
    {
        log::error("creating a texture array failed: {}", SDL_GetError());
        return {};
    }

    SDL_GPUTransferBufferCreateInfo transferInfo = {};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = static_cast<Uint32>(totalBytes);
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &transferInfo);
    auto* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device_, transfer, false));
    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    Uint32 offset = 0;
    for(size_t layer = 0; layer < layers.size(); layer++)
    {
        for(size_t level = 0; level < first.levels.size(); level++)
        {
            const image::MipLevel& mip = layers[layer]->levels[level];
            std::memcpy(mapped + offset, mip.pixels.data(), mip.byteSize());
            SDL_GPUTextureTransferInfo source = {};
            source.transfer_buffer = transfer;
            source.offset = offset;
            source.pixels_per_row = mip.width;
            source.rows_per_layer = mip.height;
            SDL_GPUTextureRegion destination = {};
            destination.texture = texture;
            destination.mip_level = static_cast<Uint32>(level);
            destination.layer = static_cast<Uint32>(layer);
            destination.w = mip.width;
            destination.h = mip.height;
            destination.d = 1;
            SDL_UploadToGPUTexture(copy, &source, &destination, false);
            offset += static_cast<Uint32>(mip.byteSize());
        }
    }
    SDL_EndGPUCopyPass(copy);
    SDL_UnmapGPUTransferBuffer(device_, transfer);
    SDL_SubmitGPUCommandBuffer(commands);
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    return GpuTexture(device_, texture, first.width(), first.height(),
                      static_cast<uint32_t>(first.levels.size()));
}

} // namespace viewer::render
