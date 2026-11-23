#include "GuiRenderer.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/gui/GuiShaders.hpp"

#include <RmlUi/Core/DecorationTypes.h>
#include <RmlUi/Core/Dictionary.h>
#include <RmlUi/Core/Math.h>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>

namespace viewer::gui
{

std::optional<Pixels> decodeImage(std::span<const uint8_t> file)
{
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* data = stbi_load_from_memory(file.data(), static_cast<int>(file.size()), &width,
                                          &height, &channels, 4);
    if(!data)
    {
        return std::nullopt;
    }
    Pixels pixels{{width, height},
                  std::vector<uint8_t>(data, data + static_cast<size_t>(width) * height * 4)};
    stbi_image_free(data);
    for(size_t i = 0; i < pixels.rgba.size(); i += 4)
    {
        const unsigned alpha = pixels.rgba[i + 3];
        for(size_t c = 0; c < 3; c++)
        {
            pixels.rgba[i + c] = static_cast<uint8_t>((pixels.rgba[i + c] * alpha + 127) / 255);
        }
    }
    return pixels;
}

namespace
{

static_assert(sizeof(Rml::Vertex) == 20);

SDL_GPUShader* shader(SDL_GPUDevice* device, std::span<const uint8_t> code,
                      SDL_GPUShaderStage stage, uint32_t samplers, uint32_t uniforms,
                      const char* name)
{
    SDL_GPUShaderCreateInfo info = {};
    info.code_size = code.size();
    info.code = code.data();
    info.entrypoint = "main";
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    info.stage = stage;
    info.num_samplers = samplers;
    info.num_uniform_buffers = uniforms;
    SDL_GPUShader* s = SDL_CreateGPUShader(device, &info);
    if(!s)
    {
        log::error("creating shader {} failed: {}", name, SDL_GetError());
    }
    return s;
}

// Premultiplied: the source's color is already times its alpha.
SDL_GPUColorTargetDescription premultiplied(SDL_GPUTextureFormat format)
{
    SDL_GPUColorTargetDescription color = {};
    color.format = format;
    color.blend_state.enable_blend = true;
    color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    return color;
}

glm::vec4 toColor(const Rml::ColourbPremultiplied& c)
{
    return glm::vec4(c.red, c.green, c.blue, c.alpha) / 255.0f;
}

} // namespace

// The pipelines: geometry ([0] anywhere, [1] only inside the clip mask),
// writing the mask, layers onto layers ([replace][inside the mask]), the
// filters' (into their own images, no mask), and pages onto the window.
struct Renderer::Pipelines
{
    std::array<SDL_GPUGraphicsPipeline*, 2> color{};
    std::array<SDL_GPUGraphicsPipeline*, 2> texture{};
    std::array<SDL_GPUGraphicsPipeline*, 2> gradient{};
    SDL_GPUGraphicsPipeline* maskReplace = nullptr;
    SDL_GPUGraphicsPipeline* maskIncrement = nullptr;
    std::array<std::array<SDL_GPUGraphicsPipeline*, 2>, 2> composite{};
    SDL_GPUGraphicsPipeline* copy = nullptr;
    SDL_GPUGraphicsPipeline* copyBlend = nullptr;
    SDL_GPUGraphicsPipeline* matrix = nullptr;
    SDL_GPUGraphicsPipeline* blur = nullptr;
    SDL_GPUGraphicsPipeline* shadow = nullptr;
    SDL_GPUGraphicsPipeline* mask = nullptr;
    SDL_GPUGraphicsPipeline* blit = nullptr;

