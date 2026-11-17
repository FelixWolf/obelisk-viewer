#include "GuiRenderer.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/gui/GuiShaders.hpp"

#include <RmlUi/Core/DecorationTypes.h>
#include <RmlUi/Core/Dictionary.h>
#include <RmlUi/Core/Math.h>

#include <SDL3/SDL_error.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <stb_image.h>

#include <algorithm>
#include <cstring>
#include <span>

namespace viewer::gui
{

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

Renderer::~Renderer()
{
    for(SDL_GPUGraphicsPipeline* p :
        {colorPipeline_, texturePipeline_, gradientPipeline_, blitPipeline_})
    {
        if(p)
        {
            SDL_ReleaseGPUGraphicsPipeline(device_, p);
        }
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
    SDL_GPUShader* vert =
        shader(device_, shaders::kGuiVert, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, "gui.vert");
    SDL_GPUShader* colorFrag = shader(device_, shaders::kGuiColorFrag, SDL_GPU_SHADERSTAGE_FRAGMENT,
                                      0, 0, "gui_color.frag");
    SDL_GPUShader* textureFrag = shader(device_, shaders::kGuiTextureFrag,
                                        SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0, "gui_texture.frag");
    SDL_GPUShader* gradientFrag = shader(device_, shaders::kGuiGradientFrag,
                                         SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1, "gui_gradient.frag");
    SDL_GPUShader* blitVert =
        shader(device_, shaders::kGuiBlitVert, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, "gui_blit.vert");
    SDL_GPUShader* blitFrag =
        shader(device_, shaders::kGuiBlitFrag, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0, "gui_blit.frag");

    if(vert && colorFrag && textureFrag && gradientFrag)
    {
        const SDL_GPUVertexBufferDescription buffer = {0, sizeof(Rml::Vertex),
                                                       SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
        const SDL_GPUVertexAttribute attributes[] = {
            {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Rml::Vertex, position)},
            {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(Rml::Vertex, colour)},
            {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Rml::Vertex, tex_coord)},
        };
        const SDL_GPUColorTargetDescription color = premultiplied(kPageFormat);
        SDL_GPUGraphicsPipelineCreateInfo info = {};
        info.vertex_shader = vert;
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_buffer_descriptions = &buffer;
        info.vertex_input_state.vertex_attributes = attributes;
        info.vertex_input_state.num_vertex_attributes = static_cast<Uint32>(std::size(attributes));
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.target_info.num_color_targets = 1;
        info.target_info.color_target_descriptions = &color;
        info.fragment_shader = colorFrag;
        colorPipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
        info.fragment_shader = textureFrag;
        texturePipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
        info.fragment_shader = gradientFrag;
        gradientPipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
    }
    if(blitVert && blitFrag)
    {
        const SDL_GPUColorTargetDescription color = premultiplied(windowFormat);
        SDL_GPUGraphicsPipelineCreateInfo info = {};
        info.vertex_shader = blitVert;
        info.fragment_shader = blitFrag;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.target_info.num_color_targets = 1;
        info.target_info.color_target_descriptions = &color;
        blitPipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
    }
    for(SDL_GPUShader* s : {vert, colorFrag, textureFrag, gradientFrag, blitVert, blitFrag})
    {
        if(s)
        {
            SDL_ReleaseGPUShader(device_, s);
        }
    }

    SDL_GPUSamplerCreateInfo sampler = {};
    sampler.min_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = SDL_CreateGPUSampler(device_, &sampler);

    if(!colorPipeline_ || !texturePipeline_ || !gradientPipeline_ || !blitPipeline_ || !sampler_)
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
    vertices_.clear();
    indices_.clear();
    draws_.clear();
    gradients_.clear();
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
    draws_.push_back(draw);
}

void Renderer::RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                              Rml::TextureHandle texture)
{
    const auto* t = reinterpret_cast<const Texture*>(texture);
    add(t ? Draw::Kind::Texture : Draw::Kind::Color, *reinterpret_cast<const Geometry*>(geometry),
        translation);
    if(t && !draws_.empty())
    {
        draws_.back().texture = t->texture;
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
    return new Texture{texture, size};
}

Rml::TextureHandle Renderer::LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source)
{
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* data = stbi_load(source.c_str(), &width, &height, &channels, 4);
    if(!data)
    {
        log::warn("interface image {}: {}", source, stbi_failure_reason());
        return {};
    }
    std::vector<uint8_t> pixels(data, data + static_cast<size_t>(width) * height * 4);
    stbi_image_free(data);
    for(size_t i = 0; i < pixels.size(); i += 4)
    {
        const unsigned alpha = pixels[i + 3];
        for(size_t c = 0; c < 3; c++)
        {
            pixels[i + c] = static_cast<uint8_t>((pixels[i + c] * alpha + 127) / 255);
        }
    }
    dimensions = {width, height};
    return reinterpret_cast<Rml::TextureHandle>(makeTexture({width, height}, std::move(pixels)));
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
    std::erase_if(uploads_, [&](const Upload& u) { return u.texture == t->texture; });
    // The GPU lets go of it once frames using it are done.
    SDL_ReleaseGPUTexture(device_, t->texture);
    delete t;
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

void Renderer::record(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target)
{
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

    SDL_GPUColorTargetInfo color = {};
    color.texture = target;
    color.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color, 1, nullptr);
    if(drawable)
    {
        const SDL_GPUBufferBinding vertices = {vertexBuffer_, 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
        const SDL_GPUBufferBinding indices = {indexBuffer_, 0};
        SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        const SDL_Rect whole{0, 0, size_.x, size_.y};
        SDL_GPUGraphicsPipeline* bound = nullptr;
        for(const Draw& d : draws_)
        {
            SDL_GPUGraphicsPipeline* pipeline = d.kind == Draw::Kind::Texture    ? texturePipeline_
                                                : d.kind == Draw::Kind::Gradient ? gradientPipeline_
                                                                                 : colorPipeline_;
            if(pipeline != bound)
            {
                SDL_BindGPUGraphicsPipeline(pass, pipeline);
                bound = pipeline;
            }
            const SDL_Rect scissor = d.scissor.value_or(whole);
            if(scissor.w <= 0 || scissor.h <= 0)
            {
                continue;
            }
            SDL_SetGPUScissor(pass, &scissor);
            SDL_PushGPUVertexUniformData(commands, 0, &d.vs, sizeof(d.vs));
            if(d.kind == Draw::Kind::Texture)
            {
                const SDL_GPUTextureSamplerBinding binding = {d.texture, sampler_};
                SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            }
            else if(d.kind == Draw::Kind::Gradient)
            {
                const Gradient& g = gradients_[static_cast<size_t>(d.gradient)];
                SDL_PushGPUFragmentUniformData(commands, 0, &g, sizeof(g));
            }
            SDL_DrawGPUIndexedPrimitives(pass, d.indexCount, 1, d.firstIndex, 0, 0);
        }
    }
    SDL_EndGPURenderPass(pass);
}

void Renderer::blit(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target, SDL_GPUTexture* image,
                    const glm::vec4& rect)
{
    SDL_GPUColorTargetInfo color = {};
    color.texture = target;
    color.load_op = SDL_GPU_LOADOP_LOAD;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &color, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, blitPipeline_);
    SDL_PushGPUVertexUniformData(commands, 0, &rect, sizeof(rect));
    const SDL_GPUTextureSamplerBinding binding = {image, sampler_};
    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
    SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

} // namespace viewer::gui
