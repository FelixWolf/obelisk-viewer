// The media host: Chromium (through CEF) for the viewer's media on prims,
// in a program of its own so the viewer never links it and a crash in a
// page can't take the viewer down. Started by the viewer (MediaHost.cpp),
// which talks to it over our standard input and output
// (viewer/media/Protocol.hpp). Each tab is a windowless browser whose
// frames go into shared memory the viewer made, and whose sound comes back
// as samples. All tabs share one engine and its cache; each names its
// profile (cookies, storage), kept under the root the viewer gives.
//
// Pages go only where UrlPolicy allows: http and https, nowhere local.
// Nothing asks the person anything: popups open in the tab (when clicked),
// downloads, file pickers, permissions (camera, microphone, ...) and
// dialogs are refused. Exits when the viewer says Quit or goes away.
//
// On Windows it's a DLL that CEF's bootstrap.exe (named as we are) loads:
// that's how a program not built with Chromium gets its sandbox (CEF 138
// and later). The bootstrap calls RunWinMain, for every process.

#include "include/cef_app.h"
#include "include/cef_client.h"
#include "include/cef_parser.h"
#include "include/cef_request_context_handler.h"
#include "include/cef_version_info.h"

#include "viewer/media/Protocol.hpp"
#include "viewer/media/SharedMemory.hpp"
#include "viewer/media/UrlPolicy.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include "include/cef_sandbox_win.h"

#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/prctl.h>
#include <unistd.h>
#endif

