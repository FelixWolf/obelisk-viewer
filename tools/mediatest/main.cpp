// Drives the media host as the viewer does, without the viewer: starts it,
// opens tabs on a page of its own (a data: URL: no network), and checks
// that frames and sound come back, that clicks reach the page, and that
// it closes cleanly. Writes the last frame of the first tab as a PNG.
//
//   mediatest [--tabs N] [--seconds S] [--png OUT.png] [--url URL]

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
    }
    const char* base = SDL_GetBasePath();
    const fs::path program = fs::path(base ? base : ".") / "media" / "obelisk-media";
    const fs::path root = fs::temp_directory_path() / std::format("mediatest-{}", getpid());

    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    auto host = startMediaHost(program, Init{kProtocolVersion, root.string(), true, true});
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
            host->send(Mouse{1, MouseKind::Down, 123, 45, MouseButton::Left, 1});
            host->send(Mouse{1, MouseKind::Up, 123, 45, MouseButton::Left, 1});
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    pump();

    int failures = 0;
    for(auto& [id, t] : open)
    {
        auto a = host->audio(id);
        const float buffered = a ? a->bufferedSeconds() : 0.0f;
        std::println("tab {}: {:.1f} frames/s, sound {} ({:.2f} s buffered), title '{}', at {}", id,
                     t.frames / static_cast<double>(seconds), t.audioStarted ? "yes" : "no",
                     buffered, t.title, t.address);
        failures += t.frames < 10 * seconds || !t.audioStarted;
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
