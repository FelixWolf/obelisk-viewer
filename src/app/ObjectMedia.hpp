#pragma once

// Media on a prim (MOAP, "shared media"): each face can show a web page or
// a video, set with llSetPrimMediaParams or the build tools. An object's
// updates carry only a version of its media ("x-mv:0000000003/<agent>",
// world::Object::mediaVersion); the faces' media themselves come from its
// region's ObjectMedia capability ({verb: GET, object_id} answered with
// {object_id, object_media_version, object_media_data: [a face's, or
// undefined, per face]}), asked again whenever the version changes, as the
// official viewer's LLObjectMediaDataClient (llmediadataclient.cpp) does.
// Field names are llmediaentry.cpp's.
//
// Only kept here, not played: the Media tab lists them, and later a
// browser (or video) shows them on their faces. Per session (clear()
// between).

#include "viewer/core/MessageQueue.hpp"
#include "viewer/core/Uuid.hpp"
#include "viewer/llsd/Value.hpp"
#include "viewer/net/HttpClient.hpp"
#include "viewer/world/World.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace viewer
{

// One face's media (LLMediaEntry).
struct MediaEntry
{
    std::string homeUrl;
    std::string currentUrl; // what it shows now (navigated to)
    bool autoPlay = false;
    bool autoLoop = false;
    bool autoScale = false;
    bool autoZoom = false;
    bool firstClickInteract = false;
    bool altImageEnable = false;
    int width = 0; // pixels (0: the viewer's choice)
    int height = 0;
    // 0: the standard controls, 1: mini.
    int controls = 0;
    bool whitelistEnable = false;
    std::vector<std::string> whitelist;
    // Who may interact with it, and use its controls (bits: 1 owner,
    // 2 group, 4 anyone).
    int permsInteract = 7;
    int permsControl = 7;

    // What it shows: the current URL, else home.
    const std::string& url() const { return currentUrl.empty() ? homeUrl : currentUrl; }
    // Whether its creator's whitelist lets it show `url` (always without
    // one), as LLMediaEntry::checkCandidateUrl: each entry a URL whose
    // scheme, host and path are globs ("*" for anything, any case), https
    // if it names no scheme, and what it leaves out matching anything.
    bool allows(std::string_view url) const;
    bool operator==(const MediaEntry&) const = default;
};

MediaEntry parseMediaEntry(const llsd::Value& value);

// An object's media, per face (nothing: none on that face).
struct ObjectMediaData
{
    Uuid object;
    world::ObjectKey key{}; // where it was when asked
    uint32_t version = 0;
    std::vector<std::optional<MediaEntry>> faces;

    size_t count() const;
};

// The capability's answer; nothing if it isn't one (or reports an error).
std::optional<ObjectMediaData> parseObjectMediaResponse(const llsd::Value& body);
// The number in a media version string ("x-mv:0000000003/..." is 3);
// nothing if it isn't one.
std::optional<uint32_t> mediaVersionNumber(std::string_view version);

class ObjectMedia
{
public:
    explicit ObjectMedia(net::HttpClient& http);
    ObjectMedia(const ObjectMedia&) = delete;
    ObjectMedia& operator=(const ObjectMedia&) = delete;

    // A region's ObjectMedia capability URL (empty: not granted yet).
    using Capability = std::function<std::string(uint64_t region)>;
    // Once a frame: the prims with media (ObjectIndex::mediaSources), the
    // world they're in, and where to ask. Asks for those whose version is
    // new (a few at a time), takes the answers, forgets the gone.
    void update(const std::unordered_set<world::ObjectKey, world::ObjectKeyHash>& withMedia,
                const world::World& world, const Capability& capability);

    // An object's media once known (nullptr until then, or if it has none).
    const ObjectMediaData* find(const Uuid& object) const;
    // Everything known, by object.
    const std::unordered_map<Uuid, ObjectMediaData>& all() const { return known_; }
    // Bumped when anything known changes.
    uint64_t revision() const { return revision_; }
    size_t pending() const { return queue_.size() + inFlight_; }

    void clear();

private:
    struct Want
    {
        world::ObjectKey key;
        std::string version; // the version string asked for
        Uuid object;
        uint8_t attempts = 0;
        std::chrono::steady_clock::time_point notBefore{};
    };
    struct Answer
    {
        Want asked;
        net::HttpResponse response;
    };

    net::HttpClient& http_;
    std::unordered_map<Uuid, ObjectMediaData> known_;
    // By object: the version string last asked for (or queued).
    std::unordered_map<Uuid, std::string> asked_;
    std::deque<Want> queue_;
    size_t inFlight_ = 0;
    uint64_t revision_ = 0;
    std::chrono::steady_clock::time_point nextSweep_{};
    MessageQueue<Answer> answers_;
    std::vector<Answer> drained_;
    // Last: destroyed first, so no callback outlives the queue.
    std::unique_ptr<net::HttpRequestGroup> group_;
};

} // namespace viewer