namespace
{

using namespace viewer::media;
namespace fs = std::filesystem;

// A path from UTF-8 (the protocol's), and back.
fs::path pathOf(const std::string& utf8)
{
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string utf8Of(const fs::path& path)
{
    const std::u8string s = path.u8string();
    return std::string(s.begin(), s.end());
}

void note(const std::string& text)
{
    const std::string line = "[media host] " + text + "\n";
#if defined(_WIN32)
    // (A GUI process's C library may have no standard error; the handle
    // the viewer gave is there.)
    DWORD wrote = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line.data(), static_cast<DWORD>(line.size()), &wrote,
              nullptr);
#else
    std::fputs(line.c_str(), stderr);
#endif
}

// --- Our pipe to the viewer ---------------------------------------------------

#if defined(_WIN32)
HANDLE gIn = nullptr;
HANDLE gOut = nullptr;
#else
int gIn = -1;
int gOut = -1;
#endif
std::mutex gWriting;
std::atomic<bool> gViewerGone{false};

// Standard input and output are the viewer's pipe. Moved aside (close-on-
// exec), so Chromium's own processes, which inherit 0, 1 and 2, can't write
// into it or keep it open; they get /dev/null and our standard error.
void takePipe()
{
#if defined(_WIN32)
    gIn = GetStdHandle(STD_INPUT_HANDLE);
    gOut = GetStdHandle(STD_OUTPUT_HANDLE);
    SetHandleInformation(gIn, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(gOut, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul =
        CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, 0, nullptr);
    SetStdHandle(STD_INPUT_HANDLE, nul);
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    SetStdHandle(STD_OUTPUT_HANDLE, err && err != INVALID_HANDLE_VALUE ? err : nul);
#else
    gIn = fcntl(0, F_DUPFD_CLOEXEC, 3);
    gOut = fcntl(1, F_DUPFD_CLOEXEC, 3);
    const int null = open("/dev/null", O_RDWR | O_CLOEXEC);
    dup2(null, 0);
    dup2(2, 1);
    close(null);
#endif
}

// Some of `size` bytes written (0: the viewer's gone), or read (0: it ended).
size_t writeSome(const uint8_t* bytes, size_t size)
{
#if defined(_WIN32)
    DWORD wrote = 0;
    return WriteFile(gOut, bytes, static_cast<DWORD>(size), &wrote, nullptr) ? wrote : 0;
#else
    for(;;)
    {
        const ssize_t n = write(gOut, bytes, size);
        if(n < 0 && errno == EINTR)
        {
            continue;
        }
        return n > 0 ? static_cast<size_t>(n) : 0;
    }
#endif
}

size_t readSome(uint8_t* into, size_t size)
{
#if defined(_WIN32)
    DWORD got = 0;
    return ReadFile(gIn, into, static_cast<DWORD>(size), &got, nullptr) ? got : 0;
#else
    for(;;)
    {
        const ssize_t n = read(gIn, into, size);
        if(n < 0 && errno == EINTR)
        {
            continue;
        }
        return n > 0 ? static_cast<size_t>(n) : 0;
    }
#endif
}

void send(const Message& m)
{
    if(gViewerGone)
    {
        return;
    }
    const auto bytes = encode(m);
    std::lock_guard lock(gWriting);
    size_t at = 0;
    while(at < bytes.size())
    {
        const size_t n = writeSome(bytes.data() + at, bytes.size() - at);
        if(n == 0)
        {
            gViewerGone = true;
            return;
        }
        at += n;
    }
}

// The next message from the viewer, waiting for it; nothing once it's gone.
std::optional<Message> receive(Parser& parser)
{
    std::vector<uint8_t> buffer(64 * 1024);
    for(;;)
    {
        if(auto m = parser.next())
        {
            return m;
        }
        if(parser.broken())
        {
            note("the viewer said something that isn't a message");
            return std::nullopt;
        }
        const size_t n = readSome(buffer.data(), buffer.size());
        if(n == 0)
        {
            return std::nullopt;
        }
        parser.feed(std::span(buffer).first(n));
    }
}

// --- On CEF's UI thread ---------------------------------------------------------

class Task final : public CefTask
{
public:
    explicit Task(std::function<void()> f) : f_(std::move(f)) {}
    void Execute() override { f_(); }

private:
    std::function<void()> f_;
    IMPLEMENT_REFCOUNTING(Task);
};

void onUi(std::function<void()> f)
{
    CefPostTask(TID_UI, new Task(std::move(f)));
}

Init gInit;

class Tab;
// The UI thread's.
std::map<TabId, CefRefPtr<Tab>> gTabs;
std::map<std::string, CefRefPtr<CefRequestContext>> gProfiles;
std::mutex gTabsGone;
std::condition_variable gTabsGoneSignal;
size_t gTabCount = 0; // under gTabsGone (the main thread waits on it)

// A profile's folder name: its letters and digits (the viewer gives an
// account's id).
std::string profileFolder(const std::string& profile)
{
    std::string out;
    for(char c : profile)
    {
        if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')
        {
            out += c;
        }
    }
    return out.empty() ? "default" : out.substr(0, 64);
}

CefRefPtr<CefRequestContext> profileContext(const std::string& profile)
{
    const std::string folder = profileFolder(profile);
    auto& context = gProfiles[folder];
    if(!context)
    {
        CefRequestContextSettings settings;
        CefString(&settings.cache_path) = utf8Of(pathOf(gInit.root) / ("profile-" + folder));
        settings.persist_session_cookies = false;
        context = CefRequestContext::CreateContext(settings, nullptr);
    }
    return context;
}

class Tab final : public CefClient,
                  public CefRenderHandler,
                  public CefAudioHandler,
                  public CefLifeSpanHandler,
                  public CefDisplayHandler,
                  public CefLoadHandler,
                  public CefRequestHandler,
                  public CefResourceRequestHandler,
                  public CefJSDialogHandler,
                  public CefContextMenuHandler,
                  public CefPermissionHandler,
                  public CefDialogHandler,
                  public CefDownloadHandler
{
public:
    Tab(const Open& open, std::unique_ptr<SharedMemory> memory)
        : id_(open.tab), width_(open.width), height_(open.height), memory_(std::move(memory))
    {
    }

    TabId id() const { return id_; }
    // The mouse buttons held on it (CEF's EVENTFLAG_*_MOUSE_BUTTON).
    uint32_t buttons = 0;
    CefRefPtr<CefBrowser> browser() const { return browser_; }

    // The viewer's, on the UI thread.
    void navigate(const std::string& url)
    {
        if(browser_ && navigable(url, gInit.allowData))
        {
            browser_->GetMainFrame()->LoadURL(url);
        }
    }
    void resize(uint32_t width, uint32_t height, std::unique_ptr<SharedMemory> memory)
    {
        width_ = width;
        height_ = height;
        memory_ = std::move(memory);
        busy_ = {};
        pendingValid_ = false;
        send(Resized{id_});
        if(browser_)
        {
            browser_->GetHost()->WasResized();
        }
    }
    void release(uint32_t slot)
    {
        if(slot >= kFrameSlots)
        {
            return;
        }
        busy_[slot] = false;
        if(pendingValid_)
        {
            pendingValid_ = false;
            put(pending_.data());
        }
    }
    void close()
    {
        closing_ = true;
        if(browser_)
        {
            browser_->GetHost()->CloseBrowser(true);
        }
    }

    // --- CefClient
    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefAudioHandler> GetAudioHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
    CefRefPtr<CefJSDialogHandler> GetJSDialogHandler() override { return this; }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
    CefRefPtr<CefPermissionHandler> GetPermissionHandler() override { return this; }
    CefRefPtr<CefDialogHandler> GetDialogHandler() override { return this; }
    CefRefPtr<CefDownloadHandler> GetDownloadHandler() override { return this; }

    // --- Drawing: into a free slot of the viewer's memory (or kept until one
    // is free: only the latest), with an open popup (a <select>'s list) over
    // it.
    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override
    {
        rect = CefRect(0, 0, static_cast<int>(width_), static_cast<int>(height_));
    }
    bool GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo& info) override
    {
        info.device_scale_factor = 1.0f;
        info.rect = info.available_rect =
            CefRect(0, 0, static_cast<int>(width_), static_cast<int>(height_));
        return true;
    }
    void OnPopupShow(CefRefPtr<CefBrowser> browser, bool show) override
    {
        if(!show)
        {
            popup_ = CefRect();
            popupPixels_.clear();
            browser->GetHost()->Invalidate(PET_VIEW);
        }
    }
    void OnPopupSize(CefRefPtr<CefBrowser>, const CefRect& rect) override { popup_ = rect; }
    // What's selected, for the viewer to copy.
    void OnTextSelectionChanged(CefRefPtr<CefBrowser>, const CefString& text,
                                const CefRange&) override
    {
        std::string selected = text.ToString();
        if(selected.size() > kMaxSelection)
        {
            selected.resize(kMaxSelection); // (may cut a character: the viewer checks)
        }
        send(Selection{id_, std::move(selected)});
    }
    // A text field took the keyboard (or let it go).
    void OnVirtualKeyboardRequested(CefRefPtr<CefBrowser>, TextInputMode mode) override
    {
        send(Editing{id_, mode != CEF_TEXT_INPUT_MODE_NONE});
    }
    void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList&,
                 const void* buffer, int width, int height) override
    {
        if(type == PET_POPUP)
        {
            popupPixels_.assign(static_cast<const uint8_t*>(buffer),
                                static_cast<const uint8_t*>(buffer) +
                                    static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
            popupSize_ = CefSize(width, height);
            browser->GetHost()->Invalidate(PET_VIEW);
            return;
        }
        if(!memory_ || static_cast<uint32_t>(width) != width_ ||
           static_cast<uint32_t>(height) != height_)
        {
            return; // a frame of before a resize
        }
        if(!popupPixels_.empty())
        {
            pending_.assign(static_cast<const uint8_t*>(buffer),
                            static_cast<const uint8_t*>(buffer) + frameSlotSize(width_, height_));
            overlayPopup(pending_.data());
            buffer = pending_.data();
        }
        put(static_cast<const uint8_t*>(buffer));
    }

    // --- Sound: stereo at 48 kHz (the viewer's own rate), interleaved.
    bool GetAudioParameters(CefRefPtr<CefBrowser>, CefAudioParameters& params) override
    {
        params.channel_layout = CEF_CHANNEL_LAYOUT_STEREO;
        params.sample_rate = 48000;
        params.frames_per_buffer = 480;
        return true;
    }
    void OnAudioStreamStarted(CefRefPtr<CefBrowser>, const CefAudioParameters& params,
                              int channels) override
    {
        channels_ = static_cast<uint32_t>(std::max(channels, 1));
        send(AudioStart{id_, static_cast<uint32_t>(params.sample_rate), channels_});
    }
    void OnAudioStreamPacket(CefRefPtr<CefBrowser>, const float** data, int frames,
                             int64_t) override
    {
        Audio a{id_, {}};
        a.samples.resize(static_cast<size_t>(frames) * channels_);
        for(int f = 0; f < frames; f++)
        {
            for(uint32_t c = 0; c < channels_; c++)
            {
                a.samples[static_cast<size_t>(f) * channels_ + c] = data[c][f];
            }
        }
        send(a);
    }
    void OnAudioStreamStopped(CefRefPtr<CefBrowser>) override { send(AudioStop{id_}); }
    void OnAudioStreamError(CefRefPtr<CefBrowser>, const CefString& message) override
    {
        if(!closing_)
        {
            note(std::format("tab {}: sound: {}", id_, message.ToString()));
        }
    }

    // --- Its life.
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override
    {
        browser_ = browser;
        if(closing_)
        {
            browser_->GetHost()->CloseBrowser(true);
        }
    }
    bool OnBeforePopup(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>, int,
                       const CefString& target, const CefString&, cef_window_open_disposition_t,
                       bool userGesture, const CefPopupFeatures&, CefWindowInfo&,
                       CefRefPtr<CefClient>&, CefBrowserSettings&, CefRefPtr<CefDictionaryValue>&,
                       bool*) override
    {
        // No windows: what a click opens shows here instead.
        if(userGesture && navigable(target.ToString(), gInit.allowData))
        {
            browser->GetMainFrame()->LoadURL(target);
        }
        return true;
    }
    void OnBeforeClose(CefRefPtr<CefBrowser>) override
    {
        browser_ = nullptr;
        send(Closed{id_});
        CefRefPtr<Tab> keep = this; // erasing drops gTabs' reference
        gTabs.erase(id_);
        std::lock_guard lock(gTabsGone);
        gTabCount = gTabs.size();
        gTabsGoneSignal.notify_all();
    }

    // --- What it shows.
    void OnAddressChange(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
                         const CefString& url) override
    {
        if(frame->IsMain())
        {
            send(Address{id_, url.ToString()});
        }
    }
    void OnTitleChange(CefRefPtr<CefBrowser>, const CefString& title) override
    {
        send(Title{id_, title.ToString()});
    }
    bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t, const CefString&,
                          const CefString&, int) override
    {
        return true; // pages' logs aren't ours
    }
    void OnLoadingStateChange(CefRefPtr<CefBrowser>, bool loading, bool, bool) override
    {
        send(Loading{id_, loading});
    }

