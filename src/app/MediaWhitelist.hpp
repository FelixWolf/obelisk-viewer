#pragma once

// Which media plays by itself. The setting (kMediaPlay) says how, and the
// service's list (docs/viewerconfig-api.md, media-whitelist) which sites may
// when it says "whitelist". Not listed isn't blocked: it waits for a click.
// Matching is done here, so the service never hears what media there is
// around. (A prim's own whitelist is another thing: its creator's, and
// always kept to.)

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace viewer
{

struct MediaSite
{
    std::string host;        // lowercase
    bool subdomains = false; // those below it too
    std::string path = "/";  // it and what's below it
};

struct MediaWhitelist
{
    std::vector<MediaSite> sites;
    int refresh = 3600; // seconds

    // Whether `url` is on a listed site. Strict: https only, on its own
    // port, and anything a browser might read another way than we do (a user
    // name, escapes or "..", odd characters) isn't.
    bool allows(std::string_view url) const;
};

std::expected<MediaWhitelist, std::string> parseMediaWhitelist(std::string_view json);

enum class MediaPlay : uint8_t
{
    Always,
    Whitelist,
    Click,
    Disable,
};
// The setting's value; Whitelist if it's none of them.
MediaPlay mediaPlayFrom(std::string_view setting);

enum class MediaStart : uint8_t
{
    Play,  // by itself
    Click, // once it's clicked
    Never,
};
// How a face's media starts. Also for each page it goes to by itself
// (`autoPlay`: the face's), so a listed site can't lead to another unasked.
MediaStart mediaStart(MediaPlay play, bool autoPlay, std::string_view url,
                      const MediaWhitelist& whitelist);

} // namespace viewer
