#pragma once

// RmlUi's rendering on SDL GPU (internal to the gui module). A page's
// draws are collected while RmlUi renders it (begin(), then the
// RenderInterface calls, its geometry copied as it's drawn), then put into
// its image in one go (record()): new textures and the frame's vertices
// uploaded, then one render pass.
//
// So far: geometry, textures, scissor rectangles, transforms and
// gradients. Not yet: clip masks, layers and filters (RmlUi leaves those
// effects out when they aren't there).

#include "viewer/gui/Gui.hpp"

#include <RmlUi/Core/RenderInterface.h>

#include <SDL3/SDL_gpu.h>

#include <glm/glm.hpp>

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace viewer::gui
{

class Renderer final : public Rml::RenderInterface
{
public:
    // Pages' images: premultiplied, sRGB-encoded values (as the window's).
    static constexpr SDL_GPUTextureFormat kPageFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

    explicit Renderer(SDL_GPUDevice* device) : device_(device) {}
    ~Renderer() override;

    bool create(SDL_GPUTextureFormat windowFormat);
    SDL_GPUDevice* device() const { return device_; }
    // Images whose source is SCHEME://... (Gui::addImageScheme).
    using Scheme = std::function<std::optional<SchemeImage>(std::string_view)>;
    void addScheme(const std::string& scheme, Scheme resolve)
    {
        schemes_[scheme] = std::move(resolve);
    }

    // A page `size` pixels starts being drawn (RmlUi's Context::Render).
    void begin(glm::ivec2 size);
    // What was drawn since begin() into `target` (cleared first).
    void record(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target);
    // An image (premultiplied) over `target`, a window's, in `rect` (clip
    // space: x0, y0 top left, x1, y1 bottom right).
    void blit(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target, SDL_GPUTexture* image,
              const glm::vec4& rect);

    // Rml::RenderInterface.
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                        Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;
    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source,
                                       Rml::Vector2i dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;
    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;
    void SetTransform(const Rml::Matrix4f* transform) override;
    Rml::CompiledShaderHandle CompileShader(const Rml::String& name,
                                            const Rml::Dictionary& parameters) override;
    void RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry,
                      Rml::Vector2f translation, Rml::TextureHandle texture) override;
    void ReleaseShader(Rml::CompiledShaderHandle shader) override;

    static constexpr int kMaxStops = 16;
    // gui_gradient.frag's GradientFS.
    struct Gradient
    {
        glm::ivec4 function{0}; // x the kind, y how many stops
        glm::vec4 pv{0.0f};
        std::array<glm::vec4, kMaxStops / 4> positions{};
        std::array<glm::vec4, kMaxStops> colors{};
    };

private:
    struct Geometry
    {
        std::vector<Rml::Vertex> vertices;
        std::vector<int> indices;
    };
    struct Texture
    {
        SDL_GPUTexture* texture = nullptr; // ours (a file's, RmlUi's)
        glm::ivec2 size{0};
        std::function<ExternalImage()> external; // or the app's, asked each frame
    };
    struct Upload
    {
        SDL_GPUTexture* texture = nullptr;
        glm::ivec2 size{0};
        std::vector<uint8_t> pixels; // RGBA8, premultiplied
    };
    // gui_texture.frag's TextureFS: how to read the texture.
    struct TextureFS
    {
        glm::vec4 mode{0.0f}; // x encode linear to sRGB, y premultiply, z flip v
    };
    // gui.vert's GuiVS.
    struct GuiVS
    {
        glm::mat4 transform{1.0f};
        glm::vec4 translate{0.0f};
    };
    struct Draw
    {
        enum class Kind
        {
            Color,
            Texture,
            Gradient,
        };
        Kind kind = Kind::Color;
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
        GuiVS vs;
        SDL_GPUTexture* texture = nullptr;
        TextureFS textureFs;
        int gradient = -1; // in gradients_
        std::optional<SDL_Rect> scissor;
    };

    Texture* makeTexture(glm::ivec2 size, std::vector<uint8_t> pixels);
    void add(Draw::Kind kind, const Geometry& geometry, Rml::Vector2f translation);

    SDL_GPUDevice* device_ = nullptr;
    std::map<std::string, Scheme, std::less<>> schemes_;
    SDL_GPUGraphicsPipeline* colorPipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* texturePipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* gradientPipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* blitPipeline_ = nullptr;
    SDL_GPUSampler* sampler_ = nullptr;

    // The page being drawn.
    glm::ivec2 size_{0};
    glm::mat4 projection_{1.0f};
    glm::mat4 transform_{1.0f}; // projection_ times the element's
    bool scissorOn_ = false;
    SDL_Rect scissor_{};
    std::vector<Rml::Vertex> vertices_;
    std::vector<uint32_t> indices_;
    std::vector<Draw> draws_;
    std::vector<Gradient> gradients_;
    // Textures made since the last record(), to be uploaded by the next.
    std::vector<Upload> uploads_;

    // Kept and grown: the frame's vertices and indices on the GPU.
    SDL_GPUBuffer* vertexBuffer_ = nullptr;
    SDL_GPUBuffer* indexBuffer_ = nullptr;
    SDL_GPUTransferBuffer* transfer_ = nullptr;
    uint32_t vertexCapacity_ = 0; // bytes
    uint32_t indexCapacity_ = 0;
    uint32_t transferCapacity_ = 0;
};

} // namespace viewer::gui