    // --- Where it goes.
    bool OnBeforeBrowse(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefRequest> request, bool, bool) override
    {
        const std::string url = request->GetURL().ToString();
        return frame->IsMain() ? !navigable(url, gInit.allowData) : !loadable(url, gInit.allowData);
    }
    bool OnOpenURLFromTab(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>,
                          const CefString& target, cef_window_open_disposition_t,
                          bool userGesture) override
    {
        if(userGesture && navigable(target.ToString(), gInit.allowData))
        {
            browser->GetMainFrame()->LoadURL(target);
        }
        return true;
    }
    CefRefPtr<CefResourceRequestHandler>
    GetResourceRequestHandler(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest>,
                              bool, bool, const CefString&, bool&) override
    {
        return this;
    }
    ReturnValue OnBeforeResourceLoad(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                     CefRefPtr<CefRequest> request, CefRefPtr<CefCallback>) override
    {
        return loadable(request->GetURL().ToString(), gInit.allowData) ? RV_CONTINUE : RV_CANCEL;
    }
    void OnRenderProcessTerminated(CefRefPtr<CefBrowser>, TerminationStatus, int,
                                   const CefString&) override
    {
        if(!closing_)
        {
            send(Crashed{id_});
        }
    }

    // --- Nothing asks the person anything.
    bool OnJSDialog(CefRefPtr<CefBrowser>, const CefString&, JSDialogType, const CefString&,
                    const CefString&, CefRefPtr<CefJSDialogCallback>, bool& suppress) override
    {
        suppress = true;
        return false;
    }
    bool OnBeforeUnloadDialog(CefRefPtr<CefBrowser>, const CefString&, bool,
                              CefRefPtr<CefJSDialogCallback> callback) override
    {
        callback->Continue(true, CefString());
        return true;
    }
    void OnBeforeContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                             CefRefPtr<CefContextMenuParams>,
                             CefRefPtr<CefMenuModel> model) override
    {
        model->Clear();
    }
    bool OnRequestMediaAccessPermission(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                        const CefString&, uint32_t,
                                        CefRefPtr<CefMediaAccessCallback> callback) override
    {
        callback->Cancel();
        return true;
    }
    bool OnShowPermissionPrompt(CefRefPtr<CefBrowser>, uint64_t, const CefString&, uint32_t,
                                CefRefPtr<CefPermissionPromptCallback> callback) override
    {
        callback->Continue(CEF_PERMISSION_RESULT_DENY);
        return true;
    }
    bool OnFileDialog(CefRefPtr<CefBrowser>, FileDialogMode, const CefString&, const CefString&,
                      const std::vector<CefString>&, const std::vector<CefString>&,
                      const std::vector<CefString>&,
                      CefRefPtr<CefFileDialogCallback> callback) override
    {
        callback->Cancel();
        return true;
    }
    bool CanDownload(CefRefPtr<CefBrowser>, const CefString&, const CefString&) override
    {
        return false;
    }
    bool OnBeforeDownload(CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem>, const CefString&,
                          CefRefPtr<CefBeforeDownloadCallback>) override
    {
        return false;
    }
    bool GetAuthCredentials(CefRefPtr<CefBrowser>, const CefString&, bool, const CefString&, int,
                            const CefString&, const CefString&, CefRefPtr<CefAuthCallback>) override
    {
        return false;
    }

