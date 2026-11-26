// Drives the media host as the viewer does, without the viewer: starts it,
// opens tabs on a page of its own (a data: URL: no network), and checks
// that frames and sound come back, that clicks reach the page, and that
// it closes cleanly. Writes the last frame of the first tab as a PNG.
//
//   mediatest [--tabs N] [--seconds S] [--png OUT.png] [--url URL] [--wait-ms MS]
//
// --wait-ms: how long a page is given to load before it's typed into (1500;
// longer under Wine).

#include "viewer/image/Png.hpp"
#include "viewer/media/MediaHost.hpp"
#include "viewer/media/SharedMemory.hpp"

#include <SDL3/SDL_filesystem.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <map>
#include <print>
#include <string>
#include <thread>

#include <unistd.h>

using namespace viewer;
using namespace viewer::media;
namespace fs = std::filesystem;

namespace
{

// Changes color every frame; the title says where it was last clicked; a
// tone plays.
constexpr const char* kPage =
    "data:text/html,<body style='margin:0'><canvas id=c width=640 height=360></canvas><script>"
    "let x=c.getContext('2d'),f=0;(function d(){f++;x.fillStyle=`hsl(${f%25360},80%25,50%25)`;"
    "x.fillRect(0,0,640,360);x.fillStyle='white';x.font='64px sans-serif';"
    "x.fillText('frame '+f,40,120);requestAnimationFrame(d)})();"
    "document.onmousedown=e=>document.title='click '+e.clientX+','+e.clientY;"
    "let a=new AudioContext(),o=a.createOscillator();o.connect(a.destination);o.start();"
    "</script>";

// A text field with the keyboard; the title says what it holds, and the
// keys pressed (key code, key) and Enter (submitted).
constexpr const char* kTypingPage =
    "data:text/html,<body style='margin:0'><input id=i style='font-size:40px;width:600px'>"
    "<script>let k=[],s='';const t=()=>document.title='['+i.value+'] '+k.join(' ')+s;"
    "i.onkeydown=e=>{k.push(e.keyCode+':'+e.key+(e.shiftKey?'+S':''));t()};"
    "i.oninput=t;i.onkeypress=e=>{if(e.key=='Enter'){s=' submitted';t()}};"
    "</script>";

struct Tab
{
    std::unique_ptr<SharedMemory> memory;
    int frames = 0;
    std::string title;
    std::string address;
    bool closed = false;
    bool audioStarted = false;
};

} // namespace

