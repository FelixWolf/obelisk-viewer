// Scene rendering: pipelines for the four material families, the HDR + depth
// pass, tonemapping into the swapchain, and screenshots.

#include "RendererPasses.hpp"
#include "RendererPipelines.hpp"
#include "viewer/render/Renderer.hpp"
#include "viewer/render/ShadowCascades.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/render/ShaderBytecode.hpp"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_timer.h>

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

// Dynamic exposure's luminance and exposure maps.
constexpr SDL_GPUTextureFormat kExposureFormat = SDL_GPU_TEXTUREFORMAT_R16_FLOAT;

// Data for the shaders (RendererPasses.hpp has the blocks they share); must
// match them.
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
    glm::vec4 mapUv[4]; // normal map's rows, then the specular map's
};
static_assert(sizeof(DrawData) == 288);
// Material family and anything else that picks the pipeline (2 bits).
uint64_t materialKind(const ObjectDraw& draw)
{
    if(const auto* t = std::get_if<PbrMaterial>(&draw.material))
    {
        return t->doubleSided ? 3 : 1;
    }
    return 0;
}

// One local light, std430 (lights.glsl LocalLight).
struct LightData
{
    glm::vec4 positionRange;
    glm::vec4 colorFalloff;
    glm::mat4 projector;
    glm::vec4 originLayer;
    glm::vec4 axisFocus;
    glm::vec4 projectorParams;
};
struct TonemapFS
{
    glm::vec4 settings;
};
struct ExposureFS
{
    glm::vec4 params;  // luminance taken as bright, least and most exposure, speed error
    glm::vec4 params2; // seconds since last frame, 1 if last frame's is valid, speed target
};

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
        device_, {kBlinnphongFrag, SDL_GPU_SHADERSTAGE_FRAGMENT, 8, 1, 3}, "blinnphong.frag");
    SDL_GPUShader* pbrFrag =
        createShader(device_, {kPbrFrag, SDL_GPU_SHADERSTAGE_FRAGMENT, 9, 1, 3}, "pbr.frag");
    SDL_GPUShader* terrainVert =
        createShader(device_, {kTerrainVert, SDL_GPU_SHADERSTAGE_VERTEX, 0, 2}, "terrain.vert");
    SDL_GPUShader* terrainBlinnPhongFrag =
        createShader(device_, {kTerrainBlinnphongFrag, SDL_GPU_SHADERSTAGE_FRAGMENT, 7, 1, 2},
                     "terrain_blinnphong.frag");
    SDL_GPUShader* terrainPbrFrag = createShader(
        device_, {kTerrainPbrFrag, SDL_GPU_SHADERSTAGE_FRAGMENT, 10, 2, 2}, "terrain_pbr.frag");
    SDL_GPUShader* fullscreenVert = createShader(
        device_, {kFullscreenVert, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0}, "fullscreen.vert");
    SDL_GPUShader* tonemapFrag =
        createShader(device_, {kTonemapFrag, SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1}, "tonemap.frag");
    SDL_GPUShader* luminanceFrag = createShader(
        device_, {kLuminanceFrag, SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 0}, "luminance.frag");
    SDL_GPUShader* exposureFrag =
        createShader(device_, {kExposureFrag, SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1}, "exposure.frag");
    SDL_GPUShader* shadowFrag =
        createShader(device_, {kShadowFrag, SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0}, "shadow.frag");
    SDL_GPUShader* shadowMaskedFrag = createShader(
        device_, {kShadowMaskedFrag, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0, 1}, "shadow_masked.frag");
    SDL_GPUShader* skyFrag =
        createShader(device_, {kSkyFrag, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 2}, "sky.frag");
    SDL_GPUShader* probeRadianceFrag = createShader(
        device_, {kProbeRadianceFrag, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1}, "probe_radiance.frag");
    SDL_GPUShader* probeIrradianceFrag =
        createShader(device_, {kProbeIrradianceFrag, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1},
                     "probe_irradiance.frag");
    SDL_GPUShader* outlineMaskFrag = createShader(
        device_, {kOutlineMaskFrag, SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0}, "outline_mask.frag");
    SDL_GPUShader* outlineFrag =
        createShader(device_, {kOutlineFrag, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1}, "outline.frag");
    SDL_GPUShader* labelVert =
        createShader(device_, {kLabelVert, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0}, "label.vert");
    SDL_GPUShader* labelFrag =
        createShader(device_, {kLabelFrag, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0}, "label.frag");
    SDL_GPUShader* pickFrag =
        createShader(device_, {kPickFrag, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0, 1}, "pick.frag");
    SDL_GPUShader* hudLegacyFrag = createShader(
        device_, {kHudLegacyFrag, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0, 1}, "hud_legacy.frag");
    SDL_GPUShader* hudPbrFrag =
        createShader(device_, {kHudPbrFrag, SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 0, 1}, "hud_pbr.frag");
    SDL_GPUShader* hudCompositeFrag = createShader(
        device_, {kHudCompositeFrag, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0}, "hud_composite.frag");
    SDL_GPUShader* hudPanelVert =
        createShader(device_, {kHudPanelVert, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1}, "hud_panel.vert");
    SDL_GPUShader* hudPanelFrag = createShader(
        device_, {kHudPanelFrag, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0}, "hud_panel.frag");
    SDL_GPUShader* brdfTableFrag = createShader(
        device_, {kBrdfTableFrag, SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0}, "brdf_table.frag");

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
                             const char* name, bool blend = false, bool bothSides = false,
                             SDL_GPUTextureFormat format = kHdrFormat) -> SDL_GPUGraphicsPipeline*
    {
        if(!vert || !frag)
        {
            return nullptr;
        }
        SDL_GPUColorTargetDescription color = {};
        color.format = format;
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
        info.rasterizer_state.cull_mode = bothSides ? SDL_GPU_CULLMODE_NONE : SDL_GPU_CULLMODE_BACK;
        info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        info.depth_stencil_state.enable_depth_test = true;
        // Blended rigged mesh writes depth, as the official viewer's: it's
        // drawn in attachment order, and later layers must stay behind
        // earlier ones on the same body.
        info.depth_stencil_state.enable_depth_write = !blend || layout == VertexLayout::Skinned;
        // Less or equal, as the official viewer: layers made of the same
        // geometry (a plate over a body) show the later one, not a mix.
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
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
    for(int blend = 0; blend < 2; blend++)
    {
        for(int sides = 0; sides < 2; sides++)
        {
            for(int skinned = 0; skinned < 2; skinned++)
            {
                SDL_GPUGraphicsPipeline*& v = p.pbrVariants[blend][sides][skinned];
                if(!blend && !sides)
                {
                    v = skinned ? p.pbrSkinned : p.pbr;
                    continue;
                }
                v = scenePipeline(skinned ? skinnedVert : objectVert, pbrFrag,
                                  skinned ? kSkinned : kObject, "PBR (variant)", blend != 0,
                                  sides != 0);
            }
        }
    }
    p.terrainBlinnPhong =
        scenePipeline(terrainVert, terrainBlinnPhongFrag, kTerrain, "terrain Blinn-Phong");
    p.terrainPbr = scenePipeline(terrainVert, terrainPbrFrag, kTerrain, "terrain PBR");
    // HUD attachments: opaque [0] and blended [1], into the HUD image.
    for(int blend = 0; blend < 2; blend++)
    {
        p.hudLegacy[blend] = scenePipeline(objectVert, hudLegacyFrag, kObject, "HUD (legacy)",
                                           blend != 0, false, kHudFormat);
        p.hudPbr[blend] = scenePipeline(objectVert, hudPbrFrag, kObject, "HUD (PBR)", blend != 0,
                                        false, kHudFormat);
    }

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
    // Dynamic exposure's passes: one value per pixel, half floats.
    auto exposurePipeline = [&](SDL_GPUShader* frag, const char* name) -> SDL_GPUGraphicsPipeline*
    {
        if(!fullscreenVert || !frag)
        {
            return nullptr;
        }
        SDL_GPUColorTargetDescription color = {};
        color.format = kExposureFormat;
        SDL_GPUGraphicsPipelineCreateInfo info = {};
        info.vertex_shader = fullscreenVert;
        info.fragment_shader = frag;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1;
        SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device_, &info);
        if(!pipeline)
        {
            log::error("creating the {} pipeline failed: {}", name, SDL_GetError());
        }
        return pipeline;
    };
    p.luminance = exposurePipeline(luminanceFrag, "luminance");
    p.exposure = exposurePipeline(exposureFrag, "exposure");
    // Reflection probes: full-screen passes into a cube face (HDR), and the
    // split-sum table (two half floats).
    auto fullscreenPipeline = [&](SDL_GPUShader* frag, SDL_GPUTextureFormat format,
                                  const char* name) -> SDL_GPUGraphicsPipeline*
    {
        if(!fullscreenVert || !frag)
        {
            return nullptr;
        }
        SDL_GPUColorTargetDescription color = {};
        color.format = format;
        SDL_GPUGraphicsPipelineCreateInfo info = {};
        info.vertex_shader = fullscreenVert;
        info.fragment_shader = frag;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1;
        SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device_, &info);
        if(!pipeline)
        {
            log::error("creating the {} pipeline failed: {}", name, SDL_GetError());
        }
        return pipeline;
    };
    p.probeRadiance = fullscreenPipeline(probeRadianceFrag, kHdrFormat, "probe radiance");
    p.probeIrradiance = fullscreenPipeline(probeIrradianceFrag, kHdrFormat, "probe irradiance");
    p.brdfTable =
        fullscreenPipeline(brdfTableFrag, SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT, "BRDF table");

    // The HUD image, premultiplied: over the view (the swapchain) and on a
    // panel in the scene (HDR, depth tested, both sides).
    SDL_GPUColorTargetDescription premultiplied = {};
    premultiplied.blend_state.enable_blend = true;
    premultiplied.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    premultiplied.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    premultiplied.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    premultiplied.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    premultiplied.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    premultiplied.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    if(fullscreenVert && hudCompositeFrag)
    {
        SDL_GPUColorTargetDescription color = premultiplied;
        color.format = swapchainFormat();
        SDL_GPUGraphicsPipelineCreateInfo info = {};
        info.vertex_shader = fullscreenVert;
        info.fragment_shader = hudCompositeFrag;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1;
        p.hudComposite = SDL_CreateGPUGraphicsPipeline(device_, &info);
    }
    if(hudPanelVert && hudPanelFrag)
    {
        SDL_GPUColorTargetDescription color = premultiplied;
        color.format = kHdrFormat;
        SDL_GPUGraphicsPipelineCreateInfo info = {};
        info.vertex_shader = hudPanelVert;
        info.fragment_shader = hudPanelFrag;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = false;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1;
        info.target_info.has_depth_stencil_target = true;
        info.target_info.depth_stencil_format = depthFormat_;
        p.hudPanel = SDL_CreateGPUGraphicsPipeline(device_, &info);
    }
    // Picking: four targets (pick id, then position, normal and binormal
    // with the texture coordinates in their w), both sides of faces.
    if(objectVert && pickFrag)
    {
        SDL_GPUColorTargetDescription colors[4] = {};
        colors[0].format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_UINT;
        for(int i = 1; i < 4; i++)
        {
            colors[i].format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
        }
        SDL_GPUGraphicsPipelineCreateInfo info = {};
        info.vertex_shader = objectVert;
        info.fragment_shader = pickFrag;
        info.vertex_input_state.num_vertex_buffers = 2;
        info.vertex_input_state.vertex_buffer_descriptions = objectBuffers;
        info.vertex_input_state.vertex_attributes = objectAttributes;
        info.vertex_input_state.num_vertex_attributes =
            static_cast<Uint32>(std::size(objectAttributes));
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = true;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        info.target_info.color_target_descriptions = colors;
        info.target_info.num_color_targets = 4;
        info.target_info.has_depth_stencil_target = true;
        info.target_info.depth_stencil_format = depthFormat_;
        p.pick = SDL_CreateGPUGraphicsPipeline(device_, &info);
    }
    // The outline: the prim's mask (depth tested against the view, both
    // sides), then the outline over the view (blended).
    if(objectVert && outlineMaskFrag)
    {
        SDL_GPUColorTargetDescription color = {};
        color.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
        SDL_GPUGraphicsPipelineCreateInfo info = {};
        info.vertex_shader = objectVert;
        info.fragment_shader = outlineMaskFrag;
        info.vertex_input_state.num_vertex_buffers = 2;
        info.vertex_input_state.vertex_buffer_descriptions = objectBuffers;
        info.vertex_input_state.vertex_attributes = objectAttributes;
        info.vertex_input_state.num_vertex_attributes =
            static_cast<Uint32>(std::size(objectAttributes));
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = false;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1;
        info.target_info.has_depth_stencil_target = true;
        info.target_info.depth_stencil_format = depthFormat_;
        p.outlineMask = SDL_CreateGPUGraphicsPipeline(device_, &info);
    }
    // Labels: glyph quads (clip space position and depth, atlas coordinates,
    // color), premultiplied.
    if(labelVert && labelFrag)
    {
        const SDL_GPUVertexBufferDescription buffer = {0, 24, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
        const SDL_GPUVertexAttribute attributes[] = {
            {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0},
            {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 12},
            {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, 20},
        };
        SDL_GPUGraphicsPipelineCreateInfo info = {};
        info.vertex_shader = labelVert;
        info.fragment_shader = labelFrag;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_buffer_descriptions = &buffer;
        info.vertex_input_state.vertex_attributes = attributes;
        info.vertex_input_state.num_vertex_attributes = static_cast<Uint32>(std::size(attributes));
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.target_info.num_color_targets = 1;

        SDL_GPUColorTargetDescription color = premultiplied;
        color.format = swapchainFormat();
        info.target_info.color_target_descriptions = &color;
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = false;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        info.target_info.has_depth_stencil_target = true;
        info.target_info.depth_stencil_format = depthFormat_;
        p.label = SDL_CreateGPUGraphicsPipeline(device_, &info);

        SDL_GPUColorTargetDescription hudColor = premultiplied;
        hudColor.format = kHudFormat;
        info.target_info.color_target_descriptions = &hudColor;
        info.depth_stencil_state = {};
        info.target_info.has_depth_stencil_target = false;
        info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_INVALID;
        p.hudLabel = SDL_CreateGPUGraphicsPipeline(device_, &info);
    }
    if(fullscreenVert && outlineFrag)
    {
        SDL_GPUColorTargetDescription color = {};
        color.format = swapchainFormat();
        color.blend_state.enable_blend = true;
        color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        SDL_GPUGraphicsPipelineCreateInfo info = {};
        info.vertex_shader = fullscreenVert;
        info.fragment_shader = outlineFrag;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1;
        p.outline = SDL_CreateGPUGraphicsPipeline(device_, &info);
    }
    if(!p.hudComposite || !p.hudPanel || !p.pick || !p.outlineMask || !p.outline || !p.label ||
       !p.hudLabel)
    {
        log::error("creating the HUD pipelines failed: {}", SDL_GetError());
    }

    // Shadow maps: depth only, both sides of every face (prims are often
    // open, and thin), pushed back a little against self-shadowing.
    auto shadowPipeline = [&](SDL_GPUShader* vert, SDL_GPUShader* frag, VertexLayout layout,
                              const char* name) -> SDL_GPUGraphicsPipeline*
    {
        if(!vert || !frag)
        {
            return nullptr;
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
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.rasterizer_state.enable_depth_bias = true;
        info.rasterizer_state.depth_bias_constant_factor = 1.0f;
        info.rasterizer_state.depth_bias_slope_factor = 2.0f;
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = true;
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        info.target_info.num_color_targets = 0;
        info.target_info.has_depth_stencil_target = true;
        info.target_info.depth_stencil_format = shadowFormat_;
        SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device_, &info);
        if(!pipeline)
        {
            log::error("creating the {} pipeline failed: {}", name, SDL_GetError());
        }
        return pipeline;
    };
    shadowFormat_ = SDL_GPUTextureSupportsFormat(
                        device_, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTURETYPE_2D_ARRAY,
                        SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER)
                        ? SDL_GPU_TEXTUREFORMAT_D32_FLOAT
                        : SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    p.shadowObject = shadowPipeline(objectVert, shadowFrag, kObject, "shadow");
    p.shadowObjectMasked = shadowPipeline(objectVert, shadowMaskedFrag, kObject, "shadow (masked)");
    p.shadowSkinned = shadowPipeline(skinnedVert, shadowFrag, kSkinned, "shadow (skinned)");
    p.shadowSkinnedMasked =
        shadowPipeline(skinnedVert, shadowMaskedFrag, kSkinned, "shadow (skinned, masked)");
    p.shadowTerrain = shadowPipeline(terrainVert, shadowFrag, kTerrain, "shadow (terrain)");

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

    for(SDL_GPUShader* s : {objectVert,        skinnedVert,
                            blinnPhongFrag,    pbrFrag,
                            terrainVert,       terrainBlinnPhongFrag,
                            terrainPbrFrag,    fullscreenVert,
                            tonemapFrag,       luminanceFrag,
                            exposureFrag,      skyFrag,
                            shadowFrag,        shadowMaskedFrag,
                            probeRadianceFrag, probeIrradianceFrag,
                            brdfTableFrag,     hudLegacyFrag,
                            hudPbrFrag,        hudCompositeFrag,
                            hudPanelVert,      hudPanelFrag,
                            pickFrag,          outlineMaskFrag,
                            outlineFrag,       labelVert,
                            labelFrag})
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
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler.max_lod = 0.0f;
    sampler.enable_compare = true;
    sampler.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL; // 1 where lit
    p.shadowSampler = SDL_CreateGPUSampler(device_, &sampler);

    return p.blinnPhong && p.blinnPhongBlend && p.pbr && p.blinnPhongSkinned &&
           p.blinnPhongSkinnedBlend && p.pbrSkinned && p.terrainBlinnPhong && p.terrainPbr &&
           p.tonemap && p.luminance && p.exposure && p.sky && p.probeRadiance &&
           p.probeIrradiance && p.brdfTable && p.hudLegacy[0] && p.hudLegacy[1] && p.hudPbr[0] &&
           p.hudPbr[1] && p.hudComposite && p.hudPanel && p.pick && p.outlineMask && p.outline &&
           p.label && p.hudLabel && p.shadowObject && p.shadowObjectMasked && p.shadowSkinned &&
           p.shadowSkinnedMasked && p.shadowTerrain && p.repeatSampler && p.clampSampler &&
           p.shadowSampler;
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
    const image::TextureData gray = solid(0x7f, 0x7f, 0x7f, 255);
    const image::TextureData* grayPointer = &gray;
    loading_ = std::move(uploadTextures({&grayPointer, 1}, true)[0]); // sRGB, as the textures

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
    // The shadow map: one layer per cascade.
    SDL_GPUTextureCreateInfo shadow = {};
    shadow.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    shadow.format = shadowFormat_;
    shadow.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    shadow.width = kShadowMapSize;
    shadow.height = kShadowMapSize;
    shadow.layer_count_or_depth = kShadowCascades;
    shadow.num_levels = 1;
    shadowMap_ = SDL_CreateGPUTexture(device_, &shadow);
    if(!shadowMap_)
    {
        log::error("creating the shadow map failed: {}", SDL_GetError());
    }

    SDL_GPUTextureCreateInfo projectors = {};
    projectors.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    projectors.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
    // Blits write it, which takes a color target.
    projectors.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    projectors.width = kProjectorSize;
    projectors.height = kProjectorSize;
    projectors.layer_count_or_depth = kProjectorLayers;
    projectors.num_levels = kProjectorLevels;
    projectors_ = SDL_CreateGPUTexture(device_, &projectors);
    if(!projectors_)
    {
        log::error("creating the projector textures failed: {}", SDL_GetError());
    }

    return white_ && black_ && flatNormal_ && loading_ && whiteLayers_ && blackLayers_ &&
           flatNormalLayers_ && shadowMap_ && terrainBlendRamp_ && projectors_;
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
    // Sampled afterwards by dynamic exposure (sky or not).
    info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
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
    const bool shadows = scene.shadows.enabled && shadowMap_;
    const Cascades cascades =
        shadows ? computeCascades(camera, aspect, scene.shadows, kShadowMapSize) : Cascades{};
    FrameFS frameFs{
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
        {cascades.matrices[0], cascades.matrices[1], cascades.matrices[2], cascades.matrices[3]},
        cascades.splits,
        cascades.texels,
        glm::vec4(shadows ? 1.0f : 0.0f, 1.0f / static_cast<float>(kShadowMapSize), 0.0f, 0.0f),
        glm::vec4(glm::normalize(camera.target - camera.position), 0.0f),
        glm::vec4(0.0f), // local lights: below
        glm::vec4(0.0f), // probes: below
    };

    // Draw data and joint matrices go up before the render pass (copy passes
    // can't nest).
    const bool drawsReady = uploadDraws(commands, scene);
    lightsUsed_ = uploadLights(commands, scene);
    frameFs.lightInfo.x = static_cast<float>(lightsUsed_);
    if(shadows)
    {
        drawShadows(commands, scene, cascades, drawsReady);
    }
    // Reflection probes: the ones surfaces use (into frameFs), and the next
    // face of whichever is being captured.
    updateProbes(commands, scene, frameFs, drawsReady);
    drawPick(commands, scene, drawsReady);
    uploadLabels(commands, scene, projection * view, frame.width, frame.height);
    // HUD attachments into their own image, as big as the view, with their
    // labels.
    const bool hud = drawHud(commands, scene, frame.width, frame.height, drawsReady);
    if(hud)
    {
        drawLabels(commands, hudImage_, true);
    }

    SDL_GPUColorTargetInfo color = {};
    color.texture = hdrTarget_;
    color.clear_color = SDL_FColor{light.skyColor.r, light.skyColor.g, light.skyColor.b, 1.0f};
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo depth = {};
    depth.texture = depthTarget_;
    depth.clear_depth = 1.0f;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_STORE; // dynamic exposure tells the sky by it
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.cycle = true;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color, 1, &depth);
    ScenePass scenePass(commands, pass, p, frameVs, frameFs, lightBuffer_.buffer.get(),
                        probeBuffer_.buffer.get());
    if(atmo.enabled)
    {
        drawSky(scenePass, atmo, view, projection);
    }
    // Terrain first: it covers the most screen, so it fills depth early.
    drawTerrain(scenePass, scene);

    // Opaque objects, then blended ones from far to near (as the official
    // viewer: each face by its own center's depth along the view, moved
    // forward by a quarter of its size), then blended rigged mesh by wearer
    // and attachment order.
    std::vector<const ObjectDraw*> opaque;
    struct Blended
    {
        float depth;  // faces: along the view; rigged: the wearer's distance
        size_t index; // scene order, to keep ties stable
        const ObjectDraw* draw;
    };
    std::vector<Blended> blended;
    std::vector<Blended> blendedRigged;
    const glm::vec3 viewAxis = glm::normalize(camera.target - camera.position);
    for(const ObjectDraw& draw : scene.objects)
    {
        if(!drawsReady || !draw.mesh || !*draw.mesh || draw.mesh->layout == VertexLayout::Terrain ||
           draw.shadowOnly || draw.hud || draw.pickOnly)
        {
            continue;
        }
        if(draw.mesh->layout == VertexLayout::Skinned && draw.joints.empty())
        {
            continue;
        }
        const auto* bp = std::get_if<BlinnPhongMaterial>(&draw.material);
        const auto* pbrBlend = std::get_if<PbrMaterial>(&draw.material);
        if((bp && bp->blend) || (pbrBlend && pbrBlend->blend))
        {
            const size_t index = static_cast<size_t>(&draw - scene.objects.data());
            if(draw.mesh->layout == VertexLayout::Skinned)
            {
                const glm::vec3 d = glm::vec3(draw.transform[3]) - camera.position;
                blendedRigged.push_back({glm::dot(d, d), index, &draw});
                continue;
            }
            const glm::vec3 localCenter = (draw.mesh->boundsMin + draw.mesh->boundsMax) * 0.5f;
            const glm::vec3 center(draw.transform * glm::vec4(localCenter, 1.0f));
            // The face's box in world axes: |rotation and scale| times its size.
            const glm::mat3 m(draw.transform);
            const glm::mat3 absolute(glm::abs(m[0]), glm::abs(m[1]), glm::abs(m[2]));
            const glm::vec3 size = absolute * (draw.mesh->boundsMax - draw.mesh->boundsMin);
            const glm::vec3 toward = center - camera.position - 0.25f * size * viewAxis;
            blended.push_back({glm::dot(toward, viewAxis), index, &draw});
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
               static_cast<uint64_t>(materialKind(*draw)) << 60 | textureBits << 16 |
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
    std::ranges::sort(blended, [](const Blended& a, const Blended& b)
                      { return a.depth != b.depth ? a.depth > b.depth : a.index < b.index; });
    std::ranges::sort(blendedRigged,
                      [](const Blended& a, const Blended& b)
                      {
                          if(a.draw->wearer != b.draw->wearer)
                          {
                              // Farther wearers first; the pointer breaks ties.
                              return a.depth != b.depth ? a.depth > b.depth
                                                        : a.draw->wearer < b.draw->wearer;
                          }
                          if(a.draw->layer != b.draw->layer)
                          {
                              return a.draw->layer > b.draw->layer;
                          }
                          return a.index < b.index;
                      });
    std::vector<const ObjectDraw*> order = std::move(opaque);
    for(const Blended& b : blended)
    {
        order.push_back(b.draw);
    }
    for(const Blended& b : blendedRigged)
    {
        order.push_back(b.draw);
    }

    drawObjects(scenePass, scene, order);
    if(hud && scene.hud.placement == Hud::Placement::World)
    {
        drawHudPanel(scenePass, scene.hud, projection * view);
    }
    SDL_EndGPURenderPass(pass);

    updateExposure(commands, light);
    drawTonemap(commands, frame.target, light, true);
    drawOutline(commands, scene, frame.target, frame.width, frame.height, frameVs);
    drawLabels(commands, frame.target, false);
    if(hud && scene.hud.placement == Hud::Placement::Screen)
    {
        compositeHud(commands, frame.target);
    }

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
            drawTonemap(commands, screenshotTexture_, light, true);
            drawOutline(commands, scene, screenshotTexture_, frame.width, frame.height, frameVs);
            drawLabels(commands, screenshotTexture_, false);
            if(hud && scene.hud.placement == Hud::Placement::Screen)
            {
                compositeHud(commands, screenshotTexture_);
            }
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

void Renderer::drawObjects(ScenePass& pass, const Scene& scene,
                           std::span<const ObjectDraw* const> order)
{
    const Pipelines& p = *pipelines_;
    auto texture = [](const GpuTexture* t, const GpuTexture& fallback)
    { return t && *t ? t->get() : fallback.get(); };
    // A base color: none is white, one not in yet is gray.
    auto baseColor = [&](const GpuTexture* t, bool loading)
    { return texture(t, loading ? loading_ : white_); };
    const auto probes = probeSamplers();
    // Every object pipeline reads the same storage buffers and instance
    // buffer; bound again with each pipeline, to be safe across backends.
    SDL_GPUGraphicsPipeline* objectResourcesFor = nullptr;
    for(const ObjectDraw* drawPointer : order)
    {
        const ObjectDraw& draw = *drawPointer;
        const bool skinned = draw.mesh->layout == VertexLayout::Skinned;
        if(const auto* m = std::get_if<BlinnPhongMaterial>(&draw.material))
        {
            pass.bindPipeline(skinned ? (m->blend ? p.blinnPhongSkinnedBlend : p.blinnPhongSkinned)
                                      : (m->blend ? p.blinnPhongBlend : p.blinnPhong));
            const SDL_GPUTextureSamplerBinding samplers[] = {
                {baseColor(m->diffuse, m->diffuseLoading), p.repeatSampler},
                {texture(m->normal, flatNormal_), p.repeatSampler},
                {texture(m->specular, white_), p.repeatSampler},
                {shadowMap_, p.shadowSampler},
                {projectors_, p.clampSampler},
                probes[0],
                probes[1],
                probes[2],
            };
            pass.bindSamplers(samplers);
        }
        else if(const auto* t = std::get_if<PbrMaterial>(&draw.material))
        {
            pass.bindPipeline(
                p.pbrVariants[t->blend ? 1 : 0][t->doubleSided ? 1 : 0][skinned ? 1 : 0]);
            const SDL_GPUTextureSamplerBinding samplers[] = {
                {baseColor(t->albedo, t->albedoLoading), p.repeatSampler},
                {texture(t->orm, white_), p.repeatSampler},
                {texture(t->emissive, white_), p.repeatSampler},
                {texture(t->normal, flatNormal_), p.repeatSampler},
                {shadowMap_, p.shadowSampler},
                {projectors_, p.clampSampler},
                probes[0],
                probes[1],
                probes[2],
            };
            pass.bindSamplers(samplers);
        }
        if(objectResourcesFor != pass.boundPipeline())
        {
            objectResourcesFor = pass.boundPipeline();
            // Without skinned draws there's no joint buffer: any buffer
            // fills the slot.
            SDL_GPUBuffer* vertexStorage[] = {drawBuffer_.buffer.get(),
                                              jointBuffer_.buffer ? jointBuffer_.buffer.get()
                                                                  : drawBuffer_.buffer.get()};
            SDL_BindGPUVertexStorageBuffers(pass.pass(), 0, vertexStorage, 2);
            SDL_GPUBuffer* fragmentStorage[] = {drawBuffer_.buffer.get(), lightBuffer_.buffer.get(),
                                                probeBuffer_.buffer.get()};
            SDL_BindGPUFragmentStorageBuffers(pass.pass(), 0, fragmentStorage, 3);
            const SDL_GPUBufferBinding instances = {instanceBuffer_.buffer.get(), 0};
            SDL_BindGPUVertexBuffers(pass.pass(), 1, &instances, 1);
        }
        pass.drawMesh(*draw.mesh, static_cast<uint32_t>(drawPointer - scene.objects.data()));
    }
}

void Renderer::drawSky(ScenePass& pass, const Atmosphere& atmo, const glm::mat4& view,
                       const glm::mat4& projection)
{
    SDL_BindGPUGraphicsPipeline(pass.pass(), pipelines_->sky);
    pass.forgetPipeline();
    const SDL_GPUTextureSamplerBinding clouds[] = {
        {atmo.clouds && *atmo.clouds ? atmo.clouds->get() : white_.get(),
         pipelines_->repeatSampler}};
    pass.bindSamplers(clouds);
    pushFragmentUniforms(pass.commands(), 0, pass.frameFs());
    // Rotation only: the shader wants directions, which don't need the
    // camera's position (leaving it out keeps them precise).
    pushFragmentUniforms(pass.commands(), 1,
                         SkyFS{glm::inverse(projection * glm::mat4(glm::mat3(view)))});
    SDL_DrawGPUPrimitives(pass.pass(), 3, 1, 0, 0);
}

void Renderer::drawTerrain(ScenePass& pass, const Scene& scene)
{
    const Pipelines& p = *pipelines_;
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
    auto texture = [](const GpuTexture* t, const GpuTexture& fallback)
    { return t && *t ? t->get() : fallback.get(); };
    const auto probes = probeSamplers();
    for(const TerrainDraw& draw : scene.terrain)
    {
        if(!draw.mesh || !*draw.mesh || draw.mesh->layout != VertexLayout::Terrain)
        {
            continue;
        }
        // Out of the view's frustum: only for passes that see all around.
        if(draw.shadowOnly && !pass.allAround())
        {
            continue;
        }
        if(const auto* m = std::get_if<TerrainBlinnPhongMaterial>(&draw.material))
        {
            pass.bindPipeline(p.terrainBlinnPhong);
            const SDL_GPUTextureSamplerBinding samplers[] = {
                {texture(m->diffuseLayers, whiteLayers_), p.repeatSampler},
                {terrainBlendRamp_.get(), p.clampSampler},
                {shadowMap_, p.shadowSampler},
                {projectors_, p.clampSampler},
                probes[0],
                probes[1],
                probes[2]};
            pass.bindSamplers(samplers);
            pushVertexUniforms(pass.commands(), 1,
                               terrainVs(draw.transform, m->textureRepeatsPerMeter));
        }
        else if(const auto* t = std::get_if<TerrainPbrMaterial>(&draw.material))
        {
            pass.bindPipeline(p.terrainPbr);
            const SDL_GPUTextureSamplerBinding samplers[] = {
                {texture(t->albedoLayers, whiteLayers_), p.repeatSampler},
                {texture(t->ormLayers, whiteLayers_), p.repeatSampler},
                {texture(t->emissiveLayers, blackLayers_), p.repeatSampler},
                {texture(t->normalLayers, flatNormalLayers_), p.repeatSampler},
                {terrainBlendRamp_.get(), p.clampSampler},
                {shadowMap_, p.shadowSampler},
                {projectors_, p.clampSampler},
                probes[0],
                probes[1],
                probes[2],
            };
            pass.bindSamplers(samplers);
            pushVertexUniforms(pass.commands(), 1,
                               terrainVs(draw.transform, t->textureRepeatsPerMeter));
            TerrainPbrFS fs{};
            for(size_t i = 0; i < 4; i++)
            {
                const auto& layer = t->layers[i];
                fs.baseColorFactor[i] = layer.baseColorFactor;
                fs.emissiveTint[i] = glm::vec4(layer.emissiveTint, layer.emissiveStrength);
                fs.factors[i] =
                    glm::vec4(layer.roughness, layer.metallic, layer.occlusionStrength, 0.0f);
            }
            pushFragmentUniforms(pass.commands(), 1, fs);
        }
        pass.drawMesh(*draw.mesh);
    }
}

void Renderer::updateExposure(SDL_GPUCommandBuffer* commands, const Lighting& light)
{
    if(!luminance_)
    {
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = kExposureFormat;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = kLuminanceSize;
        info.height = kLuminanceSize;
        info.layer_count_or_depth = 1;
        info.num_levels = kLuminanceLevels;
        luminance_ = SDL_CreateGPUTexture(device_, &info);
        info.width = 1;
        info.height = 1;
        info.num_levels = 1;
        for(SDL_GPUTexture*& t : exposureMaps_)
        {
            t = SDL_CreateGPUTexture(device_, &info);
        }
        exposureHistory_ = false;
    }
    const uint64_t now = SDL_GetTicksNS();
    const float seconds =
        std::clamp(static_cast<float>(now - lastExposureTicks_) * 1e-9f, 0.0f, 1.0f);
    lastExposureTicks_ = now;

    SDL_GPUColorTargetInfo color = {};
    color.texture = luminance_;
    color.load_op = SDL_GPU_LOADOP_DONT_CARE;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, pipelines_->luminance);
    const SDL_GPUTextureSamplerBinding scene[] = {{hdrTarget_, pipelines_->clampSampler},
                                                  {depthTarget_, pipelines_->clampSampler}};
    SDL_BindGPUFragmentSamplers(pass, 0, scene, 2);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    SDL_GenerateMipmapsForGPUTexture(commands, luminance_);

    const uint32_t previous = exposureIndex_;
    exposureIndex_ ^= 1u;
    color.texture = exposureMaps_[exposureIndex_];
    pass = SDL_BeginGPURenderPass(commands, &color, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, pipelines_->exposure);
    const SDL_GPUTextureSamplerBinding inputs[] = {
        {luminance_, pipelines_->clampSampler},
        {exposureMaps_[previous], pipelines_->clampSampler}};
    SDL_BindGPUFragmentSamplers(pass, 0, inputs, 2);
    // The official viewer's RenderDynamicExposureCoefficient, SpeedError and
    // SpeedTarget: a luminance of 0.175 takes the least exposure, and 90% of
    // the way to a new exposure takes 2 s.
    pushFragmentUniforms(
        commands, 0,
        ExposureFS{glm::vec4(0.175f, light.exposureMin, light.exposureMax, 0.1f),
                   glm::vec4(seconds, exposureHistory_ ? 1.0f : 0.0f, 2.0f, 0.0f)});
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    exposureHistory_ = true;
}

void Renderer::drawTonemap(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target,
                           const Lighting& light, bool clear)
{
    SDL_GPUColorTargetInfo color = {};
    color.texture = target;
    color.load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, pipelines_->tonemap);
    const SDL_GPUTextureSamplerBinding inputs[] = {
        {hdrTarget_, pipelines_->clampSampler},
        {exposureMaps_[exposureIndex_], pipelines_->clampSampler}};
    SDL_BindGPUFragmentSamplers(pass, 0, inputs, 2);
    pushFragmentUniforms(commands, 0,
                         TonemapFS{glm::vec4(light.exposure, light.tonemapMix, 0.0f, 0.0f)});
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

namespace
{

template <typename Vertex>
void measure(Mesh& mesh, std::span<const Vertex> vertices)
{
    if(vertices.empty())
    {
        return;
    }
    mesh.boundsMin = mesh.boundsMax = vertices[0].position;
    for(const Vertex& v : vertices)
    {
        mesh.boundsMin = glm::min(mesh.boundsMin, v.position);
        mesh.boundsMax = glm::max(mesh.boundsMax, v.position);
    }
}

} // namespace

Mesh Renderer::createMesh(std::span<const ObjectVertex> vertices, std::span<const uint32_t> indices)
{
    Mesh mesh = createMesh(vertices.data(), vertices.size_bytes(), indices, VertexLayout::Object);
    measure(mesh, vertices);
    return mesh;
}

Mesh Renderer::createMesh(std::span<const SkinnedVertex> vertices,
                          std::span<const uint32_t> indices)
{
    Mesh mesh = createMesh(vertices.data(), vertices.size_bytes(), indices, VertexLayout::Skinned);
    measure(mesh, vertices);
    return mesh;
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

void Renderer::drawShadows(SDL_GPUCommandBuffer* commands, const Scene& scene,
                           const Cascades& cascades, bool drawsReady)
{
    const Pipelines& p = *pipelines_;
    auto texture = [](const GpuTexture* t, const GpuTexture& fallback)
    { return t && *t ? t->get() : fallback.get(); };

    // The casters: opaque, masked and blended faces (the last two cast
    // where they're at least half opaque), with a sphere around each.
    enum Kind : uint8_t
    {
        Opaque,
        Masked,
        SkinnedOpaque,
        SkinnedMasked,
    };
    struct Caster
    {
        uint64_t key; // kind, then texture: few state changes
        uint32_t index;
    };
    std::vector<Caster> casters;
    casterSpheres_.assign(scene.objects.size(), glm::vec4(0.0f));
    for(size_t i = 0; drawsReady && i < scene.objects.size(); i++)
    {
        const ObjectDraw& draw = scene.objects[i];
        if(!draw.mesh || !*draw.mesh || draw.mesh->layout == VertexLayout::Terrain || draw.hud ||
           draw.pickOnly)
        {
            continue;
        }
        const bool skinned = draw.mesh->layout == VertexLayout::Skinned;
        if(skinned && draw.joints.empty())
        {
            continue;
        }
        bool masked = false;
        const void* diffuse = nullptr;
        if(const auto* m = std::get_if<BlinnPhongMaterial>(&draw.material))
        {
            if(m->fullbright && m->blend)
            {
                continue; // glowing see-through things (signs, effects) cast nothing
            }
            masked = m->blend || m->alphaCutoff >= 0.0f;
            diffuse = m->diffuse;
        }
        else if(const auto* t = std::get_if<PbrMaterial>(&draw.material))
        {
            masked = t->alphaCutoff >= 0.0f || t->blend;
            diffuse = t->albedo;
        }
        if(skinned)
        {
            // Skinned vertices move with the skeleton: a generous sphere
            // around the root covers a body and what it wears.
            casterSpheres_[i] = glm::vec4(glm::vec3(draw.transform[3]), 4.0f);
        }
        else
        {
            const glm::vec3 localCenter = (draw.mesh->boundsMin + draw.mesh->boundsMax) * 0.5f;
            const glm::mat3 m(draw.transform);
            const glm::mat3 absolute(glm::abs(m[0]), glm::abs(m[1]), glm::abs(m[2]));
            const glm::vec3 size = absolute * (draw.mesh->boundsMax - draw.mesh->boundsMin);
            casterSpheres_[i] = glm::vec4(glm::vec3(draw.transform * glm::vec4(localCenter, 1.0f)),
                                          0.5f * glm::length(size));
        }
        const uint64_t kind =
            skinned ? (masked ? SkinnedMasked : SkinnedOpaque) : (masked ? Masked : Opaque);
        const uint64_t textureBits =
            masked ? (reinterpret_cast<uintptr_t>(diffuse) >> 4) & ((1ull << 48) - 1) : 0;
        casters.push_back({kind << 56 | textureBits, static_cast<uint32_t>(i)});
    }
    std::ranges::sort(casters, {}, &Caster::key);
    // In light view coordinates, for the per-cascade tests.
    std::vector<glm::vec4> inLight(casters.size());
    for(size_t c = 0; c < casters.size(); c++)
    {
        const glm::vec4& sphere = casterSpheres_[casters[c].index];
        inLight[c] = glm::vec4(
            glm::vec3(cascades.lightRotation * glm::vec4(glm::vec3(sphere), 1.0f)), sphere.w);
    }

    for(uint32_t cascade = 0; cascade < kShadowCascades; cascade++)
    {
        SDL_GPUDepthStencilTargetInfo depth = {};
        depth.texture = shadowMap_;
        depth.clear_depth = 1.0f;
        depth.load_op = SDL_GPU_LOADOP_CLEAR;
        depth.store_op = SDL_GPU_STOREOP_STORE;
        depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        depth.cycle = cascade == 0; // last frame may still be reading it
        depth.layer = static_cast<Uint8>(cascade);
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, nullptr, 0, &depth);
        const FrameVS frameVs{cascades.matrices[cascade], glm::vec4(0.0f)};
        shadowDrawCounts_[cascade] = 0;
        const glm::vec4& bounds = cascades.bounds[cascade];

        SDL_GPUBuffer* boundVertices = nullptr;
        SDL_GPUBuffer* boundIndices = nullptr;
        SDL_GPUGraphicsPipeline* boundPipeline = nullptr;
        SDL_GPUTexture* boundTexture = nullptr;
        auto drawMesh = [&](const Mesh& mesh, uint32_t instance)
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

        // Terrain: hills shade the land behind them.
        for(const TerrainDraw& draw : scene.terrain)
        {
            if(!draw.mesh || !*draw.mesh || draw.mesh->layout != VertexLayout::Terrain)
            {
                continue;
            }
            if(boundPipeline != p.shadowTerrain)
            {
                boundPipeline = p.shadowTerrain;
                SDL_BindGPUGraphicsPipeline(pass, boundPipeline);
                pushVertexUniforms(commands, 0, frameVs);
            }
            pushVertexUniforms(commands, 1, TerrainVS{draw.transform, glm::vec4(0.0f)});
            drawMesh(*draw.mesh, 0);
        }

        for(size_t c = 0; c < casters.size(); c++)
        {
            // Too small to matter at this cascade's resolution (a shadow of a
            // texel or two), or not reaching it.
            if(inLight[c].w <
                   kMinCasterTexels * cascades.texels[static_cast<glm::length_t>(cascade)] ||
               !castsInto(glm::vec3(inLight[c]), inLight[c].w, bounds))
            {
                continue;
            }
            const ObjectDraw& draw = scene.objects[casters[c].index];
            const auto kind = static_cast<Kind>(casters[c].key >> 56);
            SDL_GPUGraphicsPipeline* pipeline = kind == Opaque          ? p.shadowObject
                                                : kind == Masked        ? p.shadowObjectMasked
                                                : kind == SkinnedOpaque ? p.shadowSkinned
                                                                        : p.shadowSkinnedMasked;
            if(pipeline != boundPipeline)
            {
                boundPipeline = pipeline;
                SDL_BindGPUGraphicsPipeline(pass, pipeline);
                pushVertexUniforms(commands, 0, frameVs);
                SDL_GPUBuffer* vertexStorage[] = {drawBuffer_.buffer.get(),
                                                  jointBuffer_.buffer ? jointBuffer_.buffer.get()
                                                                      : drawBuffer_.buffer.get()};
                SDL_BindGPUVertexStorageBuffers(pass, 0, vertexStorage, 2);
                const SDL_GPUBufferBinding instances = {instanceBuffer_.buffer.get(), 0};
                SDL_BindGPUVertexBuffers(pass, 1, &instances, 1);
                if(kind == Masked || kind == SkinnedMasked)
                {
                    SDL_GPUBuffer* fragmentStorage = drawBuffer_.buffer.get();
                    SDL_BindGPUFragmentStorageBuffers(pass, 0, &fragmentStorage, 1);
                }
                boundTexture = nullptr;
            }
            if(kind == Masked || kind == SkinnedMasked)
            {
                const GpuTexture* diffuse = nullptr;
                if(const auto* m = std::get_if<BlinnPhongMaterial>(&draw.material))
                {
                    diffuse = m->diffuse;
                }
                else if(const auto* t = std::get_if<PbrMaterial>(&draw.material))
                {
                    diffuse = t->albedo;
                }
                SDL_GPUTexture* tex = texture(diffuse, white_);
                if(tex != boundTexture)
                {
                    boundTexture = tex;
                    const SDL_GPUTextureSamplerBinding binding = {tex, p.repeatSampler};
                    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
                }
            }
            drawMesh(*draw.mesh, casters[c].index);
            shadowDrawCounts_[cascade]++;
        }
        SDL_EndGPURenderPass(pass);
    }
}

uint32_t Renderer::uploadLights(SDL_GPUCommandBuffer* commands, const Scene& scene)
{
    // The buffer always exists (the shaders bind it), with room for one.
    if(!reserve(lightBuffer_,
                static_cast<uint32_t>(
                    sizeof(LightData) *
                    std::max<size_t>(1, std::min<size_t>(scene.lights.size(), kMaxLights))),
                SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ))
    {
        return 0;
    }
    // Those reaching the view, nearest (to their reach) first.
    std::vector<std::pair<float, const LocalLight*>> chosen;
    for(const LocalLight& light : scene.lights)
    {
        if(light.range <= 0.0f || glm::dot(light.color, light.color) < 1e-6f ||
           !scene.frustum.containsSphere(light.position, light.range))
        {
            continue;
        }
        chosen.emplace_back(
            std::max(0.0f, glm::length(light.position - scene.camera.position) - light.range),
            &light);
    }
    const size_t count = std::min<size_t>(chosen.size(), kMaxLights);
    std::partial_sort(chosen.begin(), chosen.begin() + static_cast<ptrdiff_t>(count), chosen.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
    if(count == 0)
    {
        return 0;
    }
    // Projector textures into their layers first (blits go outside passes).
    lightUploads_++;
    std::vector<LightData> data(count);
    for(size_t i = 0; i < count; i++)
    {
        const LocalLight& l = *chosen[i].second;
        LightData& d = data[i];
        d.positionRange = glm::vec4(l.position, l.range);
        d.colorFalloff = glm::vec4(l.color, l.falloff);
        d.originLayer = glm::vec4(0.0f, 0.0f, 0.0f, -2.0f); // a point light
        if(!l.projector)
        {
            continue;
        }
        const LocalLight::Projector& pr = *l.projector;
        int layer = -1; // gray, until its texture is in
        float maxLod = 0.0f;
        float lodBias = 0.0f;
        if(pr.texture && *pr.texture && projectors_)
        {
            layer = projectorLayer(commands, *pr.texture);
            if(layer >= 0)
            {
                // Mip levels as the texture's own (official viewer): level
                // n here is level n + bias of the original.
                maxLod = std::log2(static_cast<float>(std::max(pr.texture->width(), 1u)));
                lodBias = maxLod - std::log2(static_cast<float>(kProjectorSize));
            }
        }
        d.projector = pr.matrix;
        d.originLayer = glm::vec4(pr.origin, static_cast<float>(layer));
        d.axisFocus = glm::vec4(pr.axis, pr.focus);
        d.projectorParams = glm::vec4(pr.depth, maxLod, pr.ambiance, lodBias);
    }
    auto* out =
        static_cast<LightData*>(SDL_MapGPUTransferBuffer(device_, lightBuffer_.transfer, true));
    std::copy(data.begin(), data.end(), out);
    SDL_UnmapGPUTransferBuffer(device_, lightBuffer_.transfer);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    const SDL_GPUTransferBufferLocation source = {lightBuffer_.transfer, 0};
    const SDL_GPUBufferRegion destination = {lightBuffer_.buffer.get(), 0,
                                             static_cast<uint32_t>(count * sizeof(LightData))};
    SDL_UploadToGPUBuffer(copy, &source, &destination, true);
    SDL_EndGPUCopyPass(copy);
    return static_cast<uint32_t>(count);
}

int Renderer::projectorLayer(SDL_GPUCommandBuffer* commands, const GpuTexture& texture)
{
    ProjectorLayer* oldest = nullptr;
    for(ProjectorLayer& layer : projectorLayers_)
    {
        if(layer.source == texture.get() && layer.width == texture.width())
        {
            layer.usedIn = lightUploads_;
            return static_cast<int>(&layer - projectorLayers_.data());
        }
        if(layer.usedIn != lightUploads_ && (!oldest || layer.usedIn < oldest->usedIn))
        {
            oldest = &layer;
        }
    }
    if(!oldest)
    {
        return -1;
    }
    oldest->source = texture.get();
    oldest->width = texture.width();
    oldest->usedIn = lightUploads_;
    const auto index = static_cast<uint32_t>(oldest - projectorLayers_.data());
    // Each level from the source level nearest its size (at least as big),
    // scaled to fit.
    for(uint32_t level = 0; level < kProjectorLevels; level++)
    {
        const uint32_t size = std::max(kProjectorSize >> level, 1u);
        uint32_t from = 0;
        while(from + 1 < texture.levels() && std::max(texture.width() >> (from + 1), 1u) >= size &&
              std::max(texture.height() >> (from + 1), 1u) >= size)
        {
            from++;
        }
        SDL_GPUBlitInfo blit = {};
        blit.source.texture = texture.get();
        blit.source.mip_level = from;
        blit.source.w = std::max(texture.width() >> from, 1u);
        blit.source.h = std::max(texture.height() >> from, 1u);
        blit.destination.texture = projectors_;
        blit.destination.mip_level = level;
        blit.destination.layer_or_depth_plane = index;
        blit.destination.w = size;
        blit.destination.h = size;
        blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
        blit.filter = SDL_GPU_FILTER_LINEAR;
        SDL_BlitGPUTexture(commands, &blit);
    }
    return static_cast<int>(index);
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
        out.uvRow0 = glm::vec4(draw.uvRow0, draw.planar ? 1.0f : 0.0f);
        out.uvRow1 = glm::vec4(draw.uvRow1, 0.0f);
        if(const auto* m = std::get_if<BlinnPhongMaterial>(&draw.material))
        {
            out.material[0] = m->diffuseTint;
            out.material[1] = glm::vec4(m->specularTint, m->glossiness);
            out.material[2] = glm::vec4(m->alphaCutoff, m->fullbright ? 1.0f : 0.0f,
                                        m->emissiveMask ? 1.0f : 0.0f, m->opaque ? 1.0f : 0.0f);
        }
        else if(const auto* t = std::get_if<PbrMaterial>(&draw.material))
        {
            out.material[0] = t->baseColorFactor;
            out.material[1] = glm::vec4(t->emissiveTint, t->emissiveStrength);
            out.material[2] =
                glm::vec4(t->roughness, t->metallic, t->occlusionStrength, t->alphaCutoff);
        }
        // The joints' first matrix; the pick id (low, high) and whether the
        // draw is only for picking (pick.frag).
        out.skin = glm::uvec4(jointBase, static_cast<uint32_t>(draw.pickId),
                              static_cast<uint32_t>(draw.pickId >> 32), draw.pickOnly ? 1u : 0u);
        const auto* legacy = std::get_if<BlinnPhongMaterial>(&draw.material);
        if(legacy && legacy->mapTransforms)
        {
            out.mapUv[0] = glm::vec4(legacy->normalUv[0], 0.0f);
            out.mapUv[1] = glm::vec4(legacy->normalUv[1], 0.0f);
            out.mapUv[2] = glm::vec4(legacy->specularUv[0], 0.0f);
            out.mapUv[3] = glm::vec4(legacy->specularUv[1], 0.0f);
        }
        else
        {
            out.mapUv[0] = out.mapUv[2] = glm::vec4(draw.uvRow0, 0.0f);
            out.mapUv[1] = out.mapUv[3] = glm::vec4(draw.uvRow1, 0.0f);
        }
        // Environment intensity rides along in a normal map row's w.
        if(legacy)
        {
            out.mapUv[1].w = legacy->environment;
        }
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