private:
    // A whole frame (BGRA, top row first) into a free slot, else kept.
    void put(const uint8_t* pixels)
    {
        for(uint32_t slot = 0; slot < kFrameSlots; slot++)
        {
            if(!busy_[slot])
            {
                busy_[slot] = true;
                std::memcpy(memory_->data() + frameSlotOffset(slot, width_, height_), pixels,
                            frameSlotSize(width_, height_));
                send(Frame{id_, slot, width_, height_});
                return;
            }
        }
        if(pixels != pending_.data())
        {
            pending_.assign(pixels, pixels + frameSlotSize(width_, height_));
        }
        pendingValid_ = true;
    }

    void overlayPopup(uint8_t* view) const
    {
        const int w = static_cast<int>(width_);
        const int h = static_cast<int>(height_);
        for(int y = 0; y < popupSize_.height; y++)
        {
            const int vy = popup_.y + y;
            if(vy < 0 || vy >= h)
            {
                continue;
            }
            for(int x = 0; x < popupSize_.width; x++)
            {
                const int vx = popup_.x + x;
                if(vx >= 0 && vx < w)
                {
                    std::memcpy(
                        view + (static_cast<size_t>(vy) * width_ + static_cast<size_t>(vx)) * 4,
                        popupPixels_.data() +
                            (static_cast<size_t>(y) * static_cast<size_t>(popupSize_.width) +
                             static_cast<size_t>(x)) *
                                4,
                        4);
                }
            }
        }
    }

    const TabId id_;
    uint32_t width_;
    uint32_t height_;
    std::unique_ptr<SharedMemory> memory_;
    std::array<bool, kFrameSlots> busy_{};
    std::vector<uint8_t> pending_;
    bool pendingValid_ = false;
    CefRect popup_;
    CefSize popupSize_;
    std::vector<uint8_t> popupPixels_;
    uint32_t channels_ = 2;
    CefRefPtr<CefBrowser> browser_;
    bool closing_ = false;
    IMPLEMENT_REFCOUNTING(Tab);
};

