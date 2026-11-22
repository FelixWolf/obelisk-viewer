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

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace viewer::render
{

namespace
{

// One local probe as the shaders see it (probes.glsl ReflectionProbe, std430).
struct ProbeData
{
    glm::vec4 sphere; // xyz origin, w radius
    glm::vec4 params; // x ambiance, y fade in, z cube, w kind (0 automatic, 1 manual sphere, 2 box)
    glm::mat4 box;
};

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

void Renderer::updateProbes(SDL_GPUCommandBuffer* commands, const Scene& scene, FrameFS& frameFs,
                            bool drawsReady)
{
    const Lighting& light = scene.lighting;
    const bool enabled = scene.reflections.enabled;
    frameFs.probeInfo = glm::vec4(light.probeAmbiance, static_cast<float>(kProbeLevels - 1),
                                  enabled && probeCubes_[0].complete ? 1.0f : 0.0f,
                                  light.probeAmbiance > 0.0f ? 0.0f : 1.0f);
    frameFs.lightInfo.y = 0.0f;
    probesShaded_ = 0;
    if(!enabled)
    {
        capture_.face = -1;
        return;
    }
    const uint64_t now = SDL_GetTicksNS();
    const glm::vec3 camera = scene.camera.position;

    // The local probes nearest the camera (by how far outside them it is)
    // get cubes; the rest wait.
    std::vector<std::pair<float, const ReflectionProbe*>> nearest;
    nearest.reserve(scene.probes.size());
    for(const ReflectionProbe& probe : scene.probes)
    {
        nearest.emplace_back(std::max(glm::length(probe.origin - camera) - probe.radius, 0.0f),
                             &probe);
    }
    const size_t chosen = std::min<size_t>(nearest.size(), kProbeCount - 1);
    std::partial_sort(nearest.begin(), nearest.begin() + static_cast<ptrdiff_t>(chosen),
                      nearest.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
    nearest.resize(chosen);
    std::unordered_map<uint64_t, const ReflectionProbe*> wanted;
    for(const auto& [distance, probe] : nearest)
    {
        wanted.emplace(probe->id, probe);
    }
    // Cubes of probes no longer wanted are freed (unless being captured).
    std::unordered_map<uint64_t, uint32_t> cubeOf;
    for(uint32_t c = 1; c < kProbeCount; c++)
    {
        ProbeCube& cube = probeCubes_[c];
        if(!cube.used)
        {
            continue;
        }
        if(!wanted.contains(cube.id) && !(capture_.face >= 0 && capture_.cube == c))
        {
            cube = ProbeCube{};
            continue;
        }
        cubeOf.emplace(cube.id, c);
    }
    uint32_t free = 1;
    for(const auto& [distance, probe] : nearest)
    {
        if(cubeOf.contains(probe->id))
        {
            continue;
        }
        while(free < kProbeCount && probeCubes_[free].used)
        {
            free++;
        }
        if(free == kProbeCount)
        {
            break;
        }
        probeCubes_[free] = ProbeCube{probe->id, true};
        cubeOf.emplace(probe->id, free);
    }

    // Those with a capture to show, nearest first, go to the shaders.
    std::vector<ProbeData> data;
    data.reserve(kProbesShaded);
    clusterProbes_.clear();
    for(const auto& [distance, probe] : nearest)
    {
        const auto c = cubeOf.find(probe->id);
        if(c == cubeOf.end() || !probeCubes_[c->second].complete)
        {
            continue;
        }
        const ProbeCube& cube = probeCubes_[c->second];
        ProbeData d;
        d.sphere = glm::vec4(probe->origin, probe->radius);
        const float fade = std::clamp(
            static_cast<float>(now - std::min(now, cube.completedTicks)) * 1e-9f, 0.0f, 1.0f);
        const float kind = probe->box ? 2.0f : probe->manual ? 1.0f : 0.0f;
        d.params = glm::vec4(std::max(light.probeAmbiance, probe->ambiance), fade,
                             static_cast<float>(c->second), kind);
        d.box = probe->box.value_or(glm::mat4(1.0f));
        data.push_back(d);
        // Where it reaches, for clusters: its sphere, or around its box
        // (`box` maps the scene to a cube from -1 to 1).
        ClusterSphere reach{probe->origin, probe->radius};
        if(probe->box)
        {
            const glm::mat4 toScene = glm::inverse(*probe->box);
            reach.center = glm::vec3(toScene * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
            reach.radius = 0.0f;
            for(int i = 0; i < 8; i++)
            {
                const glm::vec3 corner(toScene * glm::vec4((i & 1) ? 1.0f : -1.0f,
                                                           (i & 2) ? 1.0f : -1.0f,
                                                           (i & 4) ? 1.0f : -1.0f, 1.0f));
                reach.radius = std::max(reach.radius, glm::length(corner - reach.center));
            }
        }
        clusterProbes_.push_back(reach);
        if(data.size() == kProbesShaded)
        {
            break;
        }
    }
    if(reserve(probeBuffer_,
               static_cast<uint32_t>(sizeof(ProbeData) * std::max<size_t>(data.size(), 1)),
               SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ))
    {
        if(!data.empty())
        {
            auto* out = static_cast<ProbeData*>(
                SDL_MapGPUTransferBuffer(device_, probeBuffer_.transfer, true));
            std::copy(data.begin(), data.end(), out);
            SDL_UnmapGPUTransferBuffer(device_, probeBuffer_.transfer);
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
            const SDL_GPUTransferBufferLocation source = {probeBuffer_.transfer, 0};
            const SDL_GPUBufferRegion destination = {
                probeBuffer_.buffer.get(), 0,
                static_cast<uint32_t>(data.size() * sizeof(ProbeData))};
            SDL_UploadToGPUBuffer(copy, &source, &destination, true);
            SDL_EndGPUCopyPass(copy);
            probesShaded_ = static_cast<uint32_t>(data.size());
        }
    }
    frameFs.lightInfo.y = static_cast<float>(probesShaded_);

    // This frame's face (announced last frame, so the scene lists what's in
    // it: nextProbeFace).
    if(capture_.face >= 0)
    {
        captureProbeFace(commands, scene, frameFs, drawsReady);
    }

    // What to capture next: the default probe when it's due, else the
    // nearest local probe without a capture, else the one captured longest
    // ago (once it's old enough). It starts next frame.
    const float farClip = scene.reflections.drawDistance;
    if(capture_.face < 0)
    {
        const ProbeCube& sky = probeCubes_[0];
        const auto period = static_cast<uint64_t>(scene.reflections.updateSeconds * 1e9);
        const auto refresh = static_cast<uint64_t>(scene.reflections.refreshSeconds * 1e9);
        // (Captures stamp a time later than `now`.)
        if(!sky.complete || sky.updatedTicks + period <= now)
        {
            capture_ = ProbeCapture{0,
                                    0,
                                    camera + glm::vec3(0.0f, 0.0f, scene.reflections.aboveCamera),
                                    scene.reflections.nearClip,
                                    farClip,
                                    false};
        }
        else
        {
            const ReflectionProbe* next = nullptr;
            uint64_t oldest = UINT64_MAX;
            for(const auto& [distance, probe] : nearest)
            {
                const auto c = cubeOf.find(probe->id);
                if(c == cubeOf.end())
                {
                    continue;
                }
                const ProbeCube& cube = probeCubes_[c->second];
                if(!cube.complete)
                {
                    next = probe;
                    break;
                }
                if(cube.updatedTicks + refresh <= now && cube.updatedTicks < oldest)
                {
                    oldest = cube.updatedTicks;
                    next = probe;
                }
            }
            if(next)
            {
                capture_ = ProbeCapture{
                    0, cubeOf.at(next->id), next->origin, next->nearClip, farClip, true};
            }
        }
    }
}

std::optional<Frustum> Renderer::nextProbeFace() const
{
    if(capture_.face < 0 || !capture_.objects)
    {
        return std::nullopt;
    }
    return Frustum::fromViewProjection(
        glm::perspective(glm::half_pi<float>(), 1.0f, capture_.nearClip, capture_.farClip) *
        faceView(capture_.face, capture_.origin));
}

uint32_t Renderer::probesCaptured() const
{
    return static_cast<uint32_t>(
        std::ranges::count_if(probeCubes_, [](const ProbeCube& c) { return c.complete; }));
}

void Renderer::captureProbeFace(SDL_GPUCommandBuffer* commands, const Scene& scene,
                                const FrameFS& frameFs, bool drawsReady)
{
    // This face, lit as the view but from the probe's origin, and with the
    // sky's ambient light, not the probes': lit by what it captures, each
    // capture would come out brighter than the last (the official viewer's
    // irradiance pass does the same, ambiance 0).
    FrameFS fs = frameFs;
    fs.cameraPosition = glm::vec4(capture_.origin, 1.0f);
    fs.probeInfo.w = 1.0f;
    if(capture_.irradiance)
    {
        // The irradiance pass (llsettingsvo.cpp draw_color, applyToShader;
        // lldrawpoolwlsky.cpp): the sky's blue horizon and density made the
        // gray of their HSL lightness, no ambient light (direct light only)
        // and no clouds.
        auto lightness = [](glm::vec4 c)
        {
            const float l = 0.5f * (std::max({c.x, c.y, c.z}) + std::min({c.x, c.y, c.z}));
            return glm::vec4(l, l, l, c.w);
        };
        fs.blueHorizon = lightness(fs.blueHorizon);
        fs.blueDensity = lightness(fs.blueDensity);
        fs.atmoAmbient = glm::vec4(0.0f, 0.0f, 0.0f, fs.atmoAmbient.w);
        fs.skyAmbient = glm::vec4(0.0f, 0.0f, 0.0f, fs.skyAmbient.w);
        fs.groundAmbient = glm::vec4(0.0f, 0.0f, 0.0f, fs.groundAmbient.w);
        fs.cloudPos2.w = 0.0f;
    }
    const glm::mat4 view = faceView(capture_.face, capture_.origin);
    const glm::mat4 projection =
        glm::perspective(glm::half_pi<float>(), 1.0f, capture_.nearClip, capture_.farClip);
    const FrameVS vs{projection * view, glm::vec4(capture_.origin, 1.0f)};

    // Objects in the face (local probes): opaque first, then blended. Not
    // skinned ones: avatars and what they wear move (the official viewer
    // leaves them out unless a probe is set to be dynamic).
    std::vector<const ObjectDraw*> order;
    if(capture_.objects && drawsReady)
    {
        const Frustum frustum = Frustum::fromViewProjection(vs.viewProjection);
        std::vector<const ObjectDraw*> blended;
        for(const ObjectDraw& draw : scene.objects)
        {
            if(!draw.mesh || !*draw.mesh || draw.mesh->layout != VertexLayout::Object || draw.hud ||
               draw.pickOnly)
            {
                continue;
            }
            const glm::vec3 localCenter = (draw.mesh->boundsMin + draw.mesh->boundsMax) * 0.5f;
            const glm::mat3 m(draw.transform);
            const glm::mat3 absolute(glm::abs(m[0]), glm::abs(m[1]), glm::abs(m[2]));
            const glm::vec3 size = absolute * (draw.mesh->boundsMax - draw.mesh->boundsMin);
            if(!frustum.containsSphere(glm::vec3(draw.transform * glm::vec4(localCenter, 1.0f)),
                                       0.5f * glm::length(size)))
            {
                continue;
            }
            const auto* bp = std::get_if<BlinnPhongMaterial>(&draw.material);
            const auto* pbr = std::get_if<PbrMaterial>(&draw.material);
            ((bp && bp->blend) || (pbr && pbr->blend) ? blended : order).push_back(&draw);
        }
        order.insert(order.end(), blended.begin(), blended.end());
    }

    instanceOrder_.clear();
    for(const ObjectDraw* draw : order)
    {
        instanceOrder_.push_back(static_cast<uint32_t>(draw - scene.objects.data()));
    }
    const auto instanceBase = pushInstances(commands, instanceOrder_);

    const Lighting& light = scene.lighting;
    SDL_GPUColorTargetInfo color = {};
    color.texture = probeCapture_;
    color.layer_or_depth_plane = static_cast<Uint32>(capture_.face);
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
    // No clusters here (a cube face, not the view): every light and probe.
    ScenePass scenePass(commands, pass, *pipelines_, vs, fs, storageOr(lightBuffer_),
                        storageOr(probeBuffer_), emptyStorage_.get(), true);
    if(light.atmosphere.enabled)
    {
        drawSky(scenePass, light.atmosphere, view, projection);
    }
    drawTerrain(scenePass, scene);
    drawObjects(scenePass, scene, order, instanceBase);
    SDL_EndGPURenderPass(pass);

    if(++capture_.face == 6 && !capture_.irradiance)
    {
        // Reflections done: the same cube again for its ambient light.
        filterProbe(commands, capture_.cube, false);
        capture_.irradiance = true;
        capture_.face = 0;
    }
    else if(capture_.face == 6)
    {
        filterProbe(commands, capture_.cube, true);
        ProbeCube& cube = probeCubes_[capture_.cube];
        const uint64_t now = SDL_GetTicksNS();
        if(capture_.cube == 0)
        {
            cube.used = true;
        }
        if(!cube.complete)
        {
            cube.completedTicks = now;
        }
        cube.complete = true;
        cube.updatedTicks = now;
        capture_.face = -1;
    }
}

void Renderer::filterProbe(SDL_GPUCommandBuffer* commands, uint32_t cube, bool irradiance)
{
    // Lower levels of the capture: what the filters read for wide lobes.
    SDL_GenerateMipmapsForGPUTexture(commands, probeCapture_);

    const Pipelines& p = *pipelines_;
    const SDL_GPUTextureSamplerBinding capture = {probeCapture_, p.clampSampler};
    constexpr float kLastLevel = static_cast<float>(kProbeLevels - 1);
    auto filterFace = [&](SDL_GPUGraphicsPipeline* pipeline, SDL_GPUTexture* target, uint32_t level,
                          int face, float roughness)
    {
        SDL_GPUColorTargetInfo color = {};
        color.texture = target;
        color.mip_level = level;
        color.layer_or_depth_plane = cube * 6 + static_cast<Uint32>(face);
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
    if(irradiance)
    {
        for(int face = 0; face < 6; face++)
        {
            filterFace(p.probeIrradiance, probeIrradiance_, 0, face, 0.0f);
        }
        return;
    }
    // Radiance: level n stands for roughness n / last level.
    for(uint32_t level = 0; level < kProbeLevels; level++)
    {
        for(int face = 0; face < 6; face++)
        {
            filterFace(p.probeRadiance, probeRadiance_, level, face,
                       static_cast<float>(level) / kLastLevel);
        }
    }
}

} // namespace viewer::render
