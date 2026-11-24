// logintest: logs in headless with a credentials file, stays connected for a
// while, prints what happened, then logs out. For testing the session and
// circuit code against a real grid with a test account.
//
// Usage: logintest --login-file PATH [--seconds N] [--start last|home|REGION[/X/Y/Z]]
//                  [--dump-layer-data DIR]
//
// Environment variables for debugging object decoding:
//   LOGINTEST_NEAR=x,y              list objects within 20 m of (x, y)
//   LOGINTEST_FIND_TEXTURE=prefix   list objects using a texture id starting so
//
// --dump-layer-data writes each land LayerData message to DIR/layer_<region handle>_<n>.bin
// (one byte layer type, then the packed data), for test fixtures.

#include "viewer/core/Branding.hpp"
#include "viewer/protocol/Messages.hpp"
#include "viewer/session/Credentials.hpp"
#include "viewer/session/Session.hpp"
#include "viewer/world/World.hpp"

#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <print>
#include <thread>

using namespace viewer;
using namespace viewer::session;
using namespace std::chrono_literals;

namespace
{

int usage()
{
    std::println(
        stderr,
        "usage: logintest --login-file PATH [--seconds N] [--start last|home|REGION[/X/Y/Z]] "
        "[--dump-layer-data DIR]");
    return 2;
}

void printSession(const Session& session)
{
    std::println("state: {}", stateName(session.state()));
    for(const auto& [id, region] : session.regions())
    {
        std::println("region {} '{}'{} grid ({}, {}) at {}: handshake {}, {} capabilities, "
                     "event queue {}",
                     id, region->name, region->isMain ? " [main]" : "", region->gridX(),
                     region->gridY(), region->endpoint.toString(),
                     region->handshakeReceived ? "yes" : "no", region->capabilities.size(),
                     region->eventQueueRunning ? "running" : "stopped");
        if(const auto s = session.circuitStats(id))
        {
            std::println("    ping {} ms, in {}, out {}, resent {}, unacked {}, duplicates {}, "
                         "dropped {}, malformed {}",
                         s->roundTrip.count(), s->packetsIn, s->packetsOut, s->resent, s->unacked,
                         s->duplicates, s->reliableDropped, s->malformed);
        }
    }
    std::println("UDP messages received:");
    for(const auto& [name, count] : session.messageCounts())
    {
        std::println("  {:6}  {}", count, name);
    }
    std::println("event queue events received:");
    for(const auto& [name, count] : session.eventCounts())
    {
        std::println("  {:6}  {}", count, name);
    }
}

} // namespace

