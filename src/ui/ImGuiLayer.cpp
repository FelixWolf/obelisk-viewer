#include "viewer/ui/ImGuiLayer.hpp"

#include "viewer/core/Log.hpp"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

namespace viewer::ui
{

std::unique_ptr<ImGuiLayer> ImGuiLayer::create(SDL_Window* window, render::Renderer& renderer)
{
    IMGUI_CHECKVERSION();
    ImGuiContext* context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui::StyleColorsDark();
    const float scale = SDL_GetWindowDisplayScale(window);
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;

    if(!ImGui_ImplSDL3_InitForSDLGPU(window))
    {
        log::error("ImGui_ImplSDL3_InitForSDLGPU failed");
        ImGui::DestroyContext(context);
        return nullptr;
    }

    ImGui_ImplSDLGPU3_InitInfo info;
    info.Device = renderer.device();
    info.ColorTargetFormat = renderer.swapchainFormat();
    info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    if(!ImGui_ImplSDLGPU3_Init(&info))
    {
        log::error("ImGui_ImplSDLGPU3_Init failed");
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext(context);
        return nullptr;
    }

    return std::unique_ptr<ImGuiLayer>(new ImGuiLayer(context));
}

ImGuiLayer::ImGuiLayer(ImGuiContext* context) : context_(context) {}

ImGuiLayer::~ImGuiLayer()
{
    ImGui::SetCurrentContext(context_);
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(context_);
}

bool ImGuiLayer::processEvent(const SDL_Event& event)
{
    ImGui::SetCurrentContext(context_);
    ImGui_ImplSDL3_ProcessEvent(&event);

    const ImGuiIO& io = ImGui::GetIO();
    switch(event.type)
    {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
        return io.WantCaptureMouse;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
        return io.WantCaptureKeyboard;
    default:
        return false;
    }
}

void ImGuiLayer::beginFrame()
{
    ImGui::SetCurrentContext(context_);
    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::endFrame()
{
    ImGui::SetCurrentContext(context_);
    ImGui::Render();
}

void ImGuiLayer::record(const render::Frame& frame)
{
    ImGui::SetCurrentContext(context_);
    ImDrawData* drawData = ImGui::GetDrawData();
    if(!drawData || drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f)
    {
        return;
    }

    // Uploads vertex/index buffers via a copy pass; must happen outside a render pass.
    ImGui_ImplSDLGPU3_PrepareDrawData(drawData, frame.commands);

    SDL_GPUColorTargetInfo color = {};
    color.texture = frame.target;
    color.load_op = SDL_GPU_LOADOP_LOAD; // draw over the scene
    color.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(frame.commands, &color, 1, nullptr);
    ImGui_ImplSDLGPU3_RenderDrawData(drawData, frame.commands, pass);
    SDL_EndGPURenderPass(pass);
}

} // namespace viewer::ui
