#pragma once

// The viewer's own interface, made with RmlUi: pages written as documents
// (RML, like HTML) and style sheets (RCSS, like CSS) in a folder read at run
// time, so they can be changed and reloaded without building. Each page is
// a Surface: drawn into its own image, which goes over the window or onto a
// panel in the scene (VR), and given the pointers' input in its pixels.
//
// RmlUi keeps its interfaces (rendering, the system, fonts) process-wide,
// so there's one Gui at a time: the app's, made and torn down with it.
// Its surfaces must go before it does.

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_keycode.h>

#include <glm/glm.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace Rml
{
class Context;
class ElementDocument;
} // namespace Rml

namespace viewer::gui
{

class Renderer;
class System;
class Surface;

class Gui
{
public:
    struct Options
    {
        // Where the documents, style sheets and images are.
        std::filesystem::path documents;
        // The window's image format (drawOver draws into it).
        SDL_GPUTextureFormat windowFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
    };
    // Null if RmlUi or the GPU pipelines can't be set up.
    static std::unique_ptr<Gui> create(SDL_GPUDevice* device, const Options& options);
    ~Gui();

    Gui(const Gui&) = delete;
    Gui& operator=(const Gui&) = delete;

    const std::filesystem::path& documents() const { return documents_; }

    // A page `size` pixels (its image's), named (unique among them).
    std::unique_ptr<Surface> createSurface(const std::string& name, glm::ivec2 size);

    // A text field took the keyboard (true) or let it go: the app shows
    // one where there's none (VR), or starts the window's text input.
    void setKeyboard(std::function<void(bool)> keyboard);
    // The mouse cursor the page under the pointer asks for (CSS names:
    // "pointer", "text", ...; empty: the usual one).
    const std::string& cursor() const;

    // Forgets the style sheets and templates read, so pages loaded after
    // this read them again (Surface::reload).
    void forgetStyles();

    // A surface's image over a window's image (`target`, `targetSize`
    // pixels) in `rect`: x, y, width, height, pixels.
    void drawOver(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target, glm::uvec2 targetSize,
                  const Surface& surface, const glm::vec4& rect);

private:
    friend class Surface;
    Gui() = default;

    std::filesystem::path documents_;
    std::unique_ptr<System> system_;
    std::unique_ptr<Renderer> renderer_;
    bool initialised_ = false;
};

class Surface
{
public:
    ~Surface();

    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    Rml::Context* context() const { return context_; }
    glm::ivec2 size() const { return size_; }
    // The page drawn by the last render(): premultiplied, sRGB-encoded
    // values (as the window's), RGBA8.
    SDL_GPUTexture* texture() const { return texture_; }

    // A document from the documents folder (a path in it), shown; null if
    // it can't be read. The surface keeps it until reload or destruction.
    Rml::ElementDocument* load(const std::string& path);
    // Reads the document (and, after Gui::forgetStyles, its style sheets)
    // again; returns the new one. The page's data models stay.
    Rml::ElementDocument* reload();
    Rml::ElementDocument* document() const { return document_; }

    // Layout and animations: once a frame, before render().
    void update();
    // Into texture(), outside any render pass.
    void render(SDL_GPUCommandBuffer* commands);

    // The pointer (mouse, or a ray on the panel) in the page's pixels.
    void pointerMove(glm::vec2 pixel);
    void pointerLeave();
    // Buttons: 0 left (a trigger), 1 right, 2 middle.
    void pointerButton(int button, bool down);
    // Scrolling, in lines (positive: down, right).
    void scroll(glm::vec2 lines);
    // Typed text (UTF-8), and keys (Backspace, arrows, Enter, ...); true if
    // the page used them.
    bool text(std::string_view utf8);
    bool key(SDL_Keycode key, SDL_Keymod modifiers, bool down);
    // The pointer is over something on the page (not just its empty body).
    bool pointerOver() const;
    // A text field has the keyboard.
    bool typing() const;

private:
    friend class Gui;
    Surface(Gui& gui, Rml::Context* context, glm::ivec2 size, SDL_GPUTexture* texture);

    Gui& gui_;
    Rml::Context* context_ = nullptr;
    glm::ivec2 size_{0};
    SDL_GPUTexture* texture_ = nullptr;
    std::string path_;
    Rml::ElementDocument* document_ = nullptr;
    int modifiers_ = 0; // RmlUi's, from the last key
};

} // namespace viewer::gui
