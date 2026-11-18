#include "viewer/inventory/Ais.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/llsd/Serialization.hpp"

#include <format>

namespace viewer::inventory
{

Ais::Ais(net::HttpClient& http, Model& model)
    : http_(http), model_(model), requests_(std::make_unique<net::HttpRequestGroup>(http))
{
}

void Ais::get(std::string path, Done done)
{
    send(net::HttpMethod::Get, std::move(path), nullptr, std::move(done));
}

void Ais::send(net::HttpMethod method, std::string path, const llsd::Value* body, Done done)
{
    const uint64_t id = nextRequest_++;
    if(cap_.empty())
    {
        // Reported from update(), as replies are.
        replies_->push({id, net::HttpResponse{.error = net::HttpError::InvalidRequest}});
        pending_[id] = std::move(done);
        return;
    }
    net::HttpRequest request{.method = method, .url = url(path)};
    request.headers.add("Accept", std::string(llsd::kXmlContentType));
    if(body)
    {
        request.headers.add("Content-Type", std::string(llsd::kXmlContentType));
        const std::string xml = llsd::toXml(*body);
        request.body.assign(xml.begin(), xml.end());
    }
    pending_[id] = std::move(done);
    requests_->send(std::move(request), [replies = replies_, id](net::HttpResponse&& response)
                    { replies->push({id, std::move(response)}); });
}

void Ais::fetchChildren(std::string_view folder, int depth, Done done)
{
    // What's in the folder now is what came: anything else is gone.
    get(std::format("/category/{}/children?depth={}", folder, depth),
        [this, done = std::move(done)](bool ok, const Model::Merged& merged)
        {
            if(ok && !merged.folders.empty())
            {
                model_.prune(merged.folders.front(), merged);
            }
            if(done)
            {
                done(ok, merged);
            }
        });
}

void Ais::fetchLinks(std::string_view folder, Done done)
{
    get(std::format("/category/{}/links", folder), std::move(done));
}

void Ais::fetchItem(const Uuid& item, Done done)
{
    get(std::format("/item/{}", item.toString()), std::move(done));
}

void Ais::createLinks(const Uuid& folder, std::span<const Link> links, Done done)
{
    // As the official viewer's link_inventory_array: a "transaction id" to
    // the folder, the links in the body.
    llsd::Value body;
    llsd::Value& list = body["links"];
    list = llsd::Value(llsd::Array{});
    for(const Link& l : links)
    {
        llsd::Value link;
        link["linked_id"] = l.target;
        link["type"] = l.type;
        link["inv_type"] = l.inventoryType;
        link["name"] = l.name;
        link["desc"] = l.description;
        list.append(std::move(link));
    }
    send(net::HttpMethod::Post,
         std::format("/category/{}?tid={}", folder.toString(), Uuid::random().toString()), &body,
         std::move(done));
}

void Ais::slamLinks(const Uuid& folder, std::span<const Link> links, Done done)
{
    // As the official viewer's slam_inventory_folder: the links, a list,
    // replace what's in the folder.
    llsd::Value body(llsd::Array{});
    for(const Link& l : links)
    {
        llsd::Value link;
        link["linked_id"] = l.target;
        link["type"] = l.type;
        link["name"] = l.name;
        link["desc"] = l.description;
        body.append(std::move(link));
    }
    send(net::HttpMethod::Put,
         std::format("/category/{}/links?tid={}", folder.toString(), Uuid::random().toString()),
         &body, std::move(done));
}

void Ais::removeItem(const Uuid& item, Done done)
{
    send(net::HttpMethod::Delete, std::format("/item/{}", item.toString()), nullptr,
         [this, item, done = std::move(done)](bool ok, const Model::Merged& merged)
         {
             if(ok)
             {
                 model_.remove(item);
             }
             if(done)
             {
                 done(ok, merged);
             }
         });
}

void Ais::update()
{
    replies_->drain(drained_);
    for(Reply& r : drained_)
    {
        const auto it = pending_.find(r.request);
        if(it == pending_.end())
        {
            continue; // cancelled
        }
        Done done = std::move(it->second);
        pending_.erase(it);
        Model::Merged merged;
        bool ok = false;
        if(r.response.ok())
        {
            if(auto reply = llsd::parse(r.response.bodyText()))
            {
                merged = model_.merge(*reply);
                ok = true;
            }
        }
        if(!ok && !cap_.empty())
        {
            log::warn("inventory: an AIS request failed (HTTP {})", r.response.status);
        }
        if(done)
        {
            done(ok, merged);
        }
    }
}

void Ais::cancel()
{
    requests_ = std::make_unique<net::HttpRequestGroup>(http_);
    replies_ = std::make_shared<MessageQueue<Reply>>();
    pending_.clear();
}

} // namespace viewer::inventory
