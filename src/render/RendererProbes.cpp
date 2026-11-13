// Reflection probes: capturing a cube of the surroundings a face at a time,
// and filtering it into radiance (a GGX-blurred mip chain) and irradiance,
// as the official viewer's LLReflectionMapManager does for its default
// probe (sky and terrain around the camera).

#include "RendererPasses.hpp"
#include "RendererPipelines.hpp"
#include "viewer/render/Renderer.hpp"

#include "viewer/core/Log.hpp"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_timer.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace viewer::render
{

namespace
{

struct FilterFS
{
    glm::vec4 params; // x face, y roughness, z the capture's size, w its last mip level
};

// The view of cube face `face` from `origin`. Each face's right, up and
// forward follow the cube map sampling rules (s across, t down the face);
// the world is mirrored in y, which turns the faces' mirror-image layout
// back into the camera's winding, so the scene pipelines (back faces
// culled) draw them as they are. probes.glsl looks directions up mirrored.
glm::mat4 faceView(int face, const glm::vec3& origin)
{
    static const glm::vec3 kAxes[6][3] = {
        {{0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}},  // +X
        {{0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}},  // -X
        {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}},  // +Y
        {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, -1.0f, 0.0f}},  // -Y
        {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},   // +Z
        {{-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}}, // -Z
    };
    const glm::vec3 mirror(1.0f, -1.0f, 1.0f);
    const glm::vec3 right = kAxes[face][0] * mirror;
    const glm::vec3 up = kAxes[face][1] * mirror;
    const glm::vec3 forward = kAxes[face][2] * mirror;
    // Rows right, up and -forward (looking down -z, as glm::perspective
    // expects), after moving the origin to the eye.
    glm::mat4 view(1.0f);
    for(int c = 0; c < 3; c++)
    {
        view[c][0] = right[c];
        view[c][1] = up[c];
        view[c][2] = -forward[c];
    }
    view[3] =
        glm::vec4(-glm::dot(right, origin), -glm::dot(up, origin), glm::dot(forward, origin), 1.0f);
    return view;
}

} // namespace

bool Renderer::createProbes()
{
    constexpr SDL_GPUTextureUsageFlags kUsage =
        SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    if(!SDL_GPUTextureSupportsFormat(device_, kHdrFormat, SDL_GPU_TEXTURETYPE_CUBE_ARRAY, kUsage))
    {
        log::error("reflection probes need cube array textures, which this GPU lacks");
        return false;
    }
    SDL_GPUTextureCreateInfo info = {};
    info.format = kHdrFormat;
    info.usage = kUsage;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;

    info.type = SDL_GPU_TEXTURETYPE_CUBE_ARRAY;
    info.width = info.height = kProbeSize;
    info.layer_count_or_depth = 6 * kProbeCount;
    info.num_levels = kProbeLevels;
    probeRadiance_ = SDL_CreateGPUTexture(device_, &info);

    info.width = info.height = kIrradianceSize;
    info.num_levels = 1;
    probeIrradiance_ = SDL_CreateGPUTexture(device_, &info);

    info.type = SDL_GPU_TEXTURETYPE_CUBE;
    info.width = info.height = kProbeSize;
    info.layer_count_or_depth = 6;
    info.num_levels = kProbeLevels;
    probeCapture_ = SDL_CreateGPUTexture(device_, &info);

    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.format = depthFormat_;
    info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    probeDepth_ = SDL_CreateGPUTexture(device_, &info);

    info.format = SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
    info.usage = kUsage;
    info.width = info.height = kBrdfTableSize;
    brdfTable_ = SDL_CreateGPUTexture(device_, &info);

    if(!probeRadiance_ || !probeIrradiance_ || !probeCapture_ || !probeDepth_ || !brdfTable_)
    {
        log::error("creating the reflection probe textures failed: {}", SDL_GetError());
        return false;
    }

    // The split-sum table, once.
    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_);
    if(!commands)
    {
        log::error("making the BRDF table failed: {}", SDL_GetError());
        return false;
    }
    SDL_GPUColorTargetInfo color = {};
    color.texture = brdfTable_;
    color.load_op = SDL_GPU_LOADOP_DONT_CARE;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, pipelines_->brdfTable);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    SDL_SubmitGPUCommandBuffer(commands);
    return true;
}

