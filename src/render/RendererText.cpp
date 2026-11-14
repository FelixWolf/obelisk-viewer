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
// Floating text: lines no wider than this (HUD_TEXT_MAX_WIDTH_NO_BUBBLE),
// the last baseline this far above the point (VERTICAL_PADDING).
constexpr float kTextMaxWidth = 1000.0f;
constexpr float kTextBaseline = 12.0f;
// Name tags (llhudnametag.cpp): lines no wider than NAMETAG_MAX_WIDTH, this
// far apart (LINE_PADDING), in a bubble this much wider and taller than
// they are (HORIZONTAL_PADDING, VERTICAL_PADDING) with corners this round.
constexpr float kTagMaxWidth = 298.0f;
constexpr float kTagLineGap = 3.0f;
constexpr float kTagPaddingX = 16.0f;
constexpr float kTagPaddingY = 12.0f;
constexpr float kTagCorner = 6.0f;
// Chat in a name tag: lines no wider than HUD_TEXT_MAX_WIDTH, this far in
// from the bubble's left (HORIZONTAL_PADDING / 2, and one); the header band
// this much taller than its lines (VERTICAL_PADDING / 3).
constexpr float kChatMaxWidth = 190.0f;
constexpr float kChatIndent = 9.0f;
constexpr float kHeaderPadding = 4.0f;

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
        if(label.lines.empty())
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

    // Quads (bubbles, then for each glyph a shadow quad and its own), from
    // pixels to clip space. Should the atlas fill up, it's cleared and every
    // label laid out again.
    std::vector<LabelVertex> vertices;
    auto quad = [&](glm::vec2 topLeft, glm::vec2 extent, glm::vec2 uv0, glm::vec2 uv1,
                    uint32_t color, float depth)
    {
        const glm::vec2 a = topLeft / size * 2.0f - 1.0f;
        const glm::vec2 b = (topLeft + extent) / size * 2.0f - 1.0f;
        // Clip space y is up.
        const LabelVertex v00{{a.x, -a.y, depth}, uv0, color};
        const LabelVertex v10{{b.x, -a.y, depth}, {uv1.x, uv0.y}, color};
        const LabelVertex v01{{a.x, -b.y, depth}, {uv0.x, uv1.y}, color};
        const LabelVertex v11{{b.x, -b.y, depth}, uv1, color};
        vertices.insert(vertices.end(), {v00, v10, v11, v00, v11, v01});
    };
    // A rounded box: the disc's quarters at the corners (the bottom ones
    // square if not `roundBottom`), its middle texel stretched over the rest.
    auto box =
        [&](glm::vec2 topLeft, glm::vec2 extent, uint32_t color, float depth, bool roundBottom)
    {
        const text::Glyph* disc = glyphs_->disc(kTagCorner);
        if(!disc)
        {
            return false;
        }
        const float texel = 1.0f / static_cast<float>(kAtlasSize);
        const float c = std::min({std::floor(disc->size.x * 0.5f), std::floor(extent.x * 0.5f),
                                  std::floor(extent.y * 0.5f)});
        const glm::vec2 mid = (disc->uv0 + disc->uv1) * 0.5f;
        const glm::vec2 near0 = disc->uv0;
        const glm::vec2 near1 = disc->uv0 + glm::vec2(c * texel);
        const glm::vec2 far0 = disc->uv1 - glm::vec2(c * texel);
        const glm::vec2 far1 = disc->uv1;
        const glm::vec2 corner(c);
        const glm::vec2 bottomRight = topLeft + extent;
        quad(topLeft, corner, near0, near1, color, depth);
        quad({bottomRight.x - c, topLeft.y}, corner, {far0.x, near0.y}, {far1.x, near1.y}, color,
             depth);
        if(roundBottom)
        {
            quad({topLeft.x, bottomRight.y - c}, corner, {near0.x, far0.y}, {near1.x, far1.y},
                 color, depth);
            quad(bottomRight - corner, corner, far0, far1, color, depth);
        }
        else
        {
            quad({topLeft.x, bottomRight.y - c}, corner, mid, mid, color, depth);
            quad(bottomRight - corner, corner, mid, mid, color, depth);
        }
        quad({topLeft.x + c, topLeft.y}, {extent.x - 2.0f * c, c}, mid, mid, color, depth);
        quad({topLeft.x + c, bottomRight.y - c}, {extent.x - 2.0f * c, c}, mid, mid, color, depth);
        quad({topLeft.x, topLeft.y + c}, {extent.x, extent.y - 2.0f * c}, mid, mid, color, depth);
        return true;
    };
    struct Laid
    {
        text::TextLayout layout;
        const LabelLine* line = nullptr;
    };
    std::vector<Laid> laid;
    bool complete = true;
    // A line's glyphs (each after its shadow), its block's top left at `origin`.
    auto glyphQuads = [&](const Laid& l, glm::vec2 origin, float depth)
    {
        const glm::vec4& color = l.line->color;
        const float strength = shadowStrength(color);
        const uint32_t textColor = packColor(color);
        const uint32_t shadowColor = packColor(glm::vec4(0.0f, 0.0f, 0.0f, color.a * strength));
        for(const text::PlacedGlyph& g : l.layout.glyphs)
        {
            const glm::vec2 topLeft = origin + g.position;
            if(strength > 0.0f)
            {
                quad(topLeft + glm::vec2(1.0f, 1.0f), g.size, g.uv0, g.uv1, shadowColor, depth);
            }
            quad(topLeft, g.size, g.uv0, g.uv1, textColor, depth);
        }
    };
    // Floating text: lines stacked up from the last, whose baseline is
    // kTextBaseline above the point.
    auto floatingText = [&](const Placed& p)
    {
        float baseline = p.pixel.y - kTextBaseline;
        for(auto it = laid.rbegin(); it != laid.rend(); ++it)
        {
            const text::TextLayout& layout = it->layout;
            const float top =
                baseline - layout.ascent - layout.lineHeight * static_cast<float>(layout.lines - 1);
            glyphQuads(*it, {p.pixel.x - std::floor(layout.size.x * 0.5f), top}, p.depth);
            baseline = top + layout.ascent - layout.lineHeight;
        }
    };
    // A name tag: the bubble (and its header band), then lines from the top
    // down (LLHUDNameTag::renderText): a header's baseline a line below the
    // one before, other lines' a line and the gap below.
    auto nameTag = [&](const Placed& p)
    {
        const Label& label = *p.label;
        float widest = 0.0f;
        float headers = 0.0f;
        float textHeight = 0.0f;
        for(const Laid& l : laid)
        {
            widest = std::max(widest, l.layout.size.x);
            if(l.line->header)
            {
                headers += l.layout.size.y;
                textHeight += l.layout.size.y;
            }
            else
            {
                textHeight += l.layout.size.y + kTagLineGap;
            }
        }
        if(textHeight > headers)
        {
            textHeight -= kTagLineGap; // none under the last line
        }
        const glm::vec2 extent(std::ceil(widest + kTagPaddingX),
                               std::round(textHeight + kTagPaddingY));
        const float bottom = p.pixel.y - std::round(label.raise);
        const glm::vec2 topLeft(p.pixel.x - std::floor(extent.x * 0.5f), bottom - extent.y);
        if(label.background.a > 0.0f)
        {
            complete = box(topLeft, extent, packColor(label.background), p.depth, true) && complete;
        }
        if(headers > 0.0f && label.headerBackground.a > 0.0f)
        {
            complete = box(topLeft, {extent.x, std::round(headers + kHeaderPadding)},
                           packColor(label.headerBackground), p.depth, false) &&
                       complete;
        }
        float y = topLeft.y; // the latest baseline
        for(const Laid& l : laid)
        {
            const text::TextLayout& layout = l.layout;
            const bool left = label.alignLeft && !l.line->header;
            const float x =
                left ? topLeft.x + kChatIndent : p.pixel.x - std::floor(layout.size.x * 0.5f);
            // Its first baseline (a line's textHeight includes the gap).
            y += layout.lineHeight;
            glyphQuads(l, {x, y - layout.ascent}, p.depth);
            y += layout.lineHeight * static_cast<float>(layout.lines - 1);
        }
    };
    auto build = [&]
    {
        vertices.clear();
        complete = true;
        for(const auto* list : {&world, &hud})
        {
            for(const Placed& p : *list)
            {
                const Label& label = *p.label;
                const bool tag = label.style == Label::Style::NameTag;
                laid.clear();
                for(const LabelLine& line : label.lines)
                {
                    if(line.text.empty() || line.color.a <= 0.0f)
                    {
                        continue;
                    }
                    text::LayoutOptions options;
                    options.pixelSize = line.points * (96.0f / 72.0f);
                    options.align =
                        label.alignLeft && !line.header ? text::Align::Left : text::Align::Center;
                    options.skipEmptyLines = true;
                    options.maxWidth = !tag                              ? kTextMaxWidth
                                       : label.alignLeft && !line.header ? kChatMaxWidth
                                                                         : kTagMaxWidth;
                    // Name tag lines (but headers) are a gap apart, even
                    // when one wraps.
                    options.lineGap = tag && !line.header ? kTagLineGap : 0.0f;
                    text::TextLayout layout =
                        text::layoutText(*glyphs_, *font_, line.text, options);
                    complete = complete && layout.complete;
                    if(layout.lines > 0)
                    {
                        laid.push_back({std::move(layout), &line});
                    }
                }
                if(laid.empty())
                {
                    continue;
                }
                if(tag)
                {
                    nameTag(p);
                }
                else
                {
                    floatingText(p);
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