int main(int argc, char** argv)
{
    std::string loginFile;
    int seconds = 20;
    std::string start = "last";
    std::filesystem::path dumpDirectory;
    for(int i = 1; i < argc; i++)
    {
        const std::string_view arg = argv[i];
        if(i + 1 >= argc)
        {
            return usage();
        }
        if(arg == "--login-file")
        {
            loginFile = argv[++i];
        }
        else if(arg == "--seconds")
        {
            const std::string_view v = argv[++i];
            if(std::from_chars(v.data(), v.data() + v.size(), seconds).ec != std::errc())
            {
                return usage();
            }
        }
        else if(arg == "--start")
        {
            start = argv[++i];
        }
        else if(arg == "--dump-layer-data")
        {
            dumpDirectory = argv[++i];
        }
        else
        {
            return usage();
        }
    }
    if(loginFile.empty())
    {
        return usage();
    }

    auto credentials = readCredentialsFile(loginFile);
    if(!credentials)
    {
        std::println(stderr, "{}", credentials.error());
        return 1;
    }

    LoginParameters parameters;
    parameters.loginUri = credentials->loginUri;
    parameters.name = credentials->name;
    parameters.passwordHash = std::move(credentials->passwordHash);
    parameters.start = startLocation(start);
    parameters.channel = branding::kChannel;
    parameters.version = VIEWER_VERSION;
    parameters.machine = queryMachineInfo();

    auto http = net::createHttpClient(
        {.userAgent = std::format("{}/{}", branding::kUserAgent, VIEWER_VERSION)});
    Session session(*http, std::move(parameters));

    // Log in and stay for the requested time (or until something ends it).
    std::vector<RegionMessage> messages;
    world::World world;
    int dumped = 0;
    if(!dumpDirectory.empty())
    {
        std::filesystem::create_directories(dumpDirectory);
    }
    SessionState last = session.state();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while(std::chrono::steady_clock::now() < deadline)
    {
        session.update();
        session.drainRegionMessages(messages);
        for(const auto& m : messages)
        {
            if(m.kind == RegionMessage::Kind::RegionRemoved)
            {
                world.removeRegion(m.regionHandle);
            }
            else
            {
                world.handleMessage(m.regionHandle, m.event);
            }
            if(dumpDirectory.empty() || !m.event.is<protocol::msg::LayerData>())
            {
                continue;
            }
            const auto layer = m.event.decode<protocol::msg::LayerData>();
            if(layer && layer->LayerID.Type == 'L')
            {
                std::ofstream out(dumpDirectory / std::format("layer_{:016x}_{:03}.bin",
                                                              m.regionHandle, dumped++),
                                  std::ios::binary);
                out.put(static_cast<char>(layer->LayerID.Type));
                out.write(reinterpret_cast<const char*>(layer->LayerData_.Data.data()),
                          static_cast<std::streamsize>(layer->LayerData_.Data.size()));
            }
        }
        if(session.state() != last)
        {
            last = session.state();
            std::println("-> {}", stateName(last));
        }
        if(last == SessionState::Failed || last == SessionState::LoggedOut)
        {
            break;
        }
        std::this_thread::sleep_for(10ms);
    }

    printSession(session);
    {
        std::map<int, size_t> byType;
        size_t prims = 0;
        size_t sculpts = 0;
        size_t meshes = 0;
        size_t children = 0;
        std::map<int, size_t> shapes;
        for(const auto& [key, o] : world.objects())
        {
            byType[o.type]++;
            if(o.type == world::ObjectType::Prim)
            {
                prims++;
                const uint8_t sculpt = o.sculptType & world::SculptType::Mask;
                sculpts += sculpt != 0 && sculpt != world::SculptType::Mesh;
                meshes += sculpt == world::SculptType::Mesh;
                shapes[o.volume.profile.curve * 256 + o.volume.path.curve]++;
            }
            children += o.parentId != 0;
        }
        const auto& ws = world.stats();
        std::println("updates: {} full, {} compressed, {} terse, {} undecodable", ws.fullUpdates,
                     ws.compressedUpdates, ws.terseUpdates, ws.undecodable);
        std::map<int, size_t> sculptTypes;
        for(const auto& [key, o] : world.objects())
        {
            if(o.sculptType != 0)
            {
                sculptTypes[o.sculptType]++;
            }
        }
        for(const auto& [type, count] : sculptTypes)
        {
            std::println("  sculpt type 0x{:02x}: {}", type, count);
        }
        if(const char* find = std::getenv("LOGINTEST_FIND_TEXTURE"))
        {
            for(const auto& [key, o] : world.objects())
            {
                for(size_t f = 0; f < world::kMaxFaces; f++)
                {
                    if(o.faces[f].texture.toString().starts_with(find))
                    {
                        std::println(
                            "  texture {} on face {} of local {} type {} sculpt 0x{:02x} parent {}",
                            find, f, o.localId, o.type, o.sculptType, o.parentId);
                        break;
                    }
                }
            }
        }
        if(const char* near = std::getenv("LOGINTEST_NEAR"))
        {
            float nx = 0;
            float ny = 0;
            std::sscanf(near, "%f,%f", &nx, &ny);
            for(const auto& [key, o] : world.objects())
            {
                glm::vec3 p = o.position;
                if(o.parentId != 0)
                {
                    if(const auto* parent = world.object(o.region, o.parentId))
                    {
                        p = parent->position + parent->rotation * o.position;
                    }
                }
                if(glm::length(glm::vec2(p.x - nx, p.y - ny)) < 20.0f)
                {
                    std::println(
                        "  near: local {} type {} parent {} pos ({:.1f},{:.1f},{:.1f}) scale "
                        "({:.1f},{:.1f},{:.1f}) curves 0x{:02x}/0x{:02x} sculpt 0x{:02x} {}",
                        o.localId, o.type, o.parentId, p.x, p.y, p.z, o.scale.x, o.scale.y,
                        o.scale.z, o.volume.profile.curve, o.volume.path.curve, o.sculptType,
                        o.sculptTexture.toString());
                }
            }
        }
        std::println("objects: {} ({} prims: {} sculpted, {} mesh; {} linked children)",
                     world.objects().size(), prims, sculpts, meshes, children);
        for(const auto& [type, count] : byType)
        {
            std::println("  type {:3}: {}", type, count);
        }
        std::println("prim shapes (profile/path curve):");
        for(const auto& [shape, count] : shapes)
        {
            std::println("  0x{:02x}/0x{:02x}: {}", shape >> 8, shape & 0xFF, count);
        }
    }
    if(const auto* failure = session.failure())
    {
        std::println(stderr, "failed ({}): {}", failure->reason, failure->message);
        return 1;
    }

    session.requestLogout();
    while(session.state() == SessionState::LoggingOut)
    {
        session.update();
        std::this_thread::sleep_for(10ms);
    }
    std::println("-> {}", stateName(session.state()));
    return 0;
}