std::array<SDL_GPUTextureSamplerBinding, 3> Renderer::probeSamplers() const
{
    SDL_GPUSampler* clamp = pipelines_->clampSampler;
    return {{{probeRadiance_, clamp}, {probeIrradiance_, clamp}, {brdfTable_, clamp}}};
}

void Renderer::updateProbes(SDL_GPUCommandBuffer* commands, const Scene& scene,
                            const FrameFS& frameFs)
{
    if(!scene.reflections.enabled)
    {
        probe_.face = -1;
        return;
    }
    const uint64_t now = SDL_GetTicksNS();
    if(probe_.face < 0)
    {
        const auto period = static_cast<uint64_t>(scene.reflections.updateSeconds * 1e9);
        if(probe_.ready && now - probe_.startedTicks < period)
        {
            return;
        }
        probe_.face = 0;
        probe_.origin = scene.camera.position;
        probe_.startedTicks = now;
    }

    // This face: the sky and terrain from the probe's origin, lit as the
    // view but without probes (they're what's being made).
    FrameFS fs = frameFs;
    fs.cameraPosition = glm::vec4(probe_.origin, 1.0f);
    fs.probeInfo = glm::vec4(0.0f);
    const glm::mat4 view = faceView(probe_.face, probe_.origin);
    const glm::mat4 projection = glm::perspective(
        glm::half_pi<float>(), 1.0f, scene.reflections.nearClip, scene.camera.farPlane);
    const FrameVS vs{projection * view, glm::vec4(probe_.origin, 1.0f)};

    const Lighting& light = scene.lighting;
    SDL_GPUColorTargetInfo color = {};
    color.texture = probeCapture_;
    color.layer_or_depth_plane = static_cast<Uint32>(probe_.face);
    color.clear_color = SDL_FColor{light.skyColor.r, light.skyColor.g, light.skyColor.b, 1.0f};
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo depth = {};
    depth.texture = probeDepth_;
    depth.clear_depth = 1.0f;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color, 1, &depth);
    ScenePass scenePass(commands, pass, *pipelines_, vs, fs, lightBuffer_.buffer.get(), true);
    if(light.atmosphere.enabled)
    {
        drawSky(scenePass, light.atmosphere, view, projection);
    }
    drawTerrain(scenePass, scene);
    SDL_EndGPURenderPass(pass);

    if(++probe_.face == 6)
    {
        filterProbe(commands);
        probe_.face = -1;
        probe_.ready = true;
    }
}

void Renderer::filterProbe(SDL_GPUCommandBuffer* commands)
{
    // Lower levels of the capture: what the filters read for wide lobes.
    SDL_GenerateMipmapsForGPUTexture(commands, probeCapture_);

    const Pipelines& p = *pipelines_;
    const SDL_GPUTextureSamplerBinding capture = {probeCapture_, p.clampSampler};
    constexpr float kLastLevel = static_cast<float>(kProbeLevels - 1);
    constexpr uint32_t kProbe = 0; // the default probe's cube in the arrays
    auto filterFace = [&](SDL_GPUGraphicsPipeline* pipeline, SDL_GPUTexture* target, uint32_t level,
                          int face, float roughness)
    {
        SDL_GPUColorTargetInfo color = {};
        color.texture = target;
        color.mip_level = level;
        color.layer_or_depth_plane = kProbe * 6 + static_cast<Uint32>(face);
        color.load_op = SDL_GPU_LOADOP_DONT_CARE;
        color.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(pass, pipeline);
        SDL_BindGPUFragmentSamplers(pass, 0, &capture, 1);
        pushFragmentUniforms(commands, 0,
                             FilterFS{glm::vec4(static_cast<float>(face), roughness,
                                                static_cast<float>(kProbeSize), kLastLevel)});
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    };
    // Radiance: level n stands for roughness n / last level.
    for(uint32_t level = 0; level < kProbeLevels; level++)
    {
        for(int face = 0; face < 6; face++)
        {
            filterFace(p.probeRadiance, probeRadiance_, level, face,
                       static_cast<float>(level) / kLastLevel);
        }
    }
    for(int face = 0; face < 6; face++)
    {
        filterFace(p.probeIrradiance, probeIrradiance_, 0, face, 0.0f);
    }
}

} // namespace viewer::render
