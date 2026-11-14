// Labels (Scene::labels): text laid out in the glyph atlas, as quads at
// whole pixels on screen, drawn after tonemapping like the official
// viewer's floating text (llhudtext.cpp): over the view and depth tested
// at the label's point, or into the HUD image.

#include "RendererPasses.hpp"
#include "RendererPipelines.hpp"
#include "viewer/render/Renderer.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/text/TextLayout.hpp"

#include <SDL3/SDL_error.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace viewer::render
{

namespace
{

constexpr uint32_t kAtlasSize = 1024;
// The official viewer's SansSerif at 10 points (96 dpi).
constexpr float kLabelPixelSize = 13.333f;
// Its lines go no wider than this (HUD_TEXT_MAX_WIDTH_NO_BUBBLE), and the
// last one's baseline is this far above the point (VERTICAL_PADDING).
constexpr float kLabelMaxWidth = 1000.0f;
constexpr float kLabelBaseline = 12.0f;

struct LabelVertex
{
    glm::vec3 position; // clip space x, y; depth
    glm::vec2 uv;
    uint32_t color; // RGBA8
};
static_assert(sizeof(LabelVertex) == 24);

uint32_t packColor(const glm::vec4& c)
{
    auto byte = [](float v)
    { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return byte(c.r) | byte(c.g) << 8 | byte(c.b) << 16 | byte(c.a) << 24;
}

// How strong a label's drop shadow is: none for dark text, full for light
// (LLFontGL::render: by HSL lightness, 0.35 to 0.6).
float shadowStrength(const glm::vec4& color)
{
    const float lightness =
        0.5f * (std::max({color.r, color.g, color.b}) + std::min({color.r, color.g, color.b}));
    return std::clamp((lightness - 0.35f) / 0.25f, 0.0f, 1.0f);
}

struct Placed
{
    const Label* label = nullptr;
    glm::vec2 pixel{0.0f}; // the point on screen (whole pixels, y down)
    float depth = 0.0f;
    float distance = 0.0f; // for drawing far ones first
};

} // namespace

void Renderer::uploadLabels(SDL_GPUCommandBuffer* commands, const Scene& scene,
                            const glm::mat4& viewProjection, uint32_t width, uint32_t height)
{
    labelVertices_ = 0;
    hudLabelVertices_ = 0;
    if(scene.labels.empty() || width == 0 || height == 0)
    {
        return;
    }
    if(!font_)
    {
        font_ = text::Font::builtin();
        if(!font_)
        {
            log::error("loading the built-in font failed");
            return;
        }
    }
    if(!glyphs_)
    {
        glyphs_ = std::make_unique<text::GlyphAtlas>(kAtlasSize);
    }
    if(!glyphTexture_)
    {
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = kAtlasSize;
        info.height = kAtlasSize;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        glyphTexture_ = SDL_CreateGPUTexture(device_, &info);
        SDL_GPUTransferBufferCreateInfo transfer = {};
        transfer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transfer.size = kAtlasSize * kAtlasSize;
        glyphTransfer_ = SDL_CreateGPUTransferBuffer(device_, &transfer);
        if(!glyphTexture_ || !glyphTransfer_)
        {
            log::error("creating the glyph atlas failed: {}", SDL_GetError());
            return;
        }
    }

    // Where each label is on screen: scene ones in front of the camera,
    // far ones first; then the HUD's.
    const glm::vec2 size(static_cast<float>(width), static_cast<float>(height));
    const glm::mat4 hudViewProjection = Renderer::hudViewProjection(scene.hud);
    std::vector<Placed> world;
    std::vector<Placed> hud;
    for(const Label& label : scene.labels)
    {
        if(label.text.empty() || label.color.a <= 0.0f)
        {
            continue;
        }
        const glm::vec4 clip =
            (label.hud ? hudViewProjection : viewProjection) * glm::vec4(label.position, 1.0f);
        if(clip.w <= 1e-6f)
        {
            continue;
        }
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if(!label.hud && (ndc.z < 0.0f || ndc.z > 1.0f))
        {
            continue;
        }
        Placed p;
        p.label = &label;
        p.pixel = glm::round(glm::vec2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f) * size);
        p.depth = label.hud ? 0.0f : ndc.z;
        p.distance = label.hud ? label.position.x : clip.w; // the HUD looks along +X
        (label.hud ? hud : world).push_back(p);
    }
    auto farFirst = [](const Placed& a, const Placed& b) { return a.distance > b.distance; };
    std::sort(world.begin(), world.end(), farFirst);
    std::sort(hud.begin(), hud.end(), farFirst);

    // Glyph quads (a shadow quad before each glyph's own), from pixels to
    // clip space. Should the atlas fill up, it's cleared and every label
    // laid out again.
    text::LayoutOptions options;
    options.pixelSize = kLabelPixelSize;
    options.maxWidth = kLabelMaxWidth;
    options.align = text::Align::Center;
    options.skipEmptyLines = true;
    std::vector<LabelVertex> vertices;
    auto build = [&]
    {
        vertices.clear();
        bool complete = true;
        for(const auto* list : {&world, &hud})
        {
            for(const Placed& p : *list)
            {
                const text::TextLayout layout =
                    text::layoutText(*glyphs_, *font_, p.label->text, options);
                complete = complete && layout.complete;
                if(layout.lines == 0)
                {
                    continue;
                }
                // Centered over the point, the last baseline kLabelBaseline above it.
                const float lastBaseline =
                    layout.ascent + layout.lineHeight * static_cast<float>(layout.lines - 1);
                const glm::vec2 origin(p.pixel.x - std::floor(layout.size.x * 0.5f),
                                       p.pixel.y - kLabelBaseline - lastBaseline);
                const glm::vec4& color = p.label->color;
                const float strength = shadowStrength(color);
                const uint32_t textColor = packColor(color);
                const uint32_t shadowColor =
                    packColor(glm::vec4(0.0f, 0.0f, 0.0f, color.a * strength));
                for(const text::PlacedGlyph& g : layout.glyphs)
                {
                    auto quad = [&](glm::vec2 topLeft, uint32_t c)
                    {
                        const glm::vec2 a = topLeft / size * 2.0f - 1.0f;
                        const glm::vec2 b = (topLeft + g.size) / size * 2.0f - 1.0f;
                        // Clip space y is up.
                        const LabelVertex v00{{a.x, -a.y, p.depth}, g.uv0, c};
                        const LabelVertex v10{{b.x, -a.y, p.depth}, {g.uv1.x, g.uv0.y}, c};
                        const LabelVertex v01{{a.x, -b.y, p.depth}, {g.uv0.x, g.uv1.y}, c};
                        const LabelVertex v11{{b.x, -b.y, p.depth}, g.uv1, c};
                        vertices.insert(vertices.end(), {v00, v10, v11, v00, v11, v01});
                    };
                    const glm::vec2 topLeft = origin + g.position;
                    if(strength > 0.0f)
                    {
                        quad(topLeft + glm::vec2(1.0f, 1.0f), shadowColor);
                    }
                    quad(topLeft, textColor);
                }
            }
            if(list == &world)
            {
                labelVertices_ = static_cast<uint32_t>(vertices.size());
            }
        }
        return complete;
    };
    if(!build())
    {
        glyphs_->clear();
        build();
    }
    hudLabelVertices_ = static_cast<uint32_t>(vertices.size()) - labelVertices_;
    if(vertices.empty())
    {
        return;
    }

    const auto bytes = static_cast<uint32_t>(vertices.size() * sizeof(LabelVertex));
    if(!reserve(labelBuffer_, bytes, SDL_GPU_BUFFERUSAGE_VERTEX))
    {
        labelVertices_ = hudLabelVertices_ = 0;
        return;
    }
    void* mapped = SDL_MapGPUTransferBuffer(device_, labelBuffer_.transfer, true);
    std::memcpy(mapped, vertices.data(), bytes);
    SDL_UnmapGPUTransferBuffer(device_, labelBuffer_.transfer);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    const SDL_GPUTransferBufferLocation source = {labelBuffer_.transfer, 0};
    const SDL_GPUBufferRegion destination = {labelBuffer_.buffer.get(), 0, bytes};
    SDL_UploadToGPUBuffer(copy, &source, &destination, true);
    // Glyphs new to the atlas: the rows they're in.
    if(const auto rows = glyphs_->takeChanged())
    {
        const uint32_t offset = rows->y * kAtlasSize;
        const uint32_t count = rows->height * kAtlasSize;
        auto* staging =
            static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device_, glyphTransfer_, true));
        std::memcpy(staging, glyphs_->pixels().data() + offset, count);
        SDL_UnmapGPUTransferBuffer(device_, glyphTransfer_);
        SDL_GPUTextureTransferInfo from = {};
        from.transfer_buffer = glyphTransfer_;
        from.pixels_per_row = kAtlasSize;
        from.rows_per_layer = rows->height;
        SDL_GPUTextureRegion to = {};
        to.texture = glyphTexture_;
        to.y = rows->y;
        to.w = kAtlasSize;
        to.h = rows->height;
        to.d = 1;
        SDL_UploadToGPUTexture(copy, &from, &to, false);
    }
    SDL_EndGPUCopyPass(copy);
}

void Renderer::drawLabels(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target, bool hud)
{
    const uint32_t count = hud ? hudLabelVertices_ : labelVertices_;
    if(count == 0 || !target || !glyphTexture_)
    {
        return;
    }
    SDL_GPUColorTargetInfo color = {};
    color.texture = target;
    color.load_op = SDL_GPU_LOADOP_LOAD;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo depth = {};
    depth.texture = depthTarget_;
    depth.load_op = SDL_GPU_LOADOP_LOAD;
    depth.store_op = SDL_GPU_STOREOP_STORE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color, 1, hud ? nullptr : &depth);
    SDL_BindGPUGraphicsPipeline(pass, hud ? pipelines_->hudLabel : pipelines_->label);
    const SDL_GPUTextureSamplerBinding glyphs = {glyphTexture_, pipelines_->clampSampler};
    SDL_BindGPUFragmentSamplers(pass, 0, &glyphs, 1);
    const SDL_GPUBufferBinding vertices = {labelBuffer_.buffer.get(), 0};
    SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
    SDL_DrawGPUPrimitives(pass, count, 1, hud ? labelVertices_ : 0, 0);
    SDL_EndGPURenderPass(pass);
}

} // namespace viewer::render
