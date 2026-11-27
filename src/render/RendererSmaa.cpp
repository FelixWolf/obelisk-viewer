// SMAA (1x, after tonemapping; smaa.glsl): its lookup textures and
// pipelines, made when it's first chosen, and its three passes from the
// tonemapped view (aaSource_) into the target.

#include "RendererPasses.hpp"
#include "RendererPipelines.hpp"
#include "viewer/render/Renderer.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/render/ShaderBytecode.hpp"

#include <SDL3/SDL_error.h>

#include <glm/vec4.hpp>

// Upstream's precomputed textures: areaTexBytes (R8G8) and searchTexBytes (R8).
#include <AreaTex.h>
#include <SearchTex.h>

#include <cstdint>

namespace viewer::render
{

namespace
{

struct SmaaUniforms
{
    glm::vec4 metrics;  // 1 / width, 1 / height, width, height
    glm::vec4 settings; // threshold, search steps, diagonal steps, corner rounding (%)
};

// By strength: SMAA_PRESET_MEDIUM's search without diagonals (0 steps:
// none found), then SMAA_PRESET_HIGH and SMAA_PRESET_ULTRA.
constexpr glm::vec4 kSmaaPresets[] = {
    {0.1f, 8.0f, 0.0f, 25.0f},
    {0.1f, 16.0f, 8.0f, 25.0f},
    {0.05f, 32.0f, 16.0f, 25.0f},
};

// Upstream's one or two channels a texel, as RGBA8 (the rest unused).
image::TextureData expand(const unsigned char* bytes, uint32_t width, uint32_t height,
                          uint32_t channels)
{
    image::MipLevel level{width, height, std::vector<uint8_t>(size_t(width) * height * 4)};
    for(size_t i = 0; i < size_t(width) * height; i++)
    {
        level.pixels[i * 4] = bytes[i * channels];
        level.pixels[i * 4 + 1] = channels > 1 ? bytes[i * channels + 1] : 0;
        level.pixels[i * 4 + 3] = 255;
    }
    image::TextureData texture;
    texture.levels.push_back(std::move(level));
    return texture;
}

} // namespace

bool Renderer::createSmaa()
{
    Pipelines& p = *pipelines_;
    if(!smaaArea_ || !smaaSearch_)
    {
        const image::TextureData area = expand(areaTexBytes, AREATEX_WIDTH, AREATEX_HEIGHT, 2);
        const image::TextureData search =
            expand(searchTexBytes, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1);
        const image::TextureData* both[] = {&area, &search};
        auto textures = uploadTextures(both, false);
        smaaArea_ = std::move(textures[0]);
        smaaSearch_ = std::move(textures[1]);
        if(!smaaArea_ || !smaaSearch_)
        {
            log::error("uploading SMAA's textures failed");
            return false;
        }
    }

    using namespace shaders;
    // Vertex shaders: one uniform block; fragment shaders: their samplers and one.
    auto pipeline = [&](std::span<const uint8_t> vertCode, std::span<const uint8_t> fragCode,
                        uint32_t samplers, SDL_GPUTextureFormat format, const char* name)
    {
        SDL_GPUShader* vert =
            createShader(device_, {vertCode, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1}, name);
        SDL_GPUShader* frag =
            createShader(device_, {fragCode, SDL_GPU_SHADERSTAGE_FRAGMENT, samplers, 1}, name);
        SDL_GPUGraphicsPipeline* made = nullptr;
        if(vert && frag)
        {
            SDL_GPUColorTargetDescription color = {};
            color.format = format;
            SDL_GPUGraphicsPipelineCreateInfo info = {};
            info.vertex_shader = vert;
            info.fragment_shader = frag;
            info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
            info.target_info.color_target_descriptions = &color;
            info.target_info.num_color_targets = 1;
            made = SDL_CreateGPUGraphicsPipeline(device_, &info);
            if(!made)
            {
                log::error("creating the {} pipeline failed: {}", name, SDL_GetError());
            }
        }
        for(SDL_GPUShader* s : {vert, frag})
        {
            if(s)
            {
                SDL_ReleaseGPUShader(device_, s);
            }
        }
        return made;
    };
    if(!p.smaaEdges)
    {
        p.smaaEdges = pipeline(kSmaaEdgesVert, kSmaaEdgesFrag, 1, SDL_GPU_TEXTUREFORMAT_R8G8_UNORM,
                               "SMAA edges");
    }
    if(!p.smaaWeights)
    {
        p.smaaWeights = pipeline(kSmaaWeightsVert, kSmaaWeightsFrag, 3,
                                 SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, "SMAA weights");
    }
    if(!p.smaaBlend)
    {
        p.smaaBlend =
            pipeline(kSmaaBlendVert, kSmaaBlendFrag, 2, swapchainFormat(), "SMAA blending");
    }
    return p.smaaEdges && p.smaaWeights && p.smaaBlend;
}

void Renderer::drawSmaa(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target)
{
    const Pipelines& p = *pipelines_;
    const auto w = static_cast<float>(targetWidth_);
    const auto h = static_cast<float>(targetHeight_);
    const SmaaUniforms uniforms{glm::vec4(1.0f / w, 1.0f / h, w, h),
                                kSmaaPresets[postAaStrength_ - 1]};

    auto pass = [&](SDL_GPUTexture* into, bool cleared, SDL_GPUGraphicsPipeline* pipeline,
                    std::span<const SDL_GPUTextureSamplerBinding> inputs)
    {
        SDL_GPUColorTargetInfo color = {};
        color.texture = into;
        // Edges and weights: none where the shaders discard or write nothing.
        color.load_op = cleared ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_DONT_CARE;
        color.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};
        color.store_op = SDL_GPU_STOREOP_STORE;
        color.cycle = cleared; // ours: each view's, and the screenshot's, its own
        SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(commands, &color, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(rp, pipeline);
        SDL_BindGPUFragmentSamplers(rp, 0, inputs.data(), static_cast<Uint32>(inputs.size()));
        pushVertexUniforms(commands, 0, uniforms);
        pushFragmentUniforms(commands, 0, uniforms);
        SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
        SDL_EndGPURenderPass(rp);
    };

    const SDL_GPUTextureSamplerBinding edgeInputs[] = {{aaSource_, p.clampSampler}};
    pass(smaaEdges_, true, p.smaaEdges, edgeInputs);
    const SDL_GPUTextureSamplerBinding weightInputs[] = {{smaaEdges_, p.clampSampler},
                                                         {smaaArea_.get(), p.clampSampler},
                                                         {smaaSearch_.get(), p.pointSampler}};
    pass(smaaWeights_, true, p.smaaWeights, weightInputs);
    const SDL_GPUTextureSamplerBinding blendInputs[] = {{aaSource_, p.clampSampler},
                                                        {smaaWeights_, p.clampSampler}};
    pass(target, false, p.smaaBlend, blendInputs); // every pixel written
}

} // namespace viewer::render
