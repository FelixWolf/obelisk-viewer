#pragma once

// AISv3, the inventory service (the region's InventoryAPIv3 capability):
// REST over LLSD, as the official viewer's AISAPI uses it. A folder (by id,
// or by a system folder's alias: "current" for the Current Outfit Folder,
// "root", "clothing", ...) and what's in it to a depth, a folder's links
// with their targets, an item. Replies are merged into the Model; each
// request's callback says what came (Model::Merged) once update() sees the
// reply, on the thread calling update(). Writes: links made in a folder,
// items removed (more as they're needed).

#include "viewer/core/MessageQueue.hpp"
#include "viewer/inventory/Model.hpp"
#include "viewer/net/HttpClient.hpp"

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace viewer::inventory
{

class Ais
{
public:
    // ok: the service answered and its reply was merged.
    using Done = std::function<void(bool ok, const Model::Merged& merged)>;

    Ais(net::HttpClient& http, Model& model);

    // The capability (empty: none; then nothing is sent and callbacks
    // report failure).
    void setCapability(std::string url) { cap_ = std::move(url); }
    bool available() const { return !cap_.empty(); }

    // A folder and its contents, `depth` levels down (1: its children).
    void fetchChildren(std::string_view folder, int depth, Done done);
    // A folder's links, each with what it points at.
    void fetchLinks(std::string_view folder, Done done);
    void fetchItem(const Uuid& item, Done done);

    // Writes. Links in a folder (the reply, merged, has them).
    struct Link
    {
        Uuid target;            // the item (or folder) linked to
        int type = kLink;       // kLink, or kFolderLink
        int inventoryType = -1; // the target's
        std::string name;
        std::string description;
    };
    void createLinks(const Uuid& folder, std::span<const Link> links, Done done);
    // An item (or link) removed (and from the model once it is).
    void removeItem(const Uuid& item, Done done);

    // Merges arrived replies and calls their callbacks.
    void update();
    // Forgets requests under way (their callbacks aren't called).
    void cancel();

    // The URL for a path under the capability (tests).
    std::string url(std::string_view path) const { return cap_ + std::string(path); }

private:
    void get(std::string path, Done done);
    void send(net::HttpMethod method, std::string path, const llsd::Value* body, Done done);

    struct Reply
    {
        uint64_t request = 0;
        net::HttpResponse response;
    };
    net::HttpClient& http_;
    Model& model_;
    std::string cap_;
    std::unique_ptr<net::HttpRequestGroup> requests_;
    std::shared_ptr<MessageQueue<Reply>> replies_ = std::make_shared<MessageQueue<Reply>>();
    std::vector<Reply> drained_;
    std::unordered_map<uint64_t, Done> pending_;
    uint64_t nextRequest_ = 1;
};

} // namespace viewer::inventory
