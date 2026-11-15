// Particles (Scene::particles), drawn as the official viewer draws them
// (llvopartgroup.cpp, LLParticlePartition): quads (or ribbon segments) in
// the alpha pass, far ones first, each blended with its own blend factors,
// emissive ones unlit; batched where neighbours share a texture and blend.
// The quads face the camera in the vertex shader (particle.vert), so both
// eyes share one upload.

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

constexpr uint32_t kMaxQuads = 8192; // 4 vertices each, in 16-bit indices

struct ParticleVertex
{
    glm::vec3 center;   // a ribbon's: the corner
    glm::vec4 corner;   // which (-1 or 1 each), size across and up
    glm::vec3 velocity; // to point along
    glm::vec2 uv;
    glm::vec4 color;
    glm::vec2 misc; // glow, flags (1 ribbon, 2 fullbright)
};
static_assert(sizeof(ParticleVertex) == 72);

// The official blend factors (LLRender::eBlendFactor order).
SDL_GPUBlendFactor blendFactor(uint8_t f)
{
    switch(f)
    {
    case 0:
        return SDL_GPU_BLENDFACTOR_ONE;
    case 1:
        return SDL_GPU_BLENDFACTOR_ZERO;
    case 2:
        return SDL_GPU_BLENDFACTOR_DST_COLOR;
    case 3:
        return SDL_GPU_BLENDFACTOR_SRC_COLOR;
    case 4:
        return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
    case 5:
        return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    case 9:
        return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    default:
        return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    }
}

const SDL_GPUVertexBufferDescription kParticleBuffer = {0, sizeof(ParticleVertex),
                                                        SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
const SDL_GPUVertexAttribute kParticleAttributes[] = {
    {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(ParticleVertex, center)},
    {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(ParticleVertex, corner)},
    {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(ParticleVertex, velocity)},
    {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(ParticleVertex, uv)},
    {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(ParticleVertex, color)},
    {5, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(ParticleVertex, misc)},
};

SDL_GPUGraphicsPipelineCreateInfo particlePipelineInfo(SDL_GPUShader* vert, SDL_GPUShader* frag,
                                                       const SDL_GPUColorTargetDescription* color,
                                                       SDL_GPUTextureFormat depthFormat)
{
    SDL_GPUGraphicsPipelineCreateInfo info = {};
    info.vertex_shader = vert;
    info.fragment_shader = frag;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_buffer_descriptions = &kParticleBuffer;
    info.vertex_input_state.vertex_attributes = kParticleAttributes;
    info.vertex_input_state.num_vertex_attributes =
        static_cast<Uint32>(std::size(kParticleAttributes));
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.depth_stencil_state.enable_depth_test = true;
    info.depth_stencil_state.enable_depth_write = false;
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    info.target_info.color_target_descriptions = color;
    info.target_info.num_color_targets = 1;
    info.target_info.has_depth_stencil_target = true;
    info.target_info.depth_stencil_format = depthFormat;
    return info;
}

} // namespace

SDL_GPUGraphicsPipeline* Renderer::particlePipeline(uint8_t source, uint8_t dest)
{
    Pipelines& p = *pipelines_;
    const auto key = static_cast<uint16_t>(source << 8 | dest);
    if(const auto it = p.particle.find(key); it != p.particle.end())
    {
        return it->second;
    }
    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    if(p.particleVert && p.particleFrag)
    {
        SDL_GPUColorTargetDescription color = {};
        color.format = kHdrFormat;
        color.blend_state.enable_blend = true;
        color.blend_state.src_color_blendfactor = blendFactor(source);
        color.blend_state.dst_color_blendfactor = blendFactor(dest);
        color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        const SDL_GPUGraphicsPipelineCreateInfo info =
            particlePipelineInfo(p.particleVert, p.particleFrag, &color, depthFormat_);
        pipeline = SDL_CreateGPUGraphicsPipeline(device_, &info);
        if(!pipeline)
        {
            log::error("creating a particle pipeline failed: {}", SDL_GetError());
        }
    }
    p.particle[key] = pipeline; // a failure isn't tried again
    return pipeline;
}

