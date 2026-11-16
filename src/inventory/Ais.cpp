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
    const uint64_t id = nextRequest_++;
    if(cap_.empty())
    {
        // Reported from update(), as replies are.
        replies_->push({id, net::HttpResponse{.error = net::HttpError::InvalidRequest}});
        pending_[id] = std::move(done);
        return;
    }
    net::HttpRequest request{.url = url(path)};
    request.headers.add("Accept", std::string(llsd::kXmlContentType));
    pending_[id] = std::move(done);
    requests_->send(std::move(request), [replies = replies_, id](net::HttpResponse&& response)
                    { replies->push({id, std::move(response)}); });
}

void Ais::fetchChildren(std::string_view folder, int depth, Done done)
{
    get(std::format("/category/{}/children?depth={}", folder, depth), std::move(done));
}

void Ais::fetchLinks(std::string_view folder, Done done)
{
    get(std::format("/category/{}/links", folder), std::move(done));
}

void Ais::fetchItem(const Uuid& item, Done done)
{
    get(std::format("/item/{}", item.toString()), std::move(done));
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
