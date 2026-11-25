#include "viewer/voice/Messages.hpp"

#include "viewer/llsd/Serialization.hpp"

#include <glm/geometric.hpp>

#include <format>

namespace viewer::voice
{

namespace
{

constexpr const char* kServerType = "webrtc";

int64_t hundredths(double v)
{
    return static_cast<int64_t>(v * 100.0);
}

std::string point(const glm::dvec3& p)
{
    return std::format(R"({{"x":{},"y":{},"z":{}}})", hundredths(p.x), hundredths(p.y),
                       hundredths(p.z));
}

std::string rotation(const glm::quat& q)
{
    return std::format(R"({{"x":{},"y":{},"z":{},"w":{}}})", hundredths(q.x), hundredths(q.y),
                       hundredths(q.z), hundredths(q.w));
}

} // namespace

glm::dvec3 tethered(const glm::dvec3& listener, const glm::dvec3& speaker)
{
    const glm::dvec3 offset = listener - speaker;
    const double distance = glm::length(offset);
    return distance > kMaxListenerDistance ? speaker + offset * (kMaxListenerDistance / distance)
                                           : listener;
}

std::string joinMessage(bool primary)
{
    return primary ? R"({"j":{"p":true}})" : R"({"j":{}})";
}

std::string placementMessage(const Placement& p)
{
    return std::format(R"({{"sp":{},"sh":{},"lp":{},"lh":{}}})", point(p.speaker),
                       rotation(p.speakerRotation), point(tethered(p.listener, p.speaker)),
                       rotation(p.listenerRotation));
}

std::vector<ParticipantUpdate> parseParticipants(std::string_view json)
{
    std::vector<ParticipantUpdate> out;
    const auto body = llsd::fromJson(json);
    const auto* people = body ? body->get<llsd::Map>() : nullptr;
    if(!people)
    {
        return out;
    }
    for(const auto& [key, v] : *people)
    {
        const auto id = Uuid::parse(key);
        if(!id || id->isNull() || !v.isMap())
        {
            continue; // (test clients)
        }
        ParticipantUpdate u;
        u.id = *id;
        u.joined = v["j"].isMap();
        u.primary = u.joined && v["j"]["p"].asBoolean();
        u.left = v["l"].asBoolean();
        if(v.has("p"))
        {
            u.power = static_cast<float>(v["p"].asReal() / 128.0);
        }
        if(v.has("v"))
        {
            u.speaking = v["v"].asBoolean();
        }
        if(v.has("m"))
        {
            u.moderatorMuted = v["m"].asBoolean();
        }
        out.push_back(u);
    }
    return out;
}

llsd::Value provisionRequest(const std::string& sdp, std::optional<int> parcel)
{
    llsd::Value body;
    body["jsep"]["type"] = "offer";
    body["jsep"]["sdp"] = sdp;
    if(parcel)
    {
        body["parcel_local_id"] = *parcel;
    }
    body["channel_type"] = "local";
    body["voice_server_type"] = kServerType;
    return body;
}

llsd::Value logoutRequest(const std::string& viewerSession)
{
    llsd::Value body;
    body["logout"] = true;
    body["viewer_session"] = viewerSession;
    body["voice_server_type"] = kServerType;
    return body;
}

std::optional<ProvisionReply> parseProvisionReply(const llsd::Value& body)
{
    if(!body.isMap() || !body.has("viewer_session") || !body["jsep"].isMap() ||
       body["jsep"]["type"].asString() != "answer" || !body["jsep"].has("sdp"))
    {
        return std::nullopt;
    }
    return ProvisionReply{body["viewer_session"].asString(), body["jsep"]["sdp"].asString()};
}

llsd::Value candidatesRequest(const std::string& viewerSession,
                              const std::vector<Candidate>& candidates)
{
    llsd::Value body;
    if(candidates.empty())
    {
        body["candidate"]["completed"] = true;
    }
    else
    {
        llsd::Array list;
        for(const Candidate& c : candidates)
        {
            llsd::Value v;
            v["sdpMid"] = c.mid;
            v["sdpMLineIndex"] = 0; // the audio, which all else is bundled with
            v["candidate"] = c.candidate;
            list.push_back(std::move(v));
        }
        body["candidates"] = std::move(list);
    }
    body["viewer_session"] = viewerSession;
    body["voice_server_type"] = kServerType;
    return body;
}

std::vector<std::string> stunServers(std::string_view loginUri)
{
    // getConnectionOptions(): three on the main grid, two on others.
    std::string grid;
    int count = 2;
    if(loginUri.find(".agni.") != std::string_view::npos)
    {
        grid = "agni";
        count = 3;
    }
    else if(loginUri.find(".aditi.") != std::string_view::npos)
    {
        grid = "aditi";
    }
    else
    {
        return {}; // not Linden Lab's
    }
    std::vector<std::string> out;
    for(int i = 1; i <= count; i++)
    {
        out.push_back(std::format("stun:stun{}.{}.secondlife.io:3478", i, grid));
    }
    return out;
}

} // namespace viewer::voice