    std::vector<SDL_GPUGraphicsPipeline*> all() const
    {
        return {color[0],
                color[1],
                texture[0],
                texture[1],
                gradient[0],
                gradient[1],
                maskReplace,
                maskIncrement,
                composite[0][0],
                composite[0][1],
                composite[1][0],
                composite[1][1],
                copy,
                copyBlend,
                matrix,
                blur,
                shadow,
                mask,
                blit};
    }
};

Renderer::Renderer(SDL_GPUDevice* device) : device_(device) {}

Renderer::~Renderer()
{
    if(pipelines_)
    {
        for(SDL_GPUGraphicsPipeline* p : pipelines_->all())
        {
            if(p)
            {
                SDL_ReleaseGPUGraphicsPipeline(device_, p);
            }
        }
    }
    for(SDL_GPUTexture* t : layers_)
    {
        SDL_ReleaseGPUTexture(device_, t);
    }
    for(SDL_GPUTexture* t : post_)
    {
        if(t)
        {
            SDL_ReleaseGPUTexture(device_, t);
        }
    }
    if(stencil_)
    {
        SDL_ReleaseGPUTexture(device_, stencil_);
    }
    if(sampler_)
    {
        SDL_ReleaseGPUSampler(device_, sampler_);
    }
    if(vertexBuffer_)
    {
        SDL_ReleaseGPUBuffer(device_, vertexBuffer_);
    }
    if(indexBuffer_)
    {
        SDL_ReleaseGPUBuffer(device_, indexBuffer_);
    }
    if(transfer_)
    {
        SDL_ReleaseGPUTransferBuffer(device_, transfer_);
    }
}

bool Renderer::create(SDL_GPUTextureFormat windowFormat)
{
    for(const SDL_GPUTextureFormat format :
        {SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT, SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT})
    {
        if(SDL_GPUTextureSupportsFormat(device_, format, SDL_GPU_TEXTURETYPE_2D,
                                        SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
        {
            stencilFormat_ = format;
            break;
        }
    }
    if(stencilFormat_ == SDL_GPU_TEXTUREFORMAT_INVALID)
    {
        log::error("the interface needs a stencil format, and the GPU has none");
        return false;
    }

    using namespace shaders;
    const auto vertex = SDL_GPU_SHADERSTAGE_VERTEX;
    const auto fragment = SDL_GPU_SHADERSTAGE_FRAGMENT;
    SDL_GPUShader* vert = shader(device_, kGuiVert, vertex, 0, 1, "gui.vert");
    SDL_GPUShader* colorFrag = shader(device_, kGuiColorFrag, fragment, 0, 0, "gui_color.frag");
    SDL_GPUShader* textureFrag =
        shader(device_, kGuiTextureFrag, fragment, 1, 1, "gui_texture.frag");
    SDL_GPUShader* gradientFrag =
        shader(device_, kGuiGradientFrag, fragment, 0, 1, "gui_gradient.frag");
    SDL_GPUShader* blitVert = shader(device_, kGuiBlitVert, vertex, 0, 1, "gui_blit.vert");
    SDL_GPUShader* blitFrag = shader(device_, kGuiBlitFrag, fragment, 1, 0, "gui_blit.frag");
    SDL_GPUShader* quadVert = shader(device_, kGuiQuadVert, vertex, 0, 1, "gui_quad.vert");
    SDL_GPUShader* copyFrag = shader(device_, kGuiCopyFrag, fragment, 1, 1, "gui_copy.frag");
    SDL_GPUShader* matrixFrag = shader(device_, kGuiMatrixFrag, fragment, 1, 1, "gui_matrix.frag");
    SDL_GPUShader* blurFrag = shader(device_, kGuiBlurFrag, fragment, 1, 1, "gui_blur.frag");
    SDL_GPUShader* shadowFrag = shader(device_, kGuiShadowFrag, fragment, 1, 1, "gui_shadow.frag");
    SDL_GPUShader* maskFrag = shader(device_, kGuiMaskFrag, fragment, 2, 0, "gui_mask.frag");

    enum class Blend
    {
        Premultiplied,
        Replace,
        NoColor, // only the stencil written
    };
    // Into a page or its layers (with the stencil: `stencil`) or into the
    // filters' images (`layer` false: no stencil there).
    const auto make = [&](SDL_GPUShader* v, SDL_GPUShader* f, bool vertices, Blend blend,
                          Stencil stencil, bool layer,
                          SDL_GPUTextureFormat format = kPageFormat) -> SDL_GPUGraphicsPipeline*
    {
        if(!v || !f)
        {
            return nullptr;
        }
        SDL_GPUColorTargetDescription color = premultiplied(format);
        if(blend != Blend::Premultiplied)
        {
            color.blend_state.enable_blend = false;
        }
        if(blend == Blend::NoColor)
        {
            color.blend_state.enable_color_write_mask = true;
            color.blend_state.color_write_mask = 0;
        }
        const SDL_GPUVertexBufferDescription buffer = {0, sizeof(Rml::Vertex),
                                                       SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
        const SDL_GPUVertexAttribute attributes[] = {
            {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Rml::Vertex, position)},
            {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(Rml::Vertex, colour)},
            {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Rml::Vertex, tex_coord)},
        };
        SDL_GPUGraphicsPipelineCreateInfo info = {};
        info.vertex_shader = v;
        info.fragment_shader = f;
        if(vertices)
        {
            info.vertex_input_state.num_vertex_buffers = 1;
            info.vertex_input_state.vertex_buffer_descriptions = &buffer;
            info.vertex_input_state.vertex_attributes = attributes;
            info.vertex_input_state.num_vertex_attributes =
                static_cast<Uint32>(std::size(attributes));
        }
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.target_info.num_color_targets = 1;
        info.target_info.color_target_descriptions = &color;
        if(layer)
        {
            info.target_info.has_depth_stencil_target = true;
            info.target_info.depth_stencil_format = stencilFormat_;
            SDL_GPUDepthStencilState& ds = info.depth_stencil_state;
            ds.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
            if(stencil != Stencil::None)
            {
                SDL_GPUStencilOpState op = {};
                op.fail_op = SDL_GPU_STENCILOP_KEEP;
                op.depth_fail_op = SDL_GPU_STENCILOP_KEEP;
                op.pass_op = stencil == Stencil::Replace     ? SDL_GPU_STENCILOP_REPLACE
                             : stencil == Stencil::Increment ? SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP
                                                             : SDL_GPU_STENCILOP_KEEP;
                op.compare_op =
                    stencil == Stencil::Test ? SDL_GPU_COMPAREOP_EQUAL : SDL_GPU_COMPAREOP_ALWAYS;
                ds.front_stencil_state = op;
                ds.back_stencil_state = op;
                ds.compare_mask = 0xff;
                ds.write_mask = stencil == Stencil::Test ? 0 : 0xff;
                ds.enable_stencil_test = true;
            }
        }
        return SDL_CreateGPUGraphicsPipeline(device_, &info);
    };

    pipelines_ = std::make_unique<Pipelines>();
    Pipelines& p = *pipelines_;
    for(size_t test = 0; test < 2; test++)
    {
        const Stencil stencil = test ? Stencil::Test : Stencil::None;
        p.color[test] = make(vert, colorFrag, true, Blend::Premultiplied, stencil, true);
        p.texture[test] = make(vert, textureFrag, true, Blend::Premultiplied, stencil, true);
        p.gradient[test] = make(vert, gradientFrag, true, Blend::Premultiplied, stencil, true);
        for(size_t replace = 0; replace < 2; replace++)
        {
            p.composite[replace][test] =
                make(quadVert, copyFrag, false, replace ? Blend::Replace : Blend::Premultiplied,
                     stencil, true);
        }
    }
    p.maskReplace = make(vert, colorFrag, true, Blend::NoColor, Stencil::Replace, true);
    p.maskIncrement = make(vert, colorFrag, true, Blend::NoColor, Stencil::Increment, true);
    p.copy = make(quadVert, copyFrag, false, Blend::Replace, Stencil::None, false);
    p.copyBlend = make(quadVert, copyFrag, false, Blend::Premultiplied, Stencil::None, false);
    p.matrix = make(quadVert, matrixFrag, false, Blend::Replace, Stencil::None, false);
    p.blur = make(quadVert, blurFrag, false, Blend::Replace, Stencil::None, false);
    p.shadow = make(quadVert, shadowFrag, false, Blend::Replace, Stencil::None, false);
    p.mask = make(quadVert, maskFrag, false, Blend::Replace, Stencil::None, false);
    p.blit =
        make(blitVert, blitFrag, false, Blend::Premultiplied, Stencil::None, false, windowFormat);
    for(SDL_GPUShader* s : {vert, colorFrag, textureFrag, gradientFrag, blitVert, blitFrag,
                            quadVert, copyFrag, matrixFrag, blurFrag, shadowFrag, maskFrag})
    {
        if(s)
        {
            SDL_ReleaseGPUShader(device_, s);
        }
    }

    SDL_GPUSamplerCreateInfo sampler = {};
    sampler.min_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR; // the app's images have mips
    sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = SDL_CreateGPUSampler(device_, &sampler);

    const auto all = p.all();
    if(std::ranges::find(all, nullptr) != all.end() || !sampler_)
    {
        log::error("creating the interface's pipelines failed: {}", SDL_GetError());
        return false;
    }
    return true;
}

void Renderer::begin(glm::ivec2 size)
{
    size_ = size;
    // Pixels (y down) to clip space (y up).
    projection_ =
        glm::ortho(0.0f, static_cast<float>(size.x), static_cast<float>(size.y), 0.0f, -1.0f, 1.0f);
    transform_ = projection_;
    scissorOn_ = false;
    clipMask_ = false;
    stencilRef_ = 0;
    layerStack_.assign(1, 0);
    layersUsed_ = 0;
    vertices_.clear();
    indices_.clear();
    draws_.clear();
    gradients_.clear();

    const auto w = static_cast<float>(size.x);
    const auto h = static_cast<float>(size.y);
    const Rml::ColourbPremultiplied white(255, 255, 255, 255);
    screen_.vertices = {{{0.0f, 0.0f}, white, {0.0f, 0.0f}},
                        {{w, 0.0f}, white, {1.0f, 0.0f}},
                        {{w, h}, white, {1.0f, 1.0f}},
                        {{0.0f, h}, white, {0.0f, 1.0f}}};
    screen_.indices = {0, 1, 2, 0, 2, 3};
}

Rml::CompiledGeometryHandle Renderer::CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                      Rml::Span<const int> indices)
{
    auto* g = new Geometry;
    g->vertices.assign(vertices.begin(), vertices.end());
    g->indices.assign(indices.begin(), indices.end());
    return reinterpret_cast<Rml::CompiledGeometryHandle>(g);
}

void Renderer::ReleaseGeometry(Rml::CompiledGeometryHandle geometry)
{
    delete reinterpret_cast<Geometry*>(geometry);
}

void Renderer::add(Draw::Kind kind, const Geometry& geometry, Rml::Vector2f translation)
{
    if(geometry.indices.empty())
    {
        return;
    }
    Draw draw;
    draw.kind = kind;
    draw.layer = layerStack_.back();
    stencilFor(draw);
    draw.firstIndex = static_cast<uint32_t>(indices_.size());
    draw.indexCount = static_cast<uint32_t>(geometry.indices.size());
    draw.vs.transform = transform_;
    draw.vs.translate = glm::vec4(translation.x, translation.y, 0.0f, 0.0f);
    if(scissorOn_)
    {
        draw.scissor = scissor_;
    }
    const auto base = static_cast<uint32_t>(vertices_.size());
    vertices_.insert(vertices_.end(), geometry.vertices.begin(), geometry.vertices.end());
    for(const int i : geometry.indices)
    {
        indices_.push_back(base + static_cast<uint32_t>(i));
    }
    draws_.push_back(std::move(draw));
}

void Renderer::stencilFor(Draw& draw) const
{
    if(clipMask_)
    {
        draw.stencil = Stencil::Test;
        draw.stencilRef = stencilRef_;
    }
}

void Renderer::RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                              Rml::TextureHandle texture)
{
    const auto* t = reinterpret_cast<const Texture*>(texture);
    SDL_GPUTexture* gpu = t ? t->texture : nullptr;
    TextureFS mode;
    if(t && t->external)
    {
        // The app's image as it is this frame; nothing drawn until it's there.
        const ExternalImage image = t->external();
        if(!image.texture)
        {
            return;
        }
        gpu = image.texture;
        mode.mode = glm::vec4(image.linear ? 1.0f : 0.0f, image.premultiplied ? 0.0f : 1.0f,
                              image.bottomUp ? 1.0f : 0.0f, 0.0f);
    }
    if(t && !gpu)
    {
        return;
    }
    const size_t before = draws_.size();
    add(t ? Draw::Kind::Texture : Draw::Kind::Color, *reinterpret_cast<const Geometry*>(geometry),
        translation);
    if(t && draws_.size() > before)
    {
        draws_.back().texture = gpu;
        draws_.back().textureFs = mode;
    }
}

