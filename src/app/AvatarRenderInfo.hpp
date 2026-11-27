#pragma once

// Avatar complexity told to and heard from regions, through their
// AvatarRenderInfo capability, as the official viewer's
// llavatarrenderinfoaccountant.cpp does:
// - every 60 seconds (2 more for each avatar told about), a POST of each
//   avatar in the region we've added up: {agents: {<id>: {weight,
//   tooComplex}}} (ours too);
// - every 15 seconds, a GET of what the region heard: each avatar's
//   complexity as its own viewer reported it ({agents: {<id>: {weight}}}),
//   and how many of those around who report see us over their limit
//   ({reportinglimit, overlimit}).

#include "viewer/core/MessageQueue.hpp"
#include "viewer/core/Uuid.hpp"
#include "viewer/llsd/Value.hpp"
#include "viewer/net/HttpClient.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace viewer
{

// What we tell a region about one avatar.
struct AvatarRenderReport
{
    Uuid avatar;
    uint32_t complexity = 0;
    bool tooComplex = false;
};

// The POST's body.
llsd::Value avatarRenderReportBody(const std::vector<AvatarRenderReport>& reports);

// What a GET answered.
struct AvatarRenderAnswer
{
    std::unordered_map<Uuid, uint32_t> weights; // as each avatar's viewer reported
    // How many around report, and how many of them see us over their limit.
    std::optional<int> reporting;
    std::optional<int> overLimit;
};
std::optional<AvatarRenderAnswer> parseAvatarRenderAnswer(const llsd::Value& body);

class AvatarRenderInfo
{
public:
    explicit AvatarRenderInfo(net::HttpClient& http);
    AvatarRenderInfo(const AvatarRenderInfo&) = delete;
    AvatarRenderInfo& operator=(const AvatarRenderInfo&) = delete;

    // A region's AvatarRenderInfo URL (empty: not granted yet).
    using Capability = std::function<std::string(uint64_t region)>;
    // What to tell a region: the avatars in it we've added up.
    using Reports = std::function<std::vector<AvatarRenderReport>(uint64_t region)>;
    // Once a frame: the regions we're connected to, and `main` (ours: whose
    // answer says how many see us over their limit).
    void update(const std::vector<uint64_t>& regions, uint64_t main, const Capability& capability,
                const Reports& reports, std::chrono::steady_clock::time_point now);

    // An avatar's complexity as its own viewer reported it; nothing if no
    // region said.
    std::optional<uint32_t> reported(const Uuid& avatar) const;
    // How many around us see us over their limit, of how many report (the
    // main region's last answer).
    struct Seen
    {
        int reporting = 0;
        int overLimit = 0;
    };
    std::optional<Seen> seen() const { return seen_; }

    // A session ended.
    void clear();

private:
    struct Region
    {
        std::chrono::steady_clock::time_point nextGet{};
        std::chrono::steady_clock::time_point nextPost{};
        bool getting = false;
        std::unordered_map<Uuid, uint32_t> weights;
    };
    struct Answer
    {
        uint64_t region = 0;
        bool post = false;
        net::HttpResponse response;
    };

    net::HttpClient& http_;
    std::map<uint64_t, Region> regions_;
    std::optional<Seen> seen_;
    uint64_t main_ = 0;
    MessageQueue<Answer> answers_;
    std::vector<Answer> drained_;
    // Last: destroyed first, so no callback outlives the queue.
    std::unique_ptr<net::HttpRequestGroup> group_;

    static constexpr auto kGetInterval = std::chrono::seconds(15);
    static constexpr auto kPostInterval = std::chrono::seconds(60);
    static constexpr auto kFirstPost = std::chrono::seconds(5);
};

} // namespace viewer
