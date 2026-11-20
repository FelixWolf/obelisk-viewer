#pragma once

// Our profile picture, for the login screen's accounts: asked for once a
// login (the region's AgentProfile capability, GET <cap>/<agent id>, as the
// official viewer's LLAvatarPropertiesProcessor; the reply's sl_image_id),
// and given to whoever keeps it (SavedLogins). The texture itself stays in
// the disk cache, where the login screen finds it before the next login.

#include "viewer/core/MessageQueue.hpp"
#include "viewer/core/Uuid.hpp"
#include "viewer/net/HttpClient.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace viewer
{

class AccountPicture
{
public:
    explicit AccountPicture(net::HttpClient& http);

    // Each frame while logged in: asks once `capability` is known (empty:
    // not yet); `found` gets the picture's id (null: none set) when it
    // comes.
    void update(const Uuid& agent, std::string_view capability,
                const std::function<void(const Uuid&)>& found);
    // Logged out: asked again at the next login.
    void clear();

private:
    net::HttpClient& http_;
    std::unique_ptr<net::HttpRequestGroup> requests_;
    std::shared_ptr<MessageQueue<std::optional<Uuid>>> answers_ =
        std::make_shared<MessageQueue<std::optional<Uuid>>>();
    Uuid asked_; // the agent asked about this login (null: nobody yet)
};

} // namespace viewer