std::unique_ptr<SharedMemory> frameMemory(const std::string& name, uint32_t width, uint32_t height)
{
    if(width == 0 || height == 0 || width > kMaxFrameSide || height > kMaxFrameSide)
    {
        return nullptr;
    }
    auto memory = SharedMemory::open(name, frameMemorySize(width, height));
    if(memory)
    {
        memory->forget(); // nobody else needs to find it
    }
    return memory;
}

void openTab(const Open& open)
{
    if(gTabs.contains(open.tab))
    {
        return;
    }
    auto memory = frameMemory(open.memory, open.width, open.height);
    if(!memory)
    {
        note(std::format("tab {}: no memory to draw into", open.tab));
        send(Closed{open.tab});
        return;
    }
    CefRefPtr<Tab> tab = new Tab(open, std::move(memory));
    gTabs[open.tab] = tab;
    {
        std::lock_guard lock(gTabsGone);
        gTabCount = gTabs.size();
    }
    CefWindowInfo window;
    window.SetAsWindowless(0);
    CefBrowserSettings settings;
    settings.windowless_frame_rate = static_cast<int>(std::clamp(open.frameRate, 1u, 60u));
    settings.background_color = 0xFFFFFFFF;
    const std::string url =
        navigable(open.url, gInit.allowData) ? open.url : std::string("about:blank");
    CefBrowserHost::CreateBrowser(window, tab, url, settings, nullptr,
                                  profileContext(open.profile));
}

