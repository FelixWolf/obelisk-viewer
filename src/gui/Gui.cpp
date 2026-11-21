#include "viewer/gui/Gui.hpp"

#include "GuiRenderer.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/text/Font.hpp"

#include <RmlUi/Core.h>

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_timer.h>

#include <glm/gtc/matrix_transform.hpp>

namespace viewer::gui
{

// RmlUi's view of the system: the time, its log, the cursor it wants, the
// clipboard, and the keyboard (a text field focused).
class System final : public Rml::SystemInterface
{
public:
    double GetElapsedTime() override { return static_cast<double>(SDL_GetTicksNS()) * 1e-9; }

    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override
    {
        switch(type)
        {
        case Rml::Log::LT_ERROR:
        case Rml::Log::LT_ASSERT:
            log::error("interface: {}", message);
            break;
        case Rml::Log::LT_WARNING:
            log::warn("interface: {}", message);
            break;
        case Rml::Log::LT_INFO:
            log::info("interface: {}", message);
            break;
        default:
            break;
        }
        return true;
    }

    // Pages' links: res://PATH from the documents folder's top, other URIs
    // (asset://...) as they are, paths from the page's folder.
    void JoinPath(Rml::String& joined, const Rml::String& documentPath,
                  const Rml::String& path) override
    {
        if(path.starts_with("res://"))
        {
            joined = (documents / path.substr(6)).generic_string();
            return;
        }
        if(path.find("://") != Rml::String::npos)
        {
            joined = path;
            return;
        }
        Rml::SystemInterface::JoinPath(joined, documentPath, path);
    }

    void SetClipboardText(const Rml::String& text) override { SDL_SetClipboardText(text.c_str()); }
    void GetClipboardText(Rml::String& text) override
    {
        char* clipboard = SDL_GetClipboardText();
        text = clipboard ? clipboard : "";
        SDL_free(clipboard);
    }
    void ActivateKeyboard(Rml::Vector2f /*caret*/, float /*lineHeight*/) override
    {
        if(keyboard)
        {
            keyboard(true);
        }
    }
    void DeactivateKeyboard() override
    {
        if(keyboard)
        {
            keyboard(false);
        }
    }