void Renderer::uploadParticles(SDL_GPUCommandBuffer* commands, const Scene& scene,
                               const glm::vec3& camera)
{
    particleBatches_.clear();
    if(scene.particles.empty())
    {
        return;
    }
    if(!particleIndices_)
    {
        // Two triangles a quad: corners 0 1 2, 1 3 2.
        std::vector<uint16_t> indices;
        indices.reserve(kMaxQuads * 6);
        for(uint32_t q = 0; q < kMaxQuads; q++)
        {
            const auto v = static_cast<uint16_t>(q * 4);
            for(const int i : {0, 1, 2, 1, 3, 2})
            {
                indices.push_back(static_cast<uint16_t>(v + i));
            }
        }
        const auto bytes = static_cast<uint32_t>(indices.size() * sizeof(uint16_t));
        SDL_GPUBufferCreateInfo info = {};
        info.usage = SDL_GPU_BUFFERUSAGE_INDEX;
        info.size = bytes;
        GpuBuffer buffer(device_, SDL_CreateGPUBuffer(device_, &info), bytes);
        SDL_GPUTransferBufferCreateInfo transferInfo = {};
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transferInfo.size = bytes;
        SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &transferInfo);
        if(!buffer || !transfer)
        {
            log::error("creating the particle index buffer failed: {}", SDL_GetError());
            if(transfer)
            {
                SDL_ReleaseGPUTransferBuffer(device_, transfer);
            }
            return;
        }
        std::memcpy(SDL_MapGPUTransferBuffer(device_, transfer, false), indices.data(), bytes);
        SDL_UnmapGPUTransferBuffer(device_, transfer);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        const SDL_GPUTransferBufferLocation from = {transfer, 0};
        const SDL_GPUBufferRegion to = {buffer.get(), 0, bytes};
        SDL_UploadToGPUBuffer(copy, &from, &to, false);
        SDL_EndGPUCopyPass(copy);
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        particleIndices_ = std::move(buffer);
    }

    // Far ones first (each by its middle).
    std::vector<std::pair<float, const ParticleDraw*>> order;
    order.reserve(scene.particles.size());
    for(const ParticleDraw& p : scene.particles)
    {
        const glm::vec3 middle = p.ribbon ? (p.position + p.otherPosition) * 0.5f : p.position;
        const glm::vec3 d = middle - camera;
        order.emplace_back(glm::dot(d, d), &p);
    }
    std::ranges::stable_sort(order, [](const auto& a, const auto& b) { return a.first > b.first; });
    if(order.size() > kMaxQuads)
    {
        order.erase(order.begin(), order.end() - kMaxQuads); // the nearest
    }

    const auto quads = static_cast<uint32_t>(order.size());
    const uint32_t bytes = quads * 4 * static_cast<uint32_t>(sizeof(ParticleVertex));
    if(!reserve(particleVertices_, bytes, SDL_GPU_BUFFERUSAGE_VERTEX))
    {
        return;
    }
    auto* out = static_cast<ParticleVertex*>(
        SDL_MapGPUTransferBuffer(device_, particleVertices_.transfer, true));
    uint32_t quad = 0;
    for(const auto& [distance, p] : order)
    {
        ParticleVertex* v = out + quad * 4;
        const float fullbright = p->fullbright ? 2.0f : 0.0f;
        if(p->ribbon)
        {
            // The other end, then this one (llvopartgroup.cpp's order).
            const glm::vec3 axis = p->axis * (0.5f * p->size.x);
            const glm::vec3 otherAxis = p->otherAxis * (0.5f * p->otherWidth);
            const glm::vec2 misc(0.0f, 1.0f + fullbright);
            v[0] = {p->otherPosition + otherAxis,
                    glm::vec4(0.0f),
                    glm::vec3(0.0f),
                    {0.0f, 1.0f},
                    p->otherColor,
                    {p->otherGlow, misc.y}};
            v[1] = {p->otherPosition - otherAxis,
                    glm::vec4(0.0f),
                    glm::vec3(0.0f),
                    {0.0f, 0.0f},
                    p->otherColor,
                    {p->otherGlow, misc.y}};
            v[2] = {p->position + axis, glm::vec4(0.0f), glm::vec3(0.0f),
                    {1.0f, 1.0f},       p->color,        {p->glow, misc.y}};
            v[3] = {p->position - axis, glm::vec4(0.0f), glm::vec3(0.0f),
                    {1.0f, 0.0f},       p->color,        {p->glow, misc.y}};
        }
        else
        {
            // Up-left, down-left, up-right, down-right.
            const glm::vec2 corners[] = {
                {-1.0f, 1.0f}, {-1.0f, -1.0f}, {1.0f, 1.0f}, {1.0f, -1.0f}};
            for(int i = 0; i < 4; i++)
            {
                const glm::vec2 c = corners[i];
                v[i] = {p->position, glm::vec4(c, p->size), p->velocity, (c + 1.0f) * 0.5f,
                        p->color,    {p->glow, fullbright}};
            }
        }
        // Batched with the one before if they share texture and blend.
        SDL_GPUTexture* texture =
            p->texture && *p->texture ? p->texture->get() : particleImage_.get();
        const bool glows = p->glow > 0.0f || (p->ribbon && p->otherGlow > 0.0f);
        if(!particleBatches_.empty() && particleBatches_.back().texture == texture &&
           particleBatches_.back().blendSource == p->blendSource &&
           particleBatches_.back().blendDest == p->blendDest)
        {
            particleBatches_.back().indexCount += 6;
            particleBatches_.back().glows = particleBatches_.back().glows || glows;
        }
        else
        {
            particleBatches_.push_back({texture, p->blendSource, p->blendDest, quad * 6, 6, glows});
        }
        quad++;
    }
    SDL_UnmapGPUTransferBuffer(device_, particleVertices_.transfer);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    const SDL_GPUTransferBufferLocation from = {particleVertices_.transfer, 0};
    const SDL_GPUBufferRegion to = {particleVertices_.buffer.get(), 0, bytes};
    SDL_UploadToGPUBuffer(copy, &from, &to, true);
    SDL_EndGPUCopyPass(copy);
}