Renderer::Texture* Renderer::makeTexture(glm::ivec2 size, std::vector<uint8_t> pixels)
{
    if(size.x <= 0 || size.y <= 0)
    {
        return nullptr;
    }
    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = static_cast<Uint32>(size.x);
    info.height = static_cast<Uint32>(size.y);
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    SDL_GPUTexture* texture = SDL_CreateGPUTexture(device_, &info);
    if(!texture)
    {
        log::error("creating an interface texture failed: {}", SDL_GetError());
        return nullptr;
    }
    uploads_.push_back({texture, size, std::move(pixels)});
    return new Texture{texture, size, {}};
}

Rml::TextureHandle Renderer::LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source)
{
    // image://NAME: decoded by the app.
    constexpr std::string_view kImage = "image://";
    if(source.starts_with(kImage))
    {
        const auto image = images_.find(std::string_view(source).substr(kImage.size()));
        if(image == images_.end())
        {
            log::warn("interface image {}: not given (yet)", source);
            return {};
        }
        dimensions = {image->second.size.x, image->second.size.y};
        return reinterpret_cast<Rml::TextureHandle>(
            makeTexture(image->second.size, image->second.rgba));
    }
    // SCHEME://REST: the app's.
    if(const size_t split = source.find("://"); split != Rml::String::npos)
    {
        const auto scheme = schemes_.find(std::string_view(source).substr(0, split));
        if(scheme == schemes_.end())
        {
            log::warn("interface image {}: nothing gives {}:// images", source,
                      source.substr(0, split));
            return {};
        }
        const std::optional<SchemeImage> image =
            scheme->second(std::string_view(source).substr(split + 3));
        if(!image)
        {
            log::warn("interface image {}: no such image", source);
            return {};
        }
        dimensions = {image->size.x, image->size.y};
        return reinterpret_cast<Rml::TextureHandle>(
            new Texture{nullptr, image->size, image->image});
    }
    std::optional<Pixels> pixels;
    if(SDL_IOStream* file = SDL_IOFromFile(source.c_str(), "rb"))
    {
        size_t length = 0;
        if(void* data = SDL_LoadFile_IO(file, &length, true))
        {
            pixels = decodeImage({static_cast<const uint8_t*>(data), length});
            SDL_free(data);
        }
    }
    if(!pixels)
    {
        log::warn("interface image {} can't be read", source);
        return {};
    }
    dimensions = {pixels->size.x, pixels->size.y};
    return reinterpret_cast<Rml::TextureHandle>(makeTexture(pixels->size, std::move(pixels->rgba)));
}

Rml::TextureHandle Renderer::GenerateTexture(Rml::Span<const Rml::byte> source,
                                             Rml::Vector2i dimensions)
{
    return reinterpret_cast<Rml::TextureHandle>(makeTexture(
        {dimensions.x, dimensions.y}, std::vector<uint8_t>(source.begin(), source.end())));
}

void Renderer::ReleaseTexture(Rml::TextureHandle texture)
{
    auto* t = reinterpret_cast<Texture*>(texture);
    if(!t)
    {
        return;
    }
    if(t->texture)
    {
        std::erase_if(uploads_, [&](const Upload& u) { return u.texture == t->texture; });
        // The GPU lets go of it once frames using it are done.
        SDL_ReleaseGPUTexture(device_, t->texture);
    }
    delete t; // the app's images are the app's
}

void Renderer::EnableScissorRegion(bool enable)
{
    scissorOn_ = enable;
}