    std::filesystem::path documents;
    std::function<void(bool)> keyboard;
};

namespace
{

Rml::Input::KeyIdentifier keyIdentifier(SDL_Keycode key)
{
    using namespace Rml::Input;
    if(key >= SDLK_A && key <= SDLK_Z)
    {
        return static_cast<KeyIdentifier>(KI_A + static_cast<int>(key - SDLK_A));
    }
    if(key >= SDLK_0 && key <= SDLK_9)
    {
        return static_cast<KeyIdentifier>(KI_0 + static_cast<int>(key - SDLK_0));
    }
    switch(key)
    {
    case SDLK_SPACE:
        return KI_SPACE;
    case SDLK_BACKSPACE:
        return KI_BACK;
    case SDLK_TAB:
        return KI_TAB;
    case SDLK_RETURN:
        return KI_RETURN;
    case SDLK_KP_ENTER:
        return KI_NUMPADENTER;
    case SDLK_ESCAPE:
        return KI_ESCAPE;
    case SDLK_PAGEUP:
        return KI_PRIOR;
    case SDLK_PAGEDOWN:
        return KI_NEXT;
    case SDLK_END:
        return KI_END;
    case SDLK_HOME:
        return KI_HOME;
    case SDLK_LEFT:
        return KI_LEFT;
    case SDLK_UP:
        return KI_UP;
    case SDLK_RIGHT:
        return KI_RIGHT;
    case SDLK_DOWN:
        return KI_DOWN;
    case SDLK_DELETE:
        return KI_DELETE;
    default:
        return KI_UNKNOWN;
    }
}

int keyModifiers(SDL_Keymod mod)
{
    int out = 0;
    if(mod & SDL_KMOD_CTRL)
    {
        out |= Rml::Input::KM_CTRL;
    }
    if(mod & SDL_KMOD_SHIFT)
    {
        out |= Rml::Input::KM_SHIFT;
    }
    if(mod & SDL_KMOD_ALT)
    {
        out |= Rml::Input::KM_ALT;
    }
    if(mod & SDL_KMOD_GUI)
    {
        out |= Rml::Input::KM_META;
    }
    return out;
}

} // namespace

std::unique_ptr<Gui> Gui::create(SDL_GPUDevice* device, const Options& options)
{
    std::unique_ptr<Gui> gui(new Gui);
    gui->documents_ = options.documents;
    gui->system_ = std::make_unique<System>();
    gui->system_->documents = options.documents;
    gui->renderer_ = std::make_unique<Renderer>(device);
    if(!gui->renderer_->create(options.windowFormat))
    {
        return nullptr;
    }
    Rml::SetSystemInterface(gui->system_.get());
    Rml::SetRenderInterface(gui->renderer_.get());
    if(!Rml::Initialise())
    {
        log::error("starting RmlUi failed");
        return nullptr;
    }
    gui->initialised_ = true;
    // The built-in font (kept by the text module for as long as the
    // program runs, as RmlUi needs).
    const std::span<const uint8_t> inter = text::Font::builtinData();
    if(!Rml::LoadFontFace(Rml::Span<const Rml::byte>(inter.data(), inter.size()), "Inter",
                          Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Auto, true))
    {
        log::error("the interface's font didn't load");
    }
    log::info("interface: RmlUi {}, documents in {}", Rml::GetVersion(), gui->documents_.string());
    return gui;
}

Gui::~Gui()
{
    if(initialised_)
    {
        Rml::Shutdown();
    }
}

void Gui::setKeyboard(std::function<void(bool)> keyboard)
{
    system_->keyboard = std::move(keyboard);
}

void Gui::addImageScheme(const std::string& scheme,
                         std::function<std::optional<SchemeImage>(std::string_view)> resolve)
{
    renderer_->addScheme(scheme, std::move(resolve));
}

void Gui::setImage(const std::string& name, Pixels pixels)
{
    renderer_->setImage(name, std::move(pixels));
}

void Gui::removeImage(const std::string& name)
{
    renderer_->removeImage(name);
}

void Gui::forgetStyles()
{
    Rml::Factory::ClearStyleSheetCache();
    Rml::Factory::ClearTemplateCache();
}

std::unique_ptr<Surface> Gui::createSurface(const std::string& name, glm::ivec2 size)
{
    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = Renderer::kPageFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    info.width = static_cast<Uint32>(size.x);
    info.height = static_cast<Uint32>(size.y);
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    Rml::Context* context = Rml::CreateContext(name, {size.x, size.y}, renderer_.get());
    if(!context)
    {
        log::error("the interface page {} couldn't be made", name);
        return nullptr;
    }
    SDL_GPUTexture* texture = SDL_CreateGPUTexture(renderer_->device(), &info);
    if(!texture)
    {
        log::error("the interface page {}'s image: {}", name, SDL_GetError());
        Rml::RemoveContext(name);
        return nullptr;
    }
    return std::unique_ptr<Surface>(new Surface(*this, context, size, texture));
}

void Gui::drawOver(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target, glm::uvec2 targetSize,
                   const Surface& surface, const glm::vec4& rect)
{
    if(targetSize.x == 0 || targetSize.y == 0)
    {
        return;
    }
    const glm::vec2 size(targetSize);
    const glm::vec2 topLeft = glm::vec2(rect.x, rect.y) / size;
    const glm::vec2 bottomRight = glm::vec2(rect.x + rect.z, rect.y + rect.w) / size;
    // Pixels (y down) to clip space (y up).
    const glm::vec4 clip(topLeft.x * 2.0f - 1.0f, 1.0f - topLeft.y * 2.0f,
                         bottomRight.x * 2.0f - 1.0f, 1.0f - bottomRight.y * 2.0f);
    renderer_->blit(commands, target, surface.texture(), clip);
}

Surface::Surface(Gui& gui, Rml::Context* context, glm::ivec2 size, SDL_GPUTexture* texture)
    : gui_(gui), context_(context), size_(size), texture_(texture)
{
}

Surface::~Surface()
{
    Rml::RemoveContext(context_->GetName());
    SDL_ReleaseGPUTexture(gui_.renderer_->device(), texture_);
}

Rml::ElementDocument* Surface::load(const std::string& path)
{
    if(document_)
    {
        context_->UnloadDocument(document_);
        document_ = nullptr;
    }
    path_ = path;
    const std::filesystem::path full = gui_.documents() / path;
    document_ = context_->LoadDocument(full.string());
    if(document_)
    {
        document_->Show();
    }
    else
    {
        log::warn("interface page {} didn't load", full.string());
    }
    return document_;
}

Rml::ElementDocument* Surface::reload()
{
    return path_.empty() ? nullptr : load(path_);
}

void Surface::update()
{
    context_->Update();
}

void Surface::render(SDL_GPUCommandBuffer* commands)
{
    Renderer& renderer = *gui_.renderer_;
    renderer.begin(size_);
    context_->Render();
    renderer.record(commands, texture_);
}

void Surface::pointerMove(glm::vec2 pixel)
{
    context_->ProcessMouseMove(static_cast<int>(pixel.x), static_cast<int>(pixel.y), modifiers_);
}

void Surface::pointerLeave()
{
    context_->ProcessMouseLeave();
}

void Surface::pointerButton(int button, bool down)
{
    if(down)
    {
        context_->ProcessMouseButtonDown(button, modifiers_);
    }
    else
    {
        context_->ProcessMouseButtonUp(button, modifiers_);
    }
}

void Surface::scroll(glm::vec2 lines)
{
    context_->ProcessMouseWheel(Rml::Vector2f(lines.x, lines.y), modifiers_);
}

bool Surface::text(std::string_view utf8)
{
    return !context_->ProcessTextInput(Rml::String(utf8));
}

bool Surface::key(SDL_Keycode key, SDL_Keymod modifiers, bool down)
{
    modifiers_ = keyModifiers(modifiers);
    const Rml::Input::KeyIdentifier id = keyIdentifier(key);
    if(id == Rml::Input::KI_UNKNOWN)
    {
        return false;
    }
    // RmlUi answers whether the key went on to the rest of the program.
    return down ? !context_->ProcessKeyDown(id, modifiers_)
                : !context_->ProcessKeyUp(id, modifiers_);
}

bool Surface::pointerOver() const
{
    const Rml::Element* hover = context_->GetHoverElement();
    return hover && hover != context_->GetRootElement() && hover != document_;
}

std::string Surface::cursor() const
{
    // Each page's own (RmlUi tells the system one cursor for all pages).
    const Rml::Element* hover = context_->GetHoverElement();
    return hover ? hover->GetComputedValues().cursor() : std::string();
}

bool Surface::typing() const
{
    const Rml::Element* focus = context_->GetFocusElement();
    return focus && (focus->GetTagName() == "input" || focus->GetTagName() == "textarea");
}

} // namespace viewer::gui