void Renderer::drawParticles(ScenePass& pass)
{
    if(particleBatches_.empty() || !particleIndices_)
    {
        return;
    }
    const Pipelines& p = *pipelines_;
    SDL_GPURenderPass* rp = pass.pass();
    const SDL_GPUBufferBinding vertices = {particleVertices_.buffer.get(), 0};
    SDL_BindGPUVertexBuffers(rp, 0, &vertices, 1);
    const SDL_GPUBufferBinding indices = {particleIndices_.get(), 0};
    SDL_BindGPUIndexBuffer(rp, &indices, SDL_GPU_INDEXELEMENTSIZE_16BIT);
    pass.forgetBuffers();
    SDL_GPUGraphicsPipeline* bound = nullptr;
    for(const ParticleBatch& b : particleBatches_)
    {
        SDL_GPUGraphicsPipeline* pipeline = particlePipeline(b.blendSource, b.blendDest);
        if(!pipeline)
        {
            continue;
        }
        if(pipeline != bound)
        {
            bound = pipeline;
            pass.bindPipeline(pipeline);
            SDL_GPUBuffer* storage[] = {pass.lights(), pass.clusters()};
            SDL_BindGPUFragmentStorageBuffers(rp, 0, storage, 2);
        }
        const SDL_GPUTextureSamplerBinding samplers[] = {
            {b.texture, p.clampSampler},
            {shadowMap_, p.shadowSampler},
            {projectors_, p.clampSampler},
            waterMaskBinding_,
        };
        pass.bindSamplers(samplers);
        SDL_DrawGPUIndexedPrimitives(rp, b.indexCount, 1, b.firstIndex, 0, 0);
    }
}

void Renderer::drawParticleGlow(SDL_GPURenderPass* pass)
{
    Pipelines& p = *pipelines_;
    if(!particleIndices_ || std::ranges::none_of(particleBatches_, &ParticleBatch::glows))
    {
        return;
    }
    if(!p.particleGlow && p.particleVert && p.particleGlowFrag)
    {
        // Added up, as the faces' glow.
        SDL_GPUColorTargetDescription color = {};
        color.format = kGlowMaskFormat;
        color.blend_state.enable_blend = true;
        color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        const SDL_GPUGraphicsPipelineCreateInfo info =
            particlePipelineInfo(p.particleVert, p.particleGlowFrag, &color, depthFormat_);
        p.particleGlow = SDL_CreateGPUGraphicsPipeline(device_, &info);
        if(!p.particleGlow)
        {
            log::error("creating the particle glow pipeline failed: {}", SDL_GetError());
            SDL_ReleaseGPUShader(device_, p.particleGlowFrag);
            p.particleGlowFrag = nullptr; // not tried again
        }
    }
    if(!p.particleGlow)
    {
        return;
    }
    SDL_BindGPUGraphicsPipeline(pass, p.particleGlow);
    const SDL_GPUBufferBinding vertices = {particleVertices_.buffer.get(), 0};
    SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
    const SDL_GPUBufferBinding indices = {particleIndices_.get(), 0};
    SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_16BIT);
    for(const ParticleBatch& b : particleBatches_)
    {
        if(b.glows)
        {
            const SDL_GPUTextureSamplerBinding sampler = {b.texture, p.clampSampler};
            SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);
            SDL_DrawGPUIndexedPrimitives(pass, b.indexCount, 1, b.firstIndex, 0, 0);
        }
    }
}

} // namespace viewer::render
