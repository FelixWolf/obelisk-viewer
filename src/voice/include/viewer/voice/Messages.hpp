#pragma once

// What's said to voice servers and the regions in front of them: the data
// channel's JSON and the capabilities' bodies (llvoicewebrtc.cpp).

#include "viewer/core/Uuid.hpp"
#include "viewer/llsd/Value.hpp"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace viewer::voice
{

// Global meters; rotations of an x-forward, z-up frame.
struct Placement
{
    glm::dvec3 speaker{0.0};
    glm::quat speakerRotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::dvec3 listener{0.0};
    glm::quat listenerRotation{1.0f, 0.0f, 0.0f, 0.0f};
};

// The listener, held within 50 m of the speaker (MAX_AUDIO_DIST).
inline constexpr double kMaxListenerDistance = 50.0;
glm::dvec3 tethered(const glm::dvec3& listener, const glm::dvec3& speaker);

std::string joinMessage(bool primary);
std::string placementMessage(const Placement& placement);

// What a message says of someone.
struct ParticipantUpdate
{
    Uuid id;
    bool joined = false;
    bool primary = false; // joined where they are
    bool left = false;
    std::optional<float> power;
    std::optional<bool> speaking;
    std::optional<bool> moderatorMuted;
};
std::vector<ParticipantUpdate> parseParticipants(std::string_view json);

struct Candidate
{
    std::string mid;
    std::string candidate;
};

// ProvisionVoiceAccountRequest: a connection asked for (`parcel`: its own
// channel; nothing: the estate's), and given up.
llsd::Value provisionRequest(const std::string& sdp, std::optional<int> parcel);
llsd::Value logoutRequest(const std::string& viewerSession);
struct ProvisionReply
{
    std::string viewerSession;
    std::string sdp;
};
std::optional<ProvisionReply> parseProvisionReply(const llsd::Value& body);
// VoiceSignalingRequest: these candidates, or (none) that there are no more.
llsd::Value candidatesRequest(const std::string& viewerSession,
                              const std::vector<Candidate>& candidates);

// The grid's STUN servers, by its login address.
std::vector<std::string> stunServers(std::string_view loginUri);

} // namespace viewer::voice