// What the viewer says, on the UI thread.
void handle(const Message& message)
{
    auto tab = [&](TabId id) -> Tab*
    {
        const auto it = gTabs.find(id);
        return it == gTabs.end() ? nullptr : it->second.get();
    };
    std::visit(
        [&](const auto& m)
        {
            using T = std::decay_t<decltype(m)>;
            if constexpr(std::is_same_v<T, Open>)
            {
                openTab(m);
            }
            else if constexpr(std::is_same_v<T, Navigate>)
            {
                if(Tab* t = tab(m.tab))
                {
                    t->navigate(m.url);
                }
            }
            else if constexpr(std::is_same_v<T, Resize>)
            {
                if(Tab* t = tab(m.tab))
                {
                    if(auto memory = frameMemory(m.memory, m.width, m.height))
                    {
                        t->resize(m.width, m.height, std::move(memory));
                    }
                }
            }
            else if constexpr(std::is_same_v<T, Close>)
            {
                if(Tab* t = tab(m.tab))
                {
                    t->close();
                }
                else
                {
                    send(Closed{m.tab});
                }
            }
            else if constexpr(std::is_same_v<T, Mouse>)
            {
                Tab* t = tab(m.tab);
                if(!t || !t->browser())
                {
                    return;
                }
                CefMouseEvent e;
                e.x = m.x;
                e.y = m.y;
                // Every event says which buttons are held: a move without
                // them isn't a drag to Chromium (its scrollbars, selecting
                // text), however the page's own scripts take it.
                const uint32_t flag = m.button == MouseButton::Left ? EVENTFLAG_LEFT_MOUSE_BUTTON
                                      : m.button == MouseButton::Right
                                          ? EVENTFLAG_RIGHT_MOUSE_BUTTON
                                          : EVENTFLAG_MIDDLE_MOUSE_BUTTON;
                if(m.kind == MouseKind::Down)
                {
                    t->buttons |= flag;
                }
                e.modifiers = t->buttons;
                if(m.kind == MouseKind::Up)
                {
                    t->buttons &= ~flag;
                }
                auto host = t->browser()->GetHost();
                const auto button = m.button == MouseButton::Left    ? MBT_LEFT
                                    : m.button == MouseButton::Right ? MBT_RIGHT
                                                                     : MBT_MIDDLE;
                switch(m.kind)
                {
                case MouseKind::Move:
                    host->SendMouseMoveEvent(e, false);
                    break;
                case MouseKind::Leave:
                    host->SendMouseMoveEvent(e, true);
                    break;
                case MouseKind::Down:
                    host->SetFocus(true);
                    host->SendMouseClickEvent(e, button, false, std::max<int>(m.clicks, 1));
                    break;
                case MouseKind::Up:
                    host->SendMouseClickEvent(e, button, true, std::max<int>(m.clicks, 1));
                    break;
                }
            }
            else if constexpr(std::is_same_v<T, Wheel>)
            {
                Tab* t = tab(m.tab);
                if(t && t->browser())
                {
                    CefMouseEvent e;
                    e.x = m.x;
                    e.y = m.y;
                    // (CEF's: x > 0 scrolls left.)
                    t->browser()->GetHost()->SendMouseWheelEvent(e, -m.dx, m.dy);
                }
            }
            else if constexpr(std::is_same_v<T, Visibility>)
            {
                Tab* t = tab(m.tab);
                if(t && t->browser())
                {
                    t->browser()->GetHost()->WasHidden(m.hidden);
                    t->browser()->GetHost()->SetWindowlessFrameRate(
                        static_cast<int>(std::clamp(m.frameRate, 1u, 60u)));
                }
            }
            else if constexpr(std::is_same_v<T, Release>)
            {
                if(Tab* t = tab(m.tab))
                {
                    t->release(m.slot);
                }
            }
            else if constexpr(std::is_same_v<T, Focus>)
            {
                Tab* t = tab(m.tab);
                if(t && t->browser())
                {
                    t->browser()->GetHost()->SetFocus(m.focused);
                }
            }
            else if constexpr(std::is_same_v<T, Key>)
            {
                Tab* t = tab(m.tab);
                if(!t || !t->browser())
                {
                    return;
                }
                CefKeyEvent e;
                e.type = m.kind == KeyKind::Down ? KEYEVENT_RAWKEYDOWN
                         : m.kind == KeyKind::Up ? KEYEVENT_KEYUP
                                                 : KEYEVENT_CHAR;
                e.windows_key_code = static_cast<int>(m.keyCode);
                e.character = e.unmodified_character = static_cast<char16_t>(m.character);
                using namespace key_flags;
                const std::pair<uint32_t, uint32_t> flags[] = {
                    {kShift, EVENTFLAG_SHIFT_DOWN},      {kControl, EVENTFLAG_CONTROL_DOWN},
                    {kAlt, EVENTFLAG_ALT_DOWN},          {kMeta, EVENTFLAG_COMMAND_DOWN},
                    {kKeypad, EVENTFLAG_IS_KEY_PAD},     {kRepeat, EVENTFLAG_IS_REPEAT},
                    {kCapsLock, EVENTFLAG_CAPS_LOCK_ON}, {kNumLock, EVENTFLAG_NUM_LOCK_ON}};
                for(const auto& [ours, cefs] : flags)
                {
                    e.modifiers |= (m.modifiers & ours) ? cefs : 0;
                }
                t->browser()->GetHost()->SendKeyEvent(e);
            }
            else if constexpr(std::is_same_v<T, Text>)
            {
                Tab* t = tab(m.tab);
                if(t && t->browser() && !m.text.empty())
                {
                    t->browser()->GetHost()->ImeCommitText(m.text, CefRange::InvalidRange(), 0);
                }
            }
        },
        message);
}

