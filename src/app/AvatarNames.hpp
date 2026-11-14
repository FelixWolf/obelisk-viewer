#pragma once

// Avatars' display names and usernames, from the GetDisplayNames
// capability (as the official viewer's LLAvatarNameCache): asking for one
// queues it; update() sends what's queued, as many ids as fit a request's
// URL, one request at a time. Kept 20 minutes, then asked again (people
// change their display names).

#include "viewer/core/MessageQueue.hpp"
#include "viewer/core/Uuid.hpp"
#include "viewer/llsd/Value.hpp"
#include "viewer/net/HttpClient.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace viewer
{

struct AvatarName
{
    std::string displayName;
    std::string username; // "first.last", or "first"
    std::string legacyFirstName;
    std::string legacyLastName;
    bool displayNameDefault = false; // the display name is made from the username

    // "First Last", or just "First" for the many whose last name is
    // "Resident" (LLAvatarName::getUserName).
    std::string userName() const;
    // "Display Name (username)", or the display name alone when it's made
    // from the username (LLAvatarName::getCompleteName).
    std::string completeName() const;
};

// One agent from a GetDisplayNames answer (agents[i]); nothing without an id.
std::optional<std::pair<Uuid, AvatarName>> parseAvatarName(const llsd::Value& agent);

class AvatarNames
{
public:
    explicit AvatarNames(net::HttpClient& http);
    AvatarNames(const AvatarNames&) = delete;
    AvatarNames& operator=(const AvatarNames&) = delete;

    // Empty while logged out (nothing is asked for then).
    void setCapabilityUrl(std::string url);
    // The name once it has arrived (null until then, or if it's unknown).
    const AvatarName* get(const Uuid& id);
    // Handles answers and sends what's queued.
    void update();

private:
    enum class State : uint8_t
    {
        Queued,
        Requested,
        Ready,
        Missing,
    };
    struct Entry
    {
        State state = State::Queued;
        uint8_t attempts = 0;
        std::optional<AvatarName> name;
        std::chrono::steady_clock::time_point receivedAt{};
    };
    struct Answer
    {
        std::vector<Uuid> asked;
        net::HttpResponse response;
    };
    void queue(const Uuid& id, Entry& entry);

    net::HttpClient& http_;
    std::string url_;
    std::unordered_map<Uuid, Entry> entries_;
    std::vector<Uuid> queue_;
    bool requesting_ = false;
    std::chrono::steady_clock::time_point nextRequest_{};
    MessageQueue<Answer> answers_;
    std::vector<Answer> drained_;
    // Last: destroyed first, so no callback outlives the queue.
    std::unique_ptr<net::HttpRequestGroup> group_;
};

} // namespace viewer
