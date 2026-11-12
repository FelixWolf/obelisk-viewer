// Scene rendering: pipelines for the four material families, the HDR + depth
// pass, tonemapping into the swapchain, and screenshots.

#include "RendererPipelines.hpp"
#include "viewer/render/Renderer.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/render/ShaderBytecode.hpp"

#include <SDL3/SDL_error.h>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <tuple>

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
// An object draw's data, std430 (common.glsl DrawData): one storage buffer
// holds every draw's, so drawing needs no uniform pushes per draw.
struct DrawData
{
    glm::mat4 model;
    glm::mat4 normalMatrix;
    glm::vec4 uvRow0;
    glm::vec4 uvRow1;
    glm::vec4 material[3];
    glm::uvec4 skin;
};
static_assert(sizeof(DrawData) == 224);
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
    uint32_t storageBuffers = 0;
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
        createShader(device_, {kObjectVert, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 1}, "object.vert");
    SDL_GPUShader* skinnedVert =
        createShader(device_, {kSkinnedVert, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 2}, "skinned.vert");
    SDL_GPUShader* blinnPhongFrag = createShader(
        device_, {kBlinnphongFrag, SDL_GPU_SHADERSTAGE_FRAGMENT, 3, 1, 1}, "blinnphong.frag");
    SDL_GPUShader* pbrFrag =
        createShader(device_, {kPbrFrag, SDL_GPU_SHADERSTAGE_FRAGMENT, 4, 1, 1}, "pbr.frag");
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
    SDL_GPUShader* skyFrag =
        createShader(device_, {kSkyFrag, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 2}, "sky.frag");

    // Vertex layouts.
    // Object layouts have a second, per-instance buffer: the draw indices
    // 0, 1, 2, ..., so a draw drawn as instance N reads draw data N.
    const SDL_GPUVertexBufferDescription objectBuffers[] = {
        {0, sizeof(ObjectVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0},
        {1, sizeof(uint32_t), SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0},
    };
    const SDL_GPUVertexAttribute objectAttributes[] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(ObjectVertex, position)},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(ObjectVertex, normal)},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(ObjectVertex, tangent)},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(ObjectVertex, uv)},
        {4, 1, SDL_GPU_VERTEXELEMENTFORMAT_UINT, 0},
    };
    const SDL_GPUVertexBufferDescription skinnedBuffers[] = {
        {0, sizeof(SkinnedVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0},
        {1, sizeof(uint32_t), SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0},
    };
    const SDL_GPUVertexAttribute skinnedAttributes[] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(SkinnedVertex, position)},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(SkinnedVertex, normal)},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(SkinnedVertex, tangent)},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(SkinnedVertex, uv)},
        {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4, offsetof(SkinnedVertex, joints)},
        {5, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(SkinnedVertex, weights)},
        {6, 1, SDL_GPU_VERTEXELEMENTFORMAT_UINT, 0},
    };
    const SDL_GPUVertexBufferDescription terrainBuffer = {0, sizeof(TerrainVertex),
                                                          SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
    const SDL_GPUVertexAttribute terrainAttributes[] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(TerrainVertex, position)},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(TerrainVertex, normal)},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(TerrainVertex, blend)},
    };

    auto scenePipeline = [&](SDL_GPUShader* vert, SDL_GPUShader* frag, VertexLayout layout,
                             const char* name, bool blend = false) -> SDL_GPUGraphicsPipeline*
    {
        if(!vert || !frag)
        {
            return nullptr;
        }
        SDL_GPUColorTargetDescription color = {};
        color.format = kHdrFormat;
        if(blend)
        {
            color.blend_state.enable_blend = true;
            color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
            color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        }

        SDL_GPUGraphicsPipelineCreateInfo info = {};
        info.vertex_shader = vert;
        info.fragment_shader = frag;
        info.vertex_input_state.num_vertex_buffers = layout == VertexLayout::Terrain ? 1 : 2;
        switch(layout)
        {
        case VertexLayout::Object:
            info.vertex_input_state.vertex_buffer_descriptions = objectBuffers;
            info.vertex_input_state.vertex_attributes = objectAttributes;
            info.vertex_input_state.num_vertex_attributes =
                static_cast<Uint32>(std::size(objectAttributes));
            break;
        case VertexLayout::Skinned:
            info.vertex_input_state.vertex_buffer_descriptions = skinnedBuffers;
            info.vertex_input_state.vertex_attributes = skinnedAttributes;
            info.vertex_input_state.num_vertex_attributes =
                static_cast<Uint32>(std::size(skinnedAttributes));
            break;
        case VertexLayout::Terrain:
            info.vertex_input_state.vertex_buffer_descriptions = &terrainBuffer;
            info.vertex_input_state.vertex_attributes = terrainAttributes;
            info.vertex_input_state.num_vertex_attributes =
                static_cast<Uint32>(std::size(terrainAttributes));
            break;
        }
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
        info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = !blend;
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

    constexpr auto kObject = VertexLayout::Object;
    constexpr auto kSkinned = VertexLayout::Skinned;
    constexpr auto kTerrain = VertexLayout::Terrain;
    p.blinnPhong = scenePipeline(objectVert, blinnPhongFrag, kObject, "Blinn-Phong");
    p.blinnPhongBlend =
        scenePipeline(objectVert, blinnPhongFrag, kObject, "Blinn-Phong (blended)", true);
    p.pbr = scenePipeline(objectVert, pbrFrag, kObject, "PBR");
    p.blinnPhongSkinned =
        scenePipeline(skinnedVert, blinnPhongFrag, kSkinned, "Blinn-Phong (skinned)");
    p.blinnPhongSkinnedBlend = scenePipeline(skinnedVert, blinnPhongFrag, kSkinned,
                                             "Blinn-Phong (skinned, blended)", true);
    p.pbrSkinned = scenePipeline(skinnedVert, pbrFrag, kSkinned, "PBR (skinned)");
    p.terrainBlinnPhong =
        scenePipeline(terrainVert, terrainBlinnPhongFrag, kTerrain, "terrain Blinn-Phong");
    p.terrainPbr = scenePipeline(terrainVert, terrainPbrFrag, kTerrain, "terrain PBR");

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

    if(fullscreenVert && skyFrag)
    {
        SDL_GPUColorTargetDescription color = {};
        color.format = kHdrFormat;
        SDL_GPUGraphicsPipelineCreateInfo info = {};
        info.vertex_shader = fullscreenVert;
        info.fragment_shader = skyFrag;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1;
        info.target_info.has_depth_stencil_target = true; // in the scene pass, unused
        info.target_info.depth_stencil_format = depthFormat_;
        p.sky = SDL_CreateGPUGraphicsPipeline(device_, &info);
        if(!p.sky)
        {
            log::error("creating the sky pipeline failed: {}", SDL_GetError());
        }
    }

    for(SDL_GPUShader* s :
        {objectVert, skinnedVert, blinnPhongFrag, pbrFrag, terrainVert, terrainBlinnPhongFrag,
         terrainPbrFrag, fullscreenVert, tonemapFrag, skyFrag})
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

    return p.blinnPhong && p.blinnPhongBlend && p.pbr && p.blinnPhongSkinned &&
           p.blinnPhongSkinnedBlend && p.pbrSkinned && p.terrainBlinnPhong && p.terrainPbr &&
           p.tonemap && p.sky && p.repeatSampler && p.clampSampler;
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
    // New meshes' contents first: submitted before this frame's commands.
    meshPool_->flush();

    const Camera& camera = scene.camera;
    const float aspect = static_cast<float>(frame.width) / static_cast<float>(frame.height);
    const glm::mat4 view = glm::lookAt(camera.position, camera.target, camera.up);
    const glm::mat4 projection =
        glm::perspective(camera.verticalFov, aspect, camera.nearPlane, camera.farPlane);

    const FrameVS frameVs{projection * view, glm::vec4(camera.position, 1.0f)};
    const Lighting& light = scene.lighting;
    const Atmosphere& atmo = light.atmosphere;
    const FrameFS frameFs{
        glm::vec4(camera.position, 1.0f),
        glm::vec4(glm::normalize(light.sunDirection), 0.0f),
        glm::vec4(light.sunColor, 1.0f),
        glm::vec4(light.skyAmbient, 1.0f),
        glm::vec4(light.groundAmbient, 1.0f),
        glm::vec4(atmo.lightDirection, atmo.enabled ? 1.0f : 0.0f),
        glm::vec4(atmo.sunlight, atmo.sunUp ? 1.0f : 0.0f),
        glm::vec4(atmo.ambient, atmo.cloudShadow),
        glm::vec4(atmo.blueHorizon, atmo.hazeHorizon),
        glm::vec4(atmo.blueDensity, atmo.hazeDensity),
        glm::vec4(atmo.densityMultiplier, atmo.distanceMultiplier, atmo.maxY, atmo.sunMoonGlow),
        glm::vec4(atmo.glow, atmo.starBrightness),
        glm::vec4(atmo.sunDirection, atmo.sunRadius),
        glm::vec4(atmo.moonDirection, atmo.moonRadius),
        glm::vec4(atmo.moonBrightness, 0.0f, 0.0f, 0.0f),
        glm::vec4(atmo.cloudColor, atmo.cloudScale),
        glm::vec4(atmo.cloudPosDensity1, atmo.cloudVariance),
        glm::vec4(atmo.cloudPosDensity2, atmo.clouds && *atmo.clouds ? 1.0f : 0.0f),
        glm::vec4(atmo.domeRadius, atmo.domeOffset, 0.0f, 0.0f),
    };

    // Draw data and joint matrices go up before the render pass (copy passes
    // can't nest).
    const bool drawsReady = uploadDraws(commands, scene);

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

    // The sky first, behind everything (no depth).
    if(atmo.enabled)
    {
        SDL_BindGPUGraphicsPipeline(pass, pipelines_->sky);
        const SDL_GPUTextureSamplerBinding clouds = {
            atmo.clouds && *atmo.clouds ? atmo.clouds->get() : white_.get(),
            pipelines_->repeatSampler};
        SDL_BindGPUFragmentSamplers(pass, 0, &clouds, 1);
        pushFragmentUniforms(commands, 0, frameFs);
        // Rotation only: the shader wants directions, which don't need the
        // camera's position (leaving it out keeps them precise).
        pushFragmentUniforms(commands, 1,
                             SkyFS{glm::inverse(projection * glm::mat4(glm::mat3(view)))});
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    }

    auto texture = [](const GpuTexture* t, const GpuTexture& fallback)
    { return t && *t ? t->get() : fallback.get(); };
    // State is only set when it changes: meshes share buffers (MeshPool) and
    // draws are sorted so that neighbours share pipelines and textures.
    SDL_GPUBuffer* boundVertices = nullptr;
    SDL_GPUBuffer* boundIndices = nullptr;
    SDL_GPUGraphicsPipeline* boundPipeline = nullptr;
    std::array<SDL_GPUTextureSamplerBinding, 5> boundSamplers{};
    size_t boundSamplerCount = 0;
    auto drawMesh = [&](const Mesh& mesh, uint32_t instance = 0)
    {
        if(mesh.vertexBuffer() != boundVertices)
        {
            boundVertices = mesh.vertexBuffer();
            const SDL_GPUBufferBinding binding = {boundVertices, 0};
            SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
        }
        if(mesh.indexBuffer() != boundIndices)
        {
            boundIndices = mesh.indexBuffer();
            const SDL_GPUBufferBinding binding = {boundIndices, 0};
            SDL_BindGPUIndexBuffer(pass, &binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        }
        SDL_DrawGPUIndexedPrimitives(pass, mesh.indexCount, 1, mesh.firstIndex,
                                     static_cast<Sint32>(mesh.firstVertex), instance);
    };
    auto bindPipeline = [&](SDL_GPUGraphicsPipeline* pipeline)
    {
        if(pipeline == boundPipeline)
        {
            return;
        }
        boundPipeline = pipeline;
        SDL_BindGPUGraphicsPipeline(pass, pipeline);
        pushVertexUniforms(commands, 0, frameVs);
        pushFragmentUniforms(commands, 0, frameFs);
    };
    auto bindSamplers = [&](std::span<const SDL_GPUTextureSamplerBinding> samplers)
    {
        if(samplers.size() == boundSamplerCount &&
           std::equal(samplers.begin(), samplers.end(), boundSamplers.begin(),
                      [](const auto& a, const auto& b)
                      { return a.texture == b.texture && a.sampler == b.sampler; }))
        {
            return;
        }
        std::ranges::copy(samplers, boundSamplers.begin());
        boundSamplerCount = samplers.size();
        SDL_BindGPUFragmentSamplers(pass, 0, samplers.data(), static_cast<Uint32>(samplers.size()));
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
            bindSamplers(samplers);
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
            bindSamplers(samplers);
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
        drawMesh(*draw.mesh);
    }

    // Opaque objects, then blended ones from far to near.
    std::vector<const ObjectDraw*> opaque;
    std::vector<std::pair<float, const ObjectDraw*>> blended;
    for(const ObjectDraw& draw : scene.objects)
    {
        if(!drawsReady || !draw.mesh || !*draw.mesh || draw.mesh->layout == VertexLayout::Terrain)
        {
            continue;
        }
        if(draw.mesh->layout == VertexLayout::Skinned && draw.joints.empty())
        {
            continue;
        }
        const auto* bp = std::get_if<BlinnPhongMaterial>(&draw.material);
        if(bp && bp->blend)
        {
            const glm::vec3 center(draw.transform[3]);
            const glm::vec3 d = center - camera.position;
            blended.emplace_back(glm::dot(d, d), &draw);
        }
        else
        {
            opaque.push_back(&draw);
        }
    }
    // Opaque draws need no order: group them by pipeline, then texture, then
    // buffer block, so state changes between neighbours are rare. One
    // packed key: layout (2 bits), material kind (2), texture (44: its
    // address, which only needs to tell textures apart), block (16).
    auto stateKey = [](const ObjectDraw* draw)
    {
        const void* firstTexture = nullptr;
        if(const auto* m = std::get_if<BlinnPhongMaterial>(&draw->material))
        {
            firstTexture = m->diffuse;
        }
        else if(const auto* t = std::get_if<PbrMaterial>(&draw->material))
        {
            firstTexture = t->albedo;
        }
        const uint64_t textureBits =
            (reinterpret_cast<uintptr_t>(firstTexture) >> 4) & ((1ull << 44) - 1);
        return static_cast<uint64_t>(draw->mesh->layout) << 62 |
               static_cast<uint64_t>(draw->material.index()) << 60 | textureBits << 16 |
               (draw->mesh->block->index & 0xFFFFu);
    };
    keyedDraws_.clear();
    for(const ObjectDraw* draw : opaque)
    {
        keyedDraws_.emplace_back(stateKey(draw), draw);
    }
    std::ranges::sort(keyedDraws_, {}, &std::pair<uint64_t, const ObjectDraw*>::first);
    for(size_t i = 0; i < keyedDraws_.size(); i++)
    {
        opaque[i] = keyedDraws_[i].second;
    }
    std::ranges::sort(blended, [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<const ObjectDraw*> order = std::move(opaque);
    for(const auto& [distance, draw] : blended)
    {
        order.push_back(draw);
    }

    // Every object pipeline reads the same storage buffers and instance
    // buffer; bound again with each pipeline, to be safe across backends.
    SDL_GPUGraphicsPipeline* objectResourcesFor = nullptr;
    for(const ObjectDraw* drawPointer : order)
    {
        const ObjectDraw& draw = *drawPointer;
        const bool skinned = draw.mesh->layout == VertexLayout::Skinned;
        if(const auto* m = std::get_if<BlinnPhongMaterial>(&draw.material))
        {
            bindPipeline(skinned ? (m->blend ? p.blinnPhongSkinnedBlend : p.blinnPhongSkinned)
                                 : (m->blend ? p.blinnPhongBlend : p.blinnPhong));
            const SDL_GPUTextureSamplerBinding samplers[] = {
                {texture(m->diffuse, white_), p.repeatSampler},
                {texture(m->normal, flatNormal_), p.repeatSampler},
                {texture(m->specular, white_), p.repeatSampler},
            };
            bindSamplers(samplers);
        }
        else if(const auto* t = std::get_if<PbrMaterial>(&draw.material))
        {
            bindPipeline(skinned ? p.pbrSkinned : p.pbr);
            const SDL_GPUTextureSamplerBinding samplers[] = {
                {texture(t->albedo, white_), p.repeatSampler},
                {texture(t->orm, white_), p.repeatSampler},
                {texture(t->emissive, black_), p.repeatSampler},
                {texture(t->normal, flatNormal_), p.repeatSampler},
            };
            bindSamplers(samplers);
        }
        if(objectResourcesFor != boundPipeline)
        {
            objectResourcesFor = boundPipeline;
            // Without skinned draws there's no joint buffer: any buffer
            // fills the slot.
            SDL_GPUBuffer* vertexStorage[] = {drawBuffer_.buffer.get(),
                                              jointBuffer_.buffer ? jointBuffer_.buffer.get()
                                                                  : drawBuffer_.buffer.get()};
            SDL_BindGPUVertexStorageBuffers(pass, 0, vertexStorage, 2);
            SDL_GPUBuffer* fragmentStorage = drawBuffer_.buffer.get();
            SDL_BindGPUFragmentStorageBuffers(pass, 0, &fragmentStorage, 1);
            const SDL_GPUBufferBinding instances = {instanceBuffer_.buffer.get(), 0};
            SDL_BindGPUVertexBuffers(pass, 1, &instances, 1);
        }
        drawMesh(*draw.mesh, static_cast<uint32_t>(drawPointer - scene.objects.data()));
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

Mesh Renderer::createMesh(std::span<const SkinnedVertex> vertices,
                          std::span<const uint32_t> indices)
{
    return createMesh(vertices.data(), vertices.size_bytes(), indices, VertexLayout::Skinned);
}

bool Renderer::reserve(FrameBuffer& b, uint32_t bytes, SDL_GPUBufferUsageFlags usage)
{
    if(b.buffer && b.buffer.size() >= bytes)
    {
        return true;
    }
    // Grow to the next power of two, so it settles quickly.
    uint32_t size = 64u << 10;
    while(size < bytes)
    {
        size *= 2;
    }
    SDL_GPUBufferCreateInfo info = {};
    info.usage = usage;
    info.size = size;
    b.buffer = GpuBuffer(device_, SDL_CreateGPUBuffer(device_, &info), size);
    if(b.transfer)
    {
        SDL_ReleaseGPUTransferBuffer(device_, b.transfer);
    }
    SDL_GPUTransferBufferCreateInfo transferInfo = {};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = size;
    b.transfer = SDL_CreateGPUTransferBuffer(device_, &transferInfo);
    if(!b.buffer || !b.transfer)
    {
        log::error("creating a {} byte frame buffer failed: {}", size, SDL_GetError());
        b.buffer.reset();
        return false;
    }
    return true;
}

bool Renderer::uploadDraws(SDL_GPUCommandBuffer* commands, const Scene& scene)
{
    const size_t count = scene.objects.size();
    if(count == 0)
    {
        return false;
    }
    size_t jointCount = 0;
    for(const ObjectDraw& draw : scene.objects)
    {
        jointCount += draw.joints.size();
    }
    const auto drawBytes = static_cast<uint32_t>(count * sizeof(DrawData));
    const auto jointBytes = static_cast<uint32_t>(jointCount * sizeof(glm::mat4));
    const auto instanceBytes = static_cast<uint32_t>(count * sizeof(uint32_t));
    const bool newInstances =
        !instanceBuffer_.buffer || instanceBuffer_.buffer.size() < instanceBytes;
    if(!reserve(drawBuffer_, drawBytes, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ) ||
       (jointBytes > 0 &&
        !reserve(jointBuffer_, jointBytes, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ)) ||
       !reserve(instanceBuffer_, instanceBytes, SDL_GPU_BUFFERUSAGE_VERTEX))
    {
        return false;
    }

    // Cycled: the previous frame may still be reading the old contents.
    auto* draws =
        static_cast<DrawData*>(SDL_MapGPUTransferBuffer(device_, drawBuffer_.transfer, true));
    uint32_t jointBase = 0;
    for(size_t i = 0; i < count; i++)
    {
        const ObjectDraw& draw = scene.objects[i];
        DrawData& out = draws[i];
        out.model = draw.transform;
        // Only its upper 3x3 is used: cheaper than inverting the 4x4.
        out.normalMatrix = glm::mat4(glm::inverseTranspose(glm::mat3(draw.transform)));
        out.uvRow0 = glm::vec4(draw.uvRow0, 0.0f);
        out.uvRow1 = glm::vec4(draw.uvRow1, 0.0f);
        if(const auto* m = std::get_if<BlinnPhongMaterial>(&draw.material))
        {
            out.material[0] = m->diffuseTint;
            out.material[1] = glm::vec4(m->specularTint, m->maxExponent);
            out.material[2] = glm::vec4(m->alphaCutoff, m->fullbright ? 1.0f : 0.0f, 0.0f, 0.0f);
        }
        else if(const auto* t = std::get_if<PbrMaterial>(&draw.material))
        {
            out.material[0] = t->baseColorFactor;
            out.material[1] = glm::vec4(t->emissiveTint, t->emissiveStrength);
            out.material[2] =
                glm::vec4(t->roughness, t->metallic, t->occlusionStrength, t->alphaCutoff);
        }
        out.skin = glm::uvec4(jointBase, 0, 0, 0);
        jointBase += static_cast<uint32_t>(draw.joints.size());
    }
    SDL_UnmapGPUTransferBuffer(device_, drawBuffer_.transfer);

    if(jointBytes > 0)
    {
        auto* joints =
            static_cast<glm::mat4*>(SDL_MapGPUTransferBuffer(device_, jointBuffer_.transfer, true));
        for(const ObjectDraw& draw : scene.objects)
        {
            std::memcpy(joints, draw.joints.data(), draw.joints.size_bytes());
            joints += draw.joints.size();
        }
        SDL_UnmapGPUTransferBuffer(device_, jointBuffer_.transfer);
    }

    if(newInstances)
    {
        // 0, 1, 2, ...: written once per size.
        auto* ids = static_cast<uint32_t*>(
            SDL_MapGPUTransferBuffer(device_, instanceBuffer_.transfer, false));
        const uint32_t capacity = instanceBuffer_.buffer.size() / sizeof(uint32_t);
        for(uint32_t i = 0; i < capacity; i++)
        {
            ids[i] = i;
        }
        SDL_UnmapGPUTransferBuffer(device_, instanceBuffer_.transfer);
    }

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    auto upload = [&](const FrameBuffer& b, uint32_t bytes, bool cycle)
    {
        const SDL_GPUTransferBufferLocation source = {b.transfer, 0};
        const SDL_GPUBufferRegion destination = {b.buffer.get(), 0, bytes};
        SDL_UploadToGPUBuffer(copy, &source, &destination, cycle);
    };
    upload(drawBuffer_, drawBytes, true);
    if(jointBytes > 0)
    {
        upload(jointBuffer_, jointBytes, true);
    }
    if(newInstances)
    {
        upload(instanceBuffer_, instanceBuffer_.buffer.size(), false);
    }
    SDL_EndGPUCopyPass(copy);
    return true;
}

Mesh Renderer::createMesh(std::span<const TerrainVertex> vertices,
                          std::span<const uint32_t> indices)
{
    return createMesh(vertices.data(), vertices.size_bytes(), indices, VertexLayout::Terrain);
}

Mesh Renderer::createMesh(const void* vertices, size_t vertexBytes,
                          std::span<const uint32_t> indices, VertexLayout layout)
{
    const size_t stride = vertexStride(layout);
    if(vertexBytes == 0 || indices.empty() || vertexBytes / stride > UINT32_MAX ||
       indices.size() > UINT32_MAX)
    {
        return {};
    }
    return meshPool_->create(layout, vertices, static_cast<uint32_t>(vertexBytes / stride),
                             indices);
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