std::atomic<bool> gContextReady{false};

class App final : public CefApp, public CefBrowserProcessHandler
{
public:
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
    void OnBeforeCommandLineProcessing(const CefString& type,
                                       CefRefPtr<CefCommandLine> line) override
    {
        if(!type.empty())
        {
            return;
        }
        // No windows anywhere, so no display server needed (on a headset
        // or Wayland alike); the GPU is used when there is one.
        line->AppendSwitchWithValue("ozone-platform", "headless");
        // Prims' media starts by itself when the viewer says so (it decides).
        line->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");
        // Nothing of Chrome's own talks to the network in the background.
        line->AppendSwitch("disable-background-networking");
        line->AppendSwitch("disable-component-update");
        line->AppendSwitch("no-default-browser-check");
        // Not the system's keyring (it would ask for a password).
        line->AppendSwitchWithValue("password-store", "basic");
    }
    void OnContextInitialized() override { gContextReady = true; }
    // Another host on our root: not ours to serve (the viewer gives each
    // its own root).
    bool OnAlreadyRunningAppRelaunch(CefRefPtr<CefCommandLine>, const CefString&) override
    {
        return true;
    }

private:
    IMPLEMENT_REFCOUNTING(App);
};

} // namespace

namespace
{

// Every process: Chromium's own (renderers, GPU, ...) are this program too.
// `sandbox`: CEF's bootstrap's (Windows), null elsewhere.
//
// Without a stack protector, as main(): on Linux Chromium's zygote forks
// its processes from inside CefExecuteProcess, each with a new stack canary,
// so any function on the stack then would find its canary changed when the
// process returns through it ("stack smashing detected", an abort).
NO_STACK_PROTECTOR int run(const CefMainArgs& args, void* sandbox)
{
    CefRefPtr<App> app = new App;
    const int code = CefExecuteProcess(args, app, sandbox);
    if(code >= 0)
    {
        return code;
    }

    // Nothing of the viewer's but the pipe and its log; gone with it
    // (Windows: the viewer's job object ends us).
#if !defined(_WIN32)
    close_range(3, ~0u, 0);
#endif
    takePipe();
#if !defined(_WIN32)
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if(getppid() == 1)
    {
        return 0; // it's gone already
    }
#endif

    Parser parser;
    const auto first = receive(parser);
    if(!first || !std::holds_alternative<Init>(*first))
    {
        note("the viewer didn't say how to start");
        return 2;
    }
    gInit = std::get<Init>(*first);
    if(gInit.version != kProtocolVersion || gInit.root.empty())
    {
        send(Failed{std::format("protocol {} isn't ours ({})", gInit.version, kProtocolVersion)});
        return 2;
    }

    std::error_code ec;
    const fs::path root = pathOf(gInit.root);
    fs::create_directories(root, ec);
    const fs::path log = root / "cef.log";
    if(fs::file_size(log, ec) > (4u << 20))
    {
        fs::remove(log, ec); // kept small (it's only errors)
    }

    CefSettings settings;
    settings.no_sandbox = !gInit.sandbox;
    settings.windowless_rendering_enabled = true;
    settings.multi_threaded_message_loop = true;
    settings.background_color = 0xFFFFFFFF;
    settings.log_severity = LOGSEVERITY_ERROR;
    CefString(&settings.log_file) = utf8Of(log);
    CefString(&settings.root_cache_path) = gInit.root;
    CefString(&settings.locale) = "en-US";
    if(!CefInitialize(args, settings, app, sandbox))
    {
        const int exit = CefGetExitCode();
        send(Failed{exit == CEF_RESULT_CODE_NORMAL_EXIT_PROCESS_NOTIFIED
                        ? std::string("another media host uses its folder")
                        : std::format("the engine didn't start ({})", exit)});
        return 3;
    }
    for(int i = 0; i < 300 && !gContextReady; i++)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    send(Ready{kProtocolVersion,
               std::format("CEF {}.{}.{} (Chromium {}.{}.{}.{})", cef_version_info(0),
                           cef_version_info(1), cef_version_info(2), cef_version_info(4),
                           cef_version_info(5), cef_version_info(6), cef_version_info(7))});

    while(auto m = receive(parser))
    {
        if(std::holds_alternative<Quit>(*m) || std::holds_alternative<Init>(*m))
        {
            break;
        }
        onUi([m = std::move(*m)] { handle(m); });
    }

    // Closing: every tab, then the engine.
    onUi(
        []
        {
            const auto tabs = gTabs;
            for(const auto& [id, tab] : tabs)
            {
                tab->close();
            }
        });
    {
        std::unique_lock lock(gTabsGone);
        gTabsGoneSignal.wait_for(lock, std::chrono::seconds(3), [] { return gTabCount == 0; });
    }
    gViewerGone = true; // nothing more to say
    std::mutex done;
    std::condition_variable doneSignal;
    bool cleared = false;
    onUi(
        [&]
        {
            gTabs.clear();
            gProfiles.clear();
            std::lock_guard lock(done);
            cleared = true;
            doneSignal.notify_all();
        });
    {
        std::unique_lock lock(done);
        doneSignal.wait_for(lock, std::chrono::seconds(2), [&] { return cleared; });
    }
    CefShutdown();
    return 0;
}

} // namespace

#if defined(_WIN32)

// Called by the bootstrap (a Windows program: no console).
int RunWinMain(HINSTANCE instance, LPWSTR, int, void* sandbox, cef_version_info_t*)
{
    return run(CefMainArgs(instance), sandbox);
}

// And by its console twin, which isn't used, but is what it'd call.
int RunConsoleMain(int, char*[], void* sandbox, cef_version_info_t*)
{
    return run(CefMainArgs(GetModuleHandleW(nullptr)), sandbox);
}

#else

NO_STACK_PROTECTOR int main(int argc, char* argv[])
{
    return run(CefMainArgs(argc, argv), nullptr);
}

#endif