void Renderer::SetScissorRegion(Rml::Rectanglei region)
{
    const int left = std::clamp(region.Left(), 0, size_.x);
    const int top = std::clamp(region.Top(), 0, size_.y);
    const int right = std::clamp(region.Left() + region.Width(), left, size_.x);
    const int bottom = std::clamp(region.Top() + region.Height(), top, size_.y);
    scissor_ = SDL_Rect{left, top, right - left, bottom - top};
}

void Renderer::SetTransform(const Rml::Matrix4f* transform)
{
    glm::mat4 m(1.0f);
    if(transform)
    {
        m = glm::make_mat4(transform->data()); // both column-major
    }
    transform_ = projection_ * m;
}

Rml::CompiledShaderHandle Renderer::CompileShader(const Rml::String& name,
                                                  const Rml::Dictionary& parameters)
{
    // As RmlUi's own OpenGL 3 renderer reads them.
    Gradient g;
    const bool repeating = Rml::Get(parameters, "repeating", false);
    if(name == "linear-gradient")
    {
        const Rml::Vector2f p0 = Rml::Get(parameters, "p0", Rml::Vector2f(0.0f));
        const Rml::Vector2f p1 = Rml::Get(parameters, "p1", Rml::Vector2f(0.0f));
        g.function.x = repeating ? 3 : 0;
        g.pv = glm::vec4(p0.x, p0.y, p1.x - p0.x, p1.y - p0.y);
    }
    else if(name == "radial-gradient")
    {
        const Rml::Vector2f center = Rml::Get(parameters, "center", Rml::Vector2f(0.0f));
        const Rml::Vector2f radius = Rml::Get(parameters, "radius", Rml::Vector2f(1.0f));
        g.function.x = repeating ? 4 : 1;
        g.pv = glm::vec4(center.x, center.y, 1.0f / radius.x, 1.0f / radius.y);
    }
    else if(name == "conic-gradient")
    {
        const Rml::Vector2f center = Rml::Get(parameters, "center", Rml::Vector2f(0.0f));
        const float angle = Rml::Get(parameters, "angle", 0.0f);
        g.function.x = repeating ? 5 : 2;
        g.pv = glm::vec4(center.x, center.y, std::cos(angle), std::sin(angle));
    }
    else
    {
        log::warn("the interface has no shader '{}'", name);
        return {};
    }
    const auto stops = parameters.find("color_stop_list");
    if(stops == parameters.end() || stops->second.GetType() != Rml::Variant::COLORSTOPLIST)
    {
        return {};
    }
    const auto& list = stops->second.GetReference<Rml::ColorStopList>();
    const int count = std::min(static_cast<int>(list.size()), kMaxStops);
    g.function.y = count;
    for(int i = 0; i < count; i++)
    {
        const Rml::ColorStop& stop = list[static_cast<size_t>(i)];
        g.positions[static_cast<size_t>(i / 4)][i % 4] = stop.position.number;
        g.colors[static_cast<size_t>(i)] = toColor(stop.color);
    }
    return reinterpret_cast<Rml::CompiledShaderHandle>(new Gradient(g));
}

void Renderer::RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry,
                            Rml::Vector2f translation, Rml::TextureHandle /*texture*/)
{
    const auto* g = reinterpret_cast<const Gradient*>(shader);
    if(!g || !geometry)
    {
        return;
    }
    const size_t before = draws_.size();
    add(Draw::Kind::Gradient, *reinterpret_cast<const Geometry*>(geometry), translation);
    if(draws_.size() > before)
    {
        draws_.back().gradient = static_cast<int>(gradients_.size());
        gradients_.push_back(*g);
    }
}

void Renderer::ReleaseShader(Rml::CompiledShaderHandle shader)
{
    delete reinterpret_cast<Gradient*>(shader);
}

// Clip masks: a stencil, shared by the page's layers. Set clears it (by
// drawing the whole page) and writes 1 where the geometry is, then draws
// pass where it's 1; SetInverse the same, passing where it's 0; Intersect
// adds one where the geometry is, and draws pass where it's one more.
void Renderer::EnableClipMask(bool enable)
{
    clipMask_ = enable;
}

void Renderer::RenderToClipMask(Rml::ClipMaskOperation operation,
                                Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation)
{
    const auto* g = reinterpret_cast<const Geometry*>(geometry);
    if(!g)
    {
        return;
    }
    const auto write = [&](const Geometry& what, Rml::Vector2f at, Stencil how, uint8_t ref)
    {
        const size_t before = draws_.size();
        add(Draw::Kind::Color, what, at);
        if(draws_.size() > before)
        {
            draws_.back().stencil = how;
            draws_.back().stencilRef = ref;
        }
    };
    if(operation == Rml::ClipMaskOperation::Intersect)
    {
        write(*g, translation, Stencil::Increment, 0);
        stencilRef_++;
        return;
    }
    const glm::mat4 transform = transform_;
    const bool scissor = scissorOn_;
    transform_ = projection_;
    scissorOn_ = false;
    write(screen_, {}, Stencil::Replace, 0);
    transform_ = transform;
    scissorOn_ = scissor;
    write(*g, translation, Stencil::Replace, 1);
    stencilRef_ = operation == Rml::ClipMaskOperation::Set ? 1 : 0;
}

// Layers: by how deep they are (0 the page), so one popped is reused by the
// next pushed (what it held has been composited by then).
Rml::LayerHandle Renderer::PushLayer()
{
    const int layer = static_cast<int>(layerStack_.size());
    layerStack_.push_back(layer);
    layersUsed_ = std::max(layersUsed_, layer);
    Draw clear;
    clear.kind = Draw::Kind::Clear;
    clear.layer = layer;
    draws_.push_back(std::move(clear));
    return static_cast<Rml::LayerHandle>(layer);
}

void Renderer::CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination,
                               Rml::BlendMode blendMode,
                               Rml::Span<const Rml::CompiledFilterHandle> filters)
{
    Draw composite;
    composite.kind = Draw::Kind::Composite;
    composite.source = static_cast<int>(source);
    composite.layer = static_cast<int>(destination);
    composite.replace = blendMode == Rml::BlendMode::Replace;
    // Copied: RmlUi may release them before the page is recorded.
    for(const Rml::CompiledFilterHandle f : filters)
    {
        if(f)
        {
            composite.filters.push_back(*reinterpret_cast<const Filter*>(f));
        }
    }
    composite.region = scissorOn_ ? scissor_ : SDL_Rect{0, 0, size_.x, size_.y};
    if(scissorOn_)
    {
        composite.scissor = scissor_;
    }
    stencilFor(composite);
    draws_.push_back(std::move(composite));
}

void Renderer::PopLayer()
{
    if(layerStack_.size() > 1)
    {
        layerStack_.pop_back();
    }
}