int main(int argc, char** argv)
{
    int tabs = 2;
    int seconds = 4;
    fs::path png;
    std::string url = kPage;
    int waitMs = 1500;
    for(int i = 1; i + 1 < argc; i += 2)
    {
        const std::string a = argv[i];
        if(a == "--tabs")
        {
            tabs = std::atoi(argv[i + 1]);
        }
        else if(a == "--seconds")
        {
            seconds = std::atoi(argv[i + 1]);
        }
        else if(a == "--png")
        {
            png = argv[i + 1];
        }
        else if(a == "--url")
        {
            url = argv[i + 1];
        }
        else if(a == "--wait-ms")
        {
            waitMs = std::atoi(argv[i + 1]);
        }
    }
    const char* base = SDL_GetBasePath();
#if defined(_WIN32)
    const char* suffix = ".exe";
#else
    const char* suffix = "";
#endif
    const fs::path program =
        fs::path(base ? base : ".") / "media" / (std::string("obelisk-media") + suffix);
    const fs::path root = fs::temp_directory_path() / std::format("mediatest-{}", getpid());
    const std::u8string rootUtf8 = root.u8string();

    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    auto host = startMediaHost(
        program, Init{kProtocolVersion, std::string(rootUtf8.begin(), rootUtf8.end()), true, true});
    if(!host)
    {
        std::println(stderr, "FAIL: {} didn't start", program.string());
        return 1;
    }
    while(host->state() == MediaHost::State::Starting &&
          Clock::now() - started < std::chrono::seconds(20))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if(host->state() != MediaHost::State::Ready)
    {
        std::println(stderr, "FAIL: not ready: {}", host->error());
        return 1;
    }
    std::vector<Message> said;
    host->take(said);
    for(const Message& m : said)
    {
        if(const auto* r = std::get_if<Ready>(&m))
        {
            std::println(
                "ready after {} ms: {}",
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started)
                    .count(),
                r->engine);
        }
    }

    constexpr uint32_t kWidth = 640, kHeight = 360;
    std::map<TabId, Tab> open;
    for(TabId id = 1; id <= static_cast<TabId>(tabs); id++)
    {
        Tab t;
        t.memory = SharedMemory::create("mediatest", frameMemorySize(kWidth, kHeight));
        if(!t.memory)
        {
            std::println(stderr, "FAIL: no shared memory");
            return 1;
        }
        host->send(Open{id, "test", url, kWidth, kHeight, t.memory->name(), 30});
        open[id] = std::move(t);
    }

    std::vector<uint8_t> last;
    bool clicked = false;
    const auto until = Clock::now() + std::chrono::seconds(seconds);
    auto pump = [&]
    {
        host->take(said);
        for(const Message& m : said)
        {
            std::visit(
                [&](const auto& v)
                {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr(std::is_same_v<T, Frame>)
                    {
                        Tab& t = open[v.tab];
                        t.frames++;
                        if(v.tab == 1)
                        {
                            const uint8_t* p =
                                t.memory->data() + frameSlotOffset(v.slot, v.width, v.height);
                            last.assign(p, p + frameSlotSize(v.width, v.height));
                        }
                        host->send(Release{v.tab, v.slot});
                    }
                    else if constexpr(std::is_same_v<T, Title>)
                    {
                        open[v.tab].title = v.title;
                    }
                    else if constexpr(std::is_same_v<T, Address>)
                    {
                        open[v.tab].address = v.url.substr(0, 40);
                    }
                    else if constexpr(std::is_same_v<T, AudioStart>)
                    {
                        open[v.tab].audioStarted = true;
                    }
                    else if constexpr(std::is_same_v<T, Closed>)
                    {
                        open[v.tab].closed = true;
                    }
                },
                m);
        }
    };
    while(Clock::now() < until)
    {
        pump();
        if(!clicked && Clock::now() > until - std::chrono::seconds(1))
        {
            clicked = true;
            // (A move first, as the viewer sends: Windows' Chromium wants one.)
            host->send(Mouse{1, MouseKind::Move, 123, 45, MouseButton::Left, 1});
            host->send(Mouse{1, MouseKind::Down, 123, 45, MouseButton::Left, 1});
            host->send(Mouse{1, MouseKind::Up, 123, 45, MouseButton::Left, 1});
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    pump();

    int failures = 0;

    // Typing: a page of its own, clicked into, then keys and text.
    {
        auto memory = SharedMemory::create("mediatest", frameMemorySize(kWidth, kHeight));
        const TabId id = 100;
        open[id].memory = std::move(memory);
        host->send(Open{id, "test", kTypingPage, kWidth, kHeight, open[id].memory->name(), 30});
        bool editing = false;
        auto wait = [&](std::chrono::milliseconds ms)
        {
            const auto end = Clock::now() + ms;
            while(Clock::now() < end)
            {
                pump();
                for(const Message& m : said)
                {
                    if(const auto* e = std::get_if<Editing>(&m); e && e->tab == id)
                    {
                        editing = e->editing;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        };
        // (Long enough for the page to load under Wine, too.)
        wait(std::chrono::milliseconds(waitMs));
        host->send(Focus{id, true});
        host->send(Mouse{id, MouseKind::Move, 50, 25, MouseButton::Left, 1});
        host->send(Mouse{id, MouseKind::Down, 50, 25, MouseButton::Left, 1});
        host->send(Mouse{id, MouseKind::Up, 50, 25, MouseButton::Left, 1});
        wait(std::chrono::milliseconds(300));
        const bool editingSeen = editing;
        using namespace key_flags;
        auto press = [&](uint32_t code, uint32_t flags, const char* text, uint32_t ch = 0)
        {
            host->send(Key{id, KeyKind::Down, code, flags, 0});
            if(ch)
            {
                host->send(Key{id, KeyKind::Char, code, flags, ch});
            }
            if(text)
            {
                host->send(Text{id, text});
            }
            host->send(Key{id, KeyKind::Up, code, flags, 0});
        };
        press(0x41, 0, "a");        // a
        press(0x31, kShift, "!");   // shift+1
        press(0x45, 0, "\xC3\xA9"); // é (as an input method gives it)
        press(0x42, 0, "b");
        press(0x08, 0, nullptr);     // backspace
        press(0x0D, 0, nullptr, 13); // enter
        wait(std::chrono::milliseconds(500));
        std::println("typing: editing {}; title '{}'", editingSeen ? "yes" : "no", open[id].title);
        const bool typed = open[id].title.starts_with("[a!\xC3\xA9]") &&
                           open[id].title.find("65:a") != std::string::npos &&
                           open[id].title.find("8:Backspace") != std::string::npos &&
                           open[id].title.ends_with("submitted");
        std::println("typing reached the page: {}", typed ? "yes" : "no");
        failures += !typed || !editingSeen;

        // The wheel: a page taller and wider than the tab, scrolled down a
        // notch and right a notch.
        open[id].title.clear();
        host->send(Navigate{id,
                            "data:text/html,<body style='width:3000px;height:3000px'>"
                            "<script>onscroll=()=>document.title=scrollX+','+scrollY</script>"});
        wait(std::chrono::milliseconds(800));
        host->send(Wheel{id, 100, 100, 0, -120});
        wait(std::chrono::milliseconds(400));
        const std::string down = open[id].title;
        host->send(Wheel{id, 100, 100, 120, 0});
        wait(std::chrono::milliseconds(400));
        std::println("wheel: a notch down scrolled to '{}', then right to '{}'", down,
                     open[id].title);
        // (Linux scrolls 120 pixels a notch, Windows 100.)
        auto scrolled = [](const std::string& title, bool right)
        {
            const size_t comma = title.find(',');
            return comma != std::string::npos &&
                   std::atoi(right ? title.c_str() : title.c_str() + comma + 1) > 0;
        };
        failures += !scrolled(down, false) || !scrolled(open[id].title, true);

        // Dragging the scrollbar's thumb (at the right edge) down.
        host->send(Wheel{id, 100, 100, 0, 120}); // back to the top
        host->send(Wheel{id, 100, 100, -120, 0});
        wait(std::chrono::milliseconds(400));
        const int32_t x = static_cast<int32_t>(kWidth) - 6;
        host->send(Mouse{id, MouseKind::Move, x, 20, MouseButton::Left, 1});
        host->send(Mouse{id, MouseKind::Down, x, 20, MouseButton::Left, 1});
        for(int32_t y = 30; y <= 120; y += 10)
        {
            wait(std::chrono::milliseconds(20));
            host->send(Mouse{id, MouseKind::Move, x, y, MouseButton::Left, 1});
        }
        host->send(Mouse{id, MouseKind::Up, x, 120, MouseButton::Left, 1});
        wait(std::chrono::milliseconds(400));
        const std::string dragged = open[id].title;
        std::println("scrollbar dragged: scrolled to '{}'", dragged);
        const size_t comma = dragged.find(',');
        failures += comma == std::string::npos || std::atoi(dragged.c_str() + comma + 1) < 200;
    }
    for(auto& [id, t] : open)

    {
        auto a = host->audio(id);
        const float buffered = a ? a->bufferedSeconds() : 0.0f;
        std::println("tab {}: {:.1f} frames/s, sound {} ({:.2f} s buffered), title '{}', at {}", id,
                     t.frames / static_cast<double>(seconds), t.audioStarted ? "yes" : "no",
                     buffered, t.title, t.address);
        failures +=
            id < 100 && (t.frames < 10 * seconds || !t.audioStarted); // (not the typing page)
    }
    const bool clickSeen = open[1].title == "click 123,45";
    std::println("click reached tab 1: {}", clickSeen ? "yes" : "no");
    failures += !clickSeen;

    if(!png.empty() && !last.empty())
    {
        image::MipLevel level;
        level.width = kWidth;
        level.height = kHeight;
        level.pixels.resize(last.size());
        for(size_t i = 0; i < last.size(); i += 4) // BGRA to RGBA
        {
            level.pixels[i] = last[i + 2];
            level.pixels[i + 1] = last[i + 1];
            level.pixels[i + 2] = last[i];
            level.pixels[i + 3] = last[i + 3];
        }
        image::writePng(png, level);
        std::println("wrote {}", png.string());
    }

    for(auto& [id, t] : open)
    {
        host->send(Close{id});
    }
    const auto closing = Clock::now();
    while(Clock::now() - closing < std::chrono::seconds(5) &&
          !std::ranges::all_of(open, [](const auto& e) { return e.second.closed; }))
    {
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const bool allClosed = std::ranges::all_of(open, [](const auto& e) { return e.second.closed; });
    std::println("all closed: {}", allClosed ? "yes" : "no");
    failures += !allClosed;
    const auto quitting = Clock::now();
    host.reset();
    std::println(
        "host gone after {} ms",
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - quitting).count());
    std::error_code ec;
    fs::remove_all(root, ec);
    std::println("{}", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
