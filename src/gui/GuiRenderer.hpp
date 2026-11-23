#pragma once

// RmlUi's rendering on SDL GPU (internal to the gui module). What a page
// draws is collected while RmlUi renders it (begin(), then the
// RenderInterface calls, its geometry copied as it's drawn) as a list of
// steps, then put into its image in one go (record()): new textures and the
// frame's vertices uploaded, then the steps in order.
//
// Geometry, textures, scissor rectangles, transforms and gradients; clip
// masks (a stencil shared by the page's layers); layers, composited with
// filters (opacity, the color matrices, blur, drop-shadow, mask images);
// and layers saved as textures (how RmlUi draws box-shadow).

#include "viewer/gui/Gui.hpp"

#include <RmlUi/Core/RenderInterface.h>

#include <SDL3/SDL_gpu.h>

#include <glm/glm.hpp>

#include <array>
#include <functional>
#include <map>
#include <memory>
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

    explicit Renderer(SDL_GPUDevice* device);
    ~Renderer() override;

    bool create(SDL_GPUTextureFormat windowFormat);
    SDL_GPUDevice* device() const { return device_; }
    // Images whose source is SCHEME://... (Gui::addImageScheme).
    using Scheme = std::function<std::optional<SchemeImage>(std::string_view)>;
    void addScheme(const std::string& scheme, Scheme resolve)
    {
        schemes_[scheme] = std::move(resolve);
    }
    // image://NAME (Gui::setImage).
    void setImage(const std::string& name, Pixels pixels) { images_[name] = std::move(pixels); }
    void removeImage(const std::string& name) { images_.erase(name); }

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
    void EnableClipMask(bool enable) override;
    void RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry,
                          Rml::Vector2f translation) override;
    Rml::LayerHandle PushLayer() override;
    void CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination,
                         Rml::BlendMode blendMode,
                         Rml::Span<const Rml::CompiledFilterHandle> filters) override;
    void PopLayer() override;
    Rml::TextureHandle SaveLayerAsTexture() override;
    Rml::CompiledFilterHandle SaveLayerAsMaskImage() override;
    Rml::CompiledFilterHandle CompileFilter(const Rml::String& name,
                                            const Rml::Dictionary& parameters) override;
    void ReleaseFilter(Rml::CompiledFilterHandle filter) override;

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
    // How a draw meets the clip mask: not at all, only where it's set, or
    // setting it (writing ref where drawn), or narrowing it (adding one).
    enum class Stencil
    {
        None,
        Test,
        Replace,
        Increment,
    };
    struct Filter
    {
        enum class Kind
        {
            Opacity,
            Matrix, // brightness, contrast, invert, grayscale, sepia, hue-rotate, saturate
            Blur,
            DropShadow,
            Mask, // the mask image saved last (SaveLayerAsMaskImage)
        };
        Kind kind = Kind::Opacity;
        float value = 1.0f;     // opacity
        float sigma = 0.0f;     // blur, drop-shadow (pixels)
        glm::vec4 color{0.0f};  // drop-shadow's, premultiplied
        glm::vec2 offset{0.0f}; // drop-shadow's (pixels)
        glm::mat4 matrix{1.0f};
    };
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
    // One step of drawing a page, in order.
    struct Draw
    {
        enum class Kind
        {
            Color,     // geometry: its vertices' colors
            Texture,   // geometry, textured
            Gradient,  // geometry, through a gradient
            Clear,     // a layer made transparent (pushed)
            Composite, // a layer onto another, through filters
            Save,      // a layer's region copied into a texture
            SaveMask,  // a layer kept as the mask image
        };
        Kind kind = Kind::Color;
        int layer = 0; // drawn into (0: the page)
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
        GuiVS vs;
        SDL_GPUTexture* texture = nullptr; // Texture's; Save's destination
        TextureFS textureFs;
        int gradient = -1; // in gradients_
        std::optional<SDL_Rect> scissor;
        Stencil stencil = Stencil::None;
        uint8_t stencilRef = 0;
        // Composite: from this layer (into `layer`), replacing or blending,
        // through these. Save: this region.
        int source = 0;
        bool replace = false;
        std::vector<Filter> filters;
        SDL_Rect region{};
    };
    // gui_quad.vert's QuadVS.
    struct QuadVS
    {
        glm::vec4 dst{0.0f};
        glm::vec4 src{0.0f};
    };
    struct Pipelines;

    Texture* makeTexture(glm::ivec2 size, std::vector<uint8_t> pixels);
    void add(Draw::Kind kind, const Geometry& geometry, Rml::Vector2f translation);
    // Draws' clip-mask state as it is now.
    void stencilFor(Draw& draw) const;
    // The layers' textures (and the stencil) for pages up to `size`.
    bool ensureTargets(glm::ivec2 size);
    SDL_GPUTexture* layerTexture(int layer, SDL_GPUTexture* page) const;
    // Filters, on post_[at] (the result left there too, at moves).
    void runFilters(SDL_GPUCommandBuffer* commands, const std::vector<Filter>& filters,
                    const SDL_Rect& region, int& at);
    void blur(SDL_GPUCommandBuffer* commands, float sigma, const SDL_Rect& region, int image,
              int spare);
    // One rectangle of post_[from] (`src`, pixels) into post_[to] (`dst`),
    // in its own render pass.
    void quad(SDL_GPUCommandBuffer* commands, SDL_GPUGraphicsPipeline* pipeline, int from, int to,
              const SDL_Rect& dst, const SDL_Rect& src, const void* uniforms, uint32_t bytes,
              bool clear = false);
    QuadVS quadVs(const SDL_Rect& dst, const SDL_Rect& src, glm::ivec2 srcSize) const;
    glm::vec4 limits(const SDL_Rect& region, glm::ivec2 textureSize) const;

    SDL_GPUDevice* device_ = nullptr;
    std::map<std::string, Scheme, std::less<>> schemes_;
    std::map<std::string, Pixels, std::less<>> images_;
    std::unique_ptr<Pipelines> pipelines_;
    SDL_GPUSampler* sampler_ = nullptr;
    SDL_GPUTextureFormat stencilFormat_ = SDL_GPU_TEXTUREFORMAT_INVALID;

    // The layers above the page, the filters' images (three to pass between,
    // and the mask image), and the stencil: kept, as big as the biggest page.
    glm::ivec2 targetSize_{0};
    std::vector<SDL_GPUTexture*> layers_;
    std::array<SDL_GPUTexture*, 4> post_{}; // 3: the mask image
    SDL_GPUTexture* stencil_ = nullptr;

    // The page being drawn.
    glm::ivec2 size_{0};
    glm::mat4 projection_{1.0f};
    glm::mat4 transform_{1.0f}; // projection_ times the element's
    bool scissorOn_ = false;
    SDL_Rect scissor_{};
    bool clipMask_ = false;
    uint8_t stencilRef_ = 0;      // where the mask is set
    std::vector<int> layerStack_; // the layers pushed (0: the page)
    int layersUsed_ = 0;          // the most at once this page
    Geometry screen_;             // the page, whole (clearing the mask)
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