Rml::TextureHandle Renderer::SaveLayerAsTexture()
{
    const SDL_Rect region = scissorOn_ ? scissor_ : SDL_Rect{0, 0, size_.x, size_.y};
    if(region.w <= 0 || region.h <= 0)
    {
        return {};
    }
    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = kPageFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = static_cast<Uint32>(region.w);
    info.height = static_cast<Uint32>(region.h);
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    SDL_GPUTexture* texture = SDL_CreateGPUTexture(device_, &info);
    if(!texture)
    {
        log::error("saving an interface layer: {}", SDL_GetError());
        return {};
    }
    Draw save;
    save.kind = Draw::Kind::Save;
    save.layer = layerStack_.back();
    save.region = region;
    save.texture = texture;
    draws_.push_back(std::move(save));
    return reinterpret_cast<Rml::TextureHandle>(new Texture{texture, {region.w, region.h}, {}});
}

Rml::CompiledFilterHandle Renderer::SaveLayerAsMaskImage()
{
    Draw save;
    save.kind = Draw::Kind::SaveMask;
    save.layer = layerStack_.back();
    draws_.push_back(std::move(save));
    Filter mask;
    mask.kind = Filter::Kind::Mask;
    return reinterpret_cast<Rml::CompiledFilterHandle>(new Filter(mask));
}

Rml::CompiledFilterHandle Renderer::CompileFilter(const Rml::String& name,
                                                  const Rml::Dictionary& parameters)
{
    // As RmlUi's own OpenGL 3 renderer makes them.
    Filter f;
    f.kind = Filter::Kind::Matrix;
    const float value = Rml::Get(parameters, "value", 1.0f);
    Rml::Matrix4f m = Rml::Matrix4f::Identity();
    if(name == "opacity")
    {
        f.kind = Filter::Kind::Opacity;
        f.value = value;
    }
    else if(name == "blur")
    {
        f.kind = Filter::Kind::Blur;
        f.sigma = Rml::Get(parameters, "sigma", 1.0f);
    }
    else if(name == "drop-shadow")
    {
        f.kind = Filter::Kind::DropShadow;
        f.sigma = Rml::Get(parameters, "sigma", 0.0f);
        f.color = toColor(Rml::Get(parameters, "color", Rml::Colourb()).ToPremultiplied());
        const Rml::Vector2f offset = Rml::Get(parameters, "offset", Rml::Vector2f(0.0f));
        f.offset = glm::vec2(offset.x, offset.y);
    }
    else if(name == "brightness")
    {
        m = Rml::Matrix4f::Diag(value, value, value, 1.0f);
    }
    else if(name == "contrast")
    {
        const float grey = 0.5f - 0.5f * value;
        m = Rml::Matrix4f::Diag(value, value, value, 1.0f);
        m.SetColumn(3, Rml::Vector4f(grey, grey, grey, 1.0f));
    }
    else if(name == "invert")
    {
        const float v = std::clamp(value, 0.0f, 1.0f);
        const float inverted = 1.0f - 2.0f * v;
        m = Rml::Matrix4f::Diag(inverted, inverted, inverted, 1.0f);
        m.SetColumn(3, Rml::Vector4f(v, v, v, 1.0f));
    }
    else if(name == "grayscale")
    {
        const float rest = 1.0f - value;
        const Rml::Vector3f grey = value * Rml::Vector3f(0.2126f, 0.7152f, 0.0722f);
        m = Rml::Matrix4f::FromRows(
            {grey.x + rest, grey.y, grey.z, 0.0f}, {grey.x, grey.y + rest, grey.z, 0.0f},
            {grey.x, grey.y, grey.z + rest, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f});
    }
    else if(name == "sepia")
    {
        const float rest = 1.0f - value;
        const Rml::Vector3f r = value * Rml::Vector3f(0.393f, 0.769f, 0.189f);
        const Rml::Vector3f g = value * Rml::Vector3f(0.349f, 0.686f, 0.168f);
        const Rml::Vector3f b = value * Rml::Vector3f(0.272f, 0.534f, 0.131f);
        m = Rml::Matrix4f::FromRows({r.x + rest, r.y, r.z, 0.0f}, {g.x, g.y + rest, g.z, 0.0f},
                                    {b.x, b.y, b.z + rest, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f});
    }
    else if(name == "hue-rotate")
    {
        // https://www.w3.org/TR/filter-effects-1/#attr-valuedef-type-huerotate
        const float sn = std::sin(value);
        const float cs = std::cos(value);
        m = Rml::Matrix4f::FromRows(
            {0.213f + 0.787f * cs - 0.213f * sn, 0.715f - 0.715f * cs - 0.715f * sn,
             0.072f - 0.072f * cs + 0.928f * sn, 0.0f},
            {0.213f - 0.213f * cs + 0.143f * sn, 0.715f + 0.285f * cs + 0.140f * sn,
             0.072f - 0.072f * cs - 0.283f * sn, 0.0f},
            {0.213f - 0.213f * cs - 0.787f * sn, 0.715f - 0.715f * cs + 0.715f * sn,
             0.072f + 0.928f * cs + 0.072f * sn, 0.0f},
            {0.0f, 0.0f, 0.0f, 1.0f});
    }
    else if(name == "saturate")
    {
        m = Rml::Matrix4f::FromRows(
            {0.213f + 0.787f * value, 0.715f - 0.715f * value, 0.072f - 0.072f * value, 0.0f},
            {0.213f - 0.213f * value, 0.715f + 0.285f * value, 0.072f - 0.072f * value, 0.0f},
            {0.213f - 0.213f * value, 0.715f - 0.715f * value, 0.072f + 0.928f * value, 0.0f},
            {0.0f, 0.0f, 0.0f, 1.0f});
    }
    else
    {
        log::warn("the interface has no filter '{}'", name);
        return {};
    }
    f.matrix = glm::make_mat4(m.data()); // both column-major
    return reinterpret_cast<Rml::CompiledFilterHandle>(new Filter(f));
}

void Renderer::ReleaseFilter(Rml::CompiledFilterHandle filter)
{
    delete reinterpret_cast<Filter*>(filter);
}

