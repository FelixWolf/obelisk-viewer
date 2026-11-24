#pragma once

// Updates of packaged builds (./dist.sh), by Velopack from viewerconfig's
// feed (docs/viewerconfig-api.md, Updates): looked for at the start and every
// so often after, downloaded quietly, and put in place when asked to restart
// (restart()) or else the next time the viewer starts (main()).
//
// Velopack's own HTTP source sends an id of the installation, so it's given
// one that uses our HttpClient, which sends nothing but the User-Agent.
// Velopack checks what's downloaded against the feed's size and SHA-256.

#include "viewer/net/HttpClient.hpp"

#include <memory>
#include <string>
#include <vector>

namespace viewer
{

// The feed's folder under viewerconfig's address, for a package id; empty if
// updates mustn't come from that address: what comes is run, so only from
// https, or this computer (to test).
std::string updateFolder(std::string_view baseUrl, std::string_view packageId);

// Another of this same program is running (Windows; elsewhere always false).
// An update can't be put in place then: on Windows, Velopack ends every one
// of them, logged in or not. (On Linux it replaces the AppImage's file, which
// those running keep.)
bool anotherInstanceRunning();

class Updater
{
public:
    struct Options
    {
        std::string baseUrl; // viewerconfig's ".../api/v1/"
        std::string userAgent;
        std::vector<std::string> restartArguments; // the command line's
    };
    struct Status
    {
        enum class Stage
        {
            Idle, // nothing newer, or not known yet
            Downloading,
            Ready,
        };
        Stage stage = Stage::Idle;
        std::string version; // the newer one
        int progress = 0;    // percent
    };

    // Null if this build doesn't update itself: not packaged, not installed
    // (run from a build folder), or no address it may come from.
    static std::unique_ptr<Updater> create(net::HttpClient& http, Options options);
    virtual ~Updater() = default;

    virtual Status status() const = 0;
    // Puts the update in place once we've exited, and starts the viewer
    // again: the caller then exits. False (said in the log) if it can't.
    virtual bool restart() = 0;
};

} // namespace viewer