bool Renderer::ensureTargets(glm::ivec2 size)
{
    const glm::ivec2 want = glm::max(targetSize_, size);
    if(want != targetSize_)
    {
        // The GPU lets go of the old ones once frames using them are done.
        for(SDL_GPUTexture* t : layers_)
        {
            SDL_ReleaseGPUTexture(device_, t);
        }
        layers_.clear();
        for(SDL_GPUTexture*& t : post_)
        {
            if(t)
            {
                SDL_ReleaseGPUTexture(device_, t);
            }
            t = nullptr;
        }
        if(stencil_)
        {
            SDL_ReleaseGPUTexture(device_, stencil_);
            stencil_ = nullptr;
        }
        targetSize_ = want;
    }
    const auto make = [&](SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage)
    {
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = format;
        info.usage = usage;
        info.width = static_cast<Uint32>(targetSize_.x);
        info.height = static_cast<Uint32>(targetSize_.y);
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        SDL_GPUTexture* t = SDL_CreateGPUTexture(device_, &info);
        if(!t)
        {
            log::error("the interface's layers: {}", SDL_GetError());
        }
        return t;
    };
    const auto image = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    if(!stencil_)
    {
        stencil_ = make(stencilFormat_, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
    }
    while(static_cast<int>(layers_.size()) < layersUsed_)
    {
        SDL_GPUTexture* t = make(kPageFormat, image);
        if(!t)
        {
            return false;
        }
        layers_.push_back(t);
    }
    const bool post =
        std::ranges::any_of(draws_, [](const Draw& d) { return d.kind >= Draw::Kind::Composite; });
    for(SDL_GPUTexture*& t : post_)
    {
        if(post && !t)
        {
            t = make(kPageFormat, image);
        }
        if(post && !t)
        {
            return false;
        }
    }
    return stencil_ != nullptr;
}

SDL_GPUTexture* Renderer::layerTexture(int layer, SDL_GPUTexture* page) const
{
    return layer == 0 ? page : layers_[static_cast<size_t>(layer - 1)];
}

Renderer::QuadVS Renderer::quadVs(const SDL_Rect& dst, const SDL_Rect& src,
                                  glm::ivec2 srcSize) const
{
    const glm::vec2 page(size_);
    const glm::vec2 from(srcSize);
    QuadVS vs;
    vs.dst = glm::vec4(static_cast<float>(dst.x) / page.x * 2.0f - 1.0f,
                       1.0f - static_cast<float>(dst.y) / page.y * 2.0f,
                       static_cast<float>(dst.x + dst.w) / page.x * 2.0f - 1.0f,
                       1.0f - static_cast<float>(dst.y + dst.h) / page.y * 2.0f);
    vs.src = glm::vec4(static_cast<float>(src.x) / from.x, static_cast<float>(src.y) / from.y,
                       static_cast<float>(src.x + src.w) / from.x,
                       static_cast<float>(src.y + src.h) / from.y);
    return vs;
}

glm::vec4 Renderer::limits(const SDL_Rect& region, glm::ivec2 textureSize) const
{
    // Texel centres, so bilinear reads don't take in what's beside it.
    const glm::vec2 size(textureSize);
    return glm::vec4((static_cast<float>(region.x) + 0.5f) / size.x,
                     (static_cast<float>(region.y) + 0.5f) / size.y,
                     (static_cast<float>(region.x + region.w) - 0.5f) / size.x,
                     (static_cast<float>(region.y + region.h) - 0.5f) / size.y);
}

namespace
{

// gui_copy.frag's CopyFS, gui_blur.frag's BlurFS, gui_shadow.frag's ShadowFS.
struct CopyFS
{
    glm::vec4 factor{1.0f};
    glm::vec4 limits{0.0f};
};
struct BlurFS
{
    glm::vec4 texel{0.0f};
    glm::vec4 limits{0.0f};
    glm::vec4 weights{0.0f};
};
struct ShadowFS
{
    glm::vec4 color{0.0f};
    glm::vec4 limits{0.0f};
};

} // namespace

void Renderer::quad(SDL_GPUCommandBuffer* commands, SDL_GPUGraphicsPipeline* pipeline, int from,
                    int to, const SDL_Rect& dst, const SDL_Rect& src, const void* uniforms,
                    uint32_t bytes, bool clear)
{
    if(dst.w <= 0 || dst.h <= 0)
    {
        return;
    }
    SDL_GPUColorTargetInfo color = {};
    color.texture = post_[static_cast<size_t>(to)];
    color.load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color, 1, nullptr);
    const SDL_GPUViewport viewport = {
        0.0f, 0.0f, static_cast<float>(size_.x), static_cast<float>(size_.y), 0.0f, 1.0f};
    SDL_SetGPUViewport(pass, &viewport);
    SDL_SetGPUScissor(pass, &dst);
    SDL_BindGPUGraphicsPipeline(pass, pipeline);
    const QuadVS vs = quadVs(dst, src, targetSize_);
    SDL_PushGPUVertexUniformData(commands, 0, &vs, sizeof(vs));
    if(bytes > 0)
    {
        SDL_PushGPUFragmentUniformData(commands, 0, uniforms, bytes);
    }
    const SDL_GPUTextureSamplerBinding bindings[] = {{post_[static_cast<size_t>(from)], sampler_},
                                                     {post_[3], sampler_}};
    SDL_BindGPUFragmentSamplers(pass, 0, bindings, pipeline == pipelines_->mask ? 2 : 1);
    SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

void Renderer::blur(SDL_GPUCommandBuffer* commands, float sigma, const SDL_Rect& region, int image,
                    int spare)
{
    // As RmlUi's OpenGL 3 renderer: halved (bilinear) until what's left is
    // a blur of at most 3 pixels, blurred down then across, then scaled
    // back up.
    constexpr int kMaxPasses = 10;
    constexpr float kMaxSigma = 3.0f;
    const int passes = std::clamp(
        static_cast<int>(std::floor(std::log2(std::max(sigma * (2.0f / kMaxSigma), 1.0f)))), 0,
        kMaxPasses);
    sigma = std::clamp(sigma / static_cast<float>(1 << passes), 0.0f, kMaxSigma);

    const auto other = [&](int i) { return i == image ? spare : image; };
    int at = image;
    SDL_Rect r = region;
    for(int i = 0; i < passes; i++)
    {
        const int x0 = (r.x + 1) / 2;
        const int y0 = (r.y + 1) / 2;
        const SDL_Rect half{x0, y0, std::max((r.x + r.w) / 2, x0) - x0,
                            std::max((r.y + r.h) / 2, y0) - y0};
        if(half.w <= 0 || half.h <= 0)
        {
            break;
        }
        const CopyFS copy{glm::vec4(1.0f), limits(r, targetSize_)};
        quad(commands, pipelines_->copy, at, other(at), half, r, &copy, sizeof(copy));
        at = other(at);
        r = half;
    }

    BlurFS b;
    float total = 0.0f;
    for(int i = 0; i < 4; i++)
    {
        const float w = sigma < 0.1f
                            ? (i == 0 ? 1.0f : 0.0f)
                            : std::exp(-static_cast<float>(i * i) / (2.0f * sigma * sigma));
        b.weights[i] = w;
        total += (i == 0 ? 1.0f : 2.0f) * w;
    }
    b.weights /= total;
    b.limits = limits(r, targetSize_);
    b.texel = glm::vec4(0.0f, 1.0f / static_cast<float>(targetSize_.y), 0.0f, 0.0f);
    quad(commands, pipelines_->blur, at, other(at), r, r, &b, sizeof(b));
    b.texel = glm::vec4(1.0f / static_cast<float>(targetSize_.x), 0.0f, 0.0f, 0.0f);
    quad(commands, pipelines_->blur, other(at), at, r, r, &b, sizeof(b));

    const CopyFS up{glm::vec4(1.0f), limits(r, targetSize_)};
    if(r.w != region.w || r.h != region.h)
    {
        quad(commands, pipelines_->copy, at, other(at), region, r, &up, sizeof(up));
        at = other(at);
    }
    if(at != image)
    {
        const CopyFS copy{glm::vec4(1.0f), limits(region, targetSize_)};
        quad(commands, pipelines_->copy, at, image, region, region, &copy, sizeof(copy));
    }
}

void Renderer::runFilters(SDL_GPUCommandBuffer* commands, const std::vector<Filter>& filters,
                          const SDL_Rect& region, int& at)
{
    const glm::vec4 inside = limits(region, targetSize_);
    for(const Filter& f : filters)
    {
        const int other = at == 0 ? 1 : 0;
        const int third = 3 - at - other; // of 0, 1 and 2
        switch(f.kind)
        {
        case Filter::Kind::Opacity:
        {
            const CopyFS copy{glm::vec4(f.value), inside};
            quad(commands, pipelines_->copy, at, other, region, region, &copy, sizeof(copy));
            at = other;
            break;
        }
        case Filter::Kind::Matrix:
            quad(commands, pipelines_->matrix, at, other, region, region, &f.matrix,
                 sizeof(f.matrix));
            at = other;
            break;
        case Filter::Kind::Blur:
            blur(commands, f.sigma, region, at, other);
            break;
        case Filter::Kind::DropShadow:
        {
            // The shadow (read from where the offset moves it from), blurred,
            // then the layer over it.
            const SDL_Rect from{region.x - static_cast<int>(std::lround(f.offset.x)),
                                region.y - static_cast<int>(std::lround(f.offset.y)), region.w,
                                region.h};
            const ShadowFS shadow{f.color, inside};
            quad(commands, pipelines_->shadow, at, other, region, from, &shadow, sizeof(shadow));
            if(f.sigma >= 0.5f)
            {
                blur(commands, f.sigma, region, other, third);
            }
            const CopyFS copy{glm::vec4(1.0f), inside};
            quad(commands, pipelines_->copyBlend, at, other, region, region, &copy, sizeof(copy));
            at = other;
            break;
        }
        case Filter::Kind::Mask:
            quad(commands, pipelines_->mask, at, other, region, region, nullptr, 0);
            at = other;
            break;
        }
    }
}

void Renderer::record(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target)
{
    if(!ensureTargets(size_))
    {
        return;
    }
    const auto vertexBytes = static_cast<uint32_t>(vertices_.size() * sizeof(Rml::Vertex));
    const auto indexBytes = static_cast<uint32_t>(indices_.size() * sizeof(uint32_t));
    auto grow = [&](SDL_GPUBuffer*& buffer, uint32_t& capacity, uint32_t bytes,
                    SDL_GPUBufferUsageFlags usage)
    {
        if(bytes <= capacity)
        {
            return true;
        }
        if(buffer)
        {
            SDL_ReleaseGPUBuffer(device_, buffer);
        }
        capacity = std::max(bytes, capacity * 2);
        SDL_GPUBufferCreateInfo info = {};
        info.usage = usage;
        info.size = capacity;
        buffer = SDL_CreateGPUBuffer(device_, &info);
        return buffer != nullptr;
    };
    bool drawable = !draws_.empty();
    if(drawable)
    {
        drawable = grow(vertexBuffer_, vertexCapacity_, vertexBytes, SDL_GPU_BUFFERUSAGE_VERTEX) &&
                   grow(indexBuffer_, indexCapacity_, indexBytes, SDL_GPU_BUFFERUSAGE_INDEX);
        if(drawable && vertexBytes + indexBytes > transferCapacity_)
        {
            if(transfer_)
            {
                SDL_ReleaseGPUTransferBuffer(device_, transfer_);
            }
            transferCapacity_ = std::max(vertexBytes + indexBytes, transferCapacity_ * 2);
            SDL_GPUTransferBufferCreateInfo info = {};
            info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            info.size = transferCapacity_;
            transfer_ = SDL_CreateGPUTransferBuffer(device_, &info);
            drawable = transfer_ != nullptr;
        }
        if(!drawable)
        {
            log::error("the interface's buffers: {}", SDL_GetError());
        }
    }

    // New textures and the page's vertices up.
    if(!uploads_.empty() || drawable)
    {
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        for(Upload& u : uploads_)
        {
            SDL_GPUTransferBufferCreateInfo info = {};
            info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            info.size = static_cast<Uint32>(u.pixels.size());
            SDL_GPUTransferBuffer* staging = SDL_CreateGPUTransferBuffer(device_, &info);
            if(!staging)
            {
                continue;
            }
            std::memcpy(SDL_MapGPUTransferBuffer(device_, staging, false), u.pixels.data(),
                        u.pixels.size());
            SDL_UnmapGPUTransferBuffer(device_, staging);
            SDL_GPUTextureTransferInfo from = {};
            from.transfer_buffer = staging;
            SDL_GPUTextureRegion to = {};
            to.texture = u.texture;
            to.w = static_cast<Uint32>(u.size.x);
            to.h = static_cast<Uint32>(u.size.y);
            to.d = 1;
            SDL_UploadToGPUTexture(copy, &from, &to, false);
            SDL_ReleaseGPUTransferBuffer(device_, staging);
        }
        uploads_.clear();
        if(drawable)
        {
            auto* mapped =
                static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device_, transfer_, true));
            std::memcpy(mapped, vertices_.data(), vertexBytes);
            std::memcpy(mapped + vertexBytes, indices_.data(), indexBytes);
            SDL_UnmapGPUTransferBuffer(device_, transfer_);
            const SDL_GPUTransferBufferLocation vertexFrom = {transfer_, 0};
            const SDL_GPUBufferRegion vertexTo = {vertexBuffer_, 0, vertexBytes};
            SDL_UploadToGPUBuffer(copy, &vertexFrom, &vertexTo, true);
            const SDL_GPUTransferBufferLocation indexFrom = {transfer_, vertexBytes};
            const SDL_GPUBufferRegion indexTo = {indexBuffer_, 0, indexBytes};
            SDL_UploadToGPUBuffer(copy, &indexFrom, &indexTo, true);
        }
        SDL_EndGPUCopyPass(copy);
    }

    // The steps, in order: a render pass on whichever layer is being drawn
    // into, ended for copies and filters (each their own).
    const SDL_Rect whole{0, 0, size_.x, size_.y};
    const Pipelines& p = *pipelines_;
    SDL_GPURenderPass* pass = nullptr;
    int passLayer = -1;
    bool stencilCleared = false;
    SDL_GPUGraphicsPipeline* bound = nullptr;
    const auto close = [&]
    {
        if(pass)
        {
            SDL_EndGPURenderPass(pass);
            pass = nullptr;
            passLayer = -1;
        }
    };
    const auto open = [&](int layer, bool clear)
    {
        close();
        SDL_GPUColorTargetInfo color = {};
        color.texture = layerTexture(layer, target);
        color.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};
        color.load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        color.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPUDepthStencilTargetInfo depth = {};
        depth.texture = stencil_;
        depth.load_op = SDL_GPU_LOADOP_DONT_CARE;
        depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
        depth.stencil_load_op = stencilCleared ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
        depth.stencil_store_op = SDL_GPU_STOREOP_STORE;
        depth.clear_stencil = 0;
        stencilCleared = true;
        pass = SDL_BeginGPURenderPass(commands, &color, 1, &depth);
        const SDL_GPUViewport viewport = {
            0.0f, 0.0f, static_cast<float>(size_.x), static_cast<float>(size_.y), 0.0f, 1.0f};
        SDL_SetGPUViewport(pass, &viewport);
        if(drawable)
        {
            const SDL_GPUBufferBinding vertices = {vertexBuffer_, 0};
            SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
            const SDL_GPUBufferBinding indices = {indexBuffer_, 0};
            SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        }
        bound = nullptr;
        passLayer = layer;
    };
    const auto bind = [&](SDL_GPUGraphicsPipeline* pipeline)
    {
        if(pipeline != bound)
        {
            SDL_BindGPUGraphicsPipeline(pass, pipeline);
            bound = pipeline;
        }
    };
    const auto copy =
        [&](SDL_GPUTexture* from, const SDL_Rect& region, SDL_GPUTexture* to, int x, int y)
    {
        SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commands);
        SDL_GPUTextureLocation source = {};
        source.texture = from;
        source.x = static_cast<Uint32>(region.x);
        source.y = static_cast<Uint32>(region.y);
        SDL_GPUTextureLocation destination = {};
        destination.texture = to;
        destination.x = static_cast<Uint32>(x);
        destination.y = static_cast<Uint32>(y);
        SDL_CopyGPUTextureToTexture(copyPass, &source, &destination, static_cast<Uint32>(region.w),
                                    static_cast<Uint32>(region.h), 1, false);
        SDL_EndGPUCopyPass(copyPass);
    };

    open(0, true);
    for(const Draw& d : draws_)
    {
        switch(d.kind)
        {
        case Draw::Kind::Clear:
            open(d.layer, true);
            break;
        case Draw::Kind::Color:
        case Draw::Kind::Texture:
        case Draw::Kind::Gradient:
        {
            const SDL_Rect scissor = d.scissor.value_or(whole);
            if(!drawable || scissor.w <= 0 || scissor.h <= 0)
            {
                break;
            }
            if(passLayer != d.layer)
            {
                open(d.layer, false);
            }
            const size_t test = d.stencil == Stencil::Test ? 1 : 0;
            bind(d.stencil == Stencil::Replace     ? p.maskReplace
                 : d.stencil == Stencil::Increment ? p.maskIncrement
                 : d.kind == Draw::Kind::Texture   ? p.texture[test]
                 : d.kind == Draw::Kind::Gradient  ? p.gradient[test]
                                                   : p.color[test]);
            if(d.stencil != Stencil::None)
            {
                SDL_SetGPUStencilReference(pass, d.stencilRef);
            }
            SDL_SetGPUScissor(pass, &scissor);
            SDL_PushGPUVertexUniformData(commands, 0, &d.vs, sizeof(d.vs));
            if(d.kind == Draw::Kind::Texture)
            {
                const SDL_GPUTextureSamplerBinding binding = {d.texture, sampler_};
                SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
                SDL_PushGPUFragmentUniformData(commands, 0, &d.textureFs, sizeof(d.textureFs));
            }
            else if(d.kind == Draw::Kind::Gradient)
            {
                const Gradient& g = gradients_[static_cast<size_t>(d.gradient)];
                SDL_PushGPUFragmentUniformData(commands, 0, &g, sizeof(g));
            }
            SDL_DrawGPUIndexedPrimitives(pass, d.indexCount, 1, d.firstIndex, 0, 0);
            break;
        }
        case Draw::Kind::Composite:
        {
            // The source into the filters' first image, through them, then
            // onto the destination (inside the clip mask, if it's on).
            const SDL_Rect scissor = d.scissor.value_or(whole);
            if(scissor.w <= 0 || scissor.h <= 0)
            {
                break;
            }
            close();
            copy(layerTexture(d.source, target), whole, post_[0], 0, 0);
            int at = 0;
            runFilters(commands, d.filters, d.region, at);
            open(d.layer, false);
            bind(p.composite[d.replace ? 1 : 0][d.stencil == Stencil::Test ? 1 : 0]);
            if(d.stencil == Stencil::Test)
            {
                SDL_SetGPUStencilReference(pass, d.stencilRef);
            }
            SDL_SetGPUScissor(pass, &scissor);
            const QuadVS vs = quadVs(whole, whole, targetSize_);
            SDL_PushGPUVertexUniformData(commands, 0, &vs, sizeof(vs));
            const CopyFS fs{glm::vec4(1.0f), limits(whole, targetSize_)};
            SDL_PushGPUFragmentUniformData(commands, 0, &fs, sizeof(fs));
            const SDL_GPUTextureSamplerBinding binding = {post_[static_cast<size_t>(at)], sampler_};
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
            break;
        }
        case Draw::Kind::Save:
            close();
            copy(layerTexture(d.layer, target), d.region, d.texture, 0, 0);
            break;
        case Draw::Kind::SaveMask:
            close();
            copy(layerTexture(d.layer, target), whole, post_[3], 0, 0);
            break;
        }
    }
    close();
}

void Renderer::blit(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target, SDL_GPUTexture* image,
                    const glm::vec4& rect)
{
    SDL_GPUColorTargetInfo color = {};
    color.texture = target;
    color.load_op = SDL_GPU_LOADOP_LOAD;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, pipelines_->blit);
    SDL_PushGPUVertexUniformData(commands, 0, &rect, sizeof(rect));
    const SDL_GPUTextureSamplerBinding binding = {image, sampler_};
    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
    SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

} // namespace viewer::gui
