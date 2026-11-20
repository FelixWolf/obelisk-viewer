#include "AccountPicture.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/llsd/Serialization.hpp"

#include <format>
#include <vector>

namespace viewer
{

AccountPicture::AccountPicture(net::HttpClient& http)
    : http_(http), requests_(std::make_unique<net::HttpRequestGroup>(http))
{
}

void AccountPicture::update(const Uuid& agent, std::string_view capability,
                            const std::function<void(const Uuid&)>& found)
{
    if(asked_ != agent && !capability.empty() && !agent.isNull())
    {
        asked_ = agent;
        net::HttpRequest request{.url = std::format("{}/{}", capability, agent.toString())};
        request.headers.add("Accept", std::string(llsd::kXmlContentType));
        requests_->send(std::move(request),
                        [answers = answers_](net::HttpResponse&& response)
                        {
                            std::optional<Uuid> image;
                            if(response.ok())
                            {
                                if(const auto reply = llsd::parse(response.bodyText()))
                                {
                                    image = (*reply)["sl_image_id"].asUuid();
                                }
                            }
                            answers->push(image);
                        });
    }
    std::vector<std::optional<Uuid>> answers;
    answers_->drain(answers);
    for(const std::optional<Uuid>& image : answers)
    {
        if(image)
        {
            found(*image);
        }
        else
        {
            log::info("our profile picture couldn't be had");
        }
    }
}

void AccountPicture::clear()
{
    requests_ = std::make_unique<net::HttpRequestGroup>(http_);
    answers_ = std::make_shared<MessageQueue<std::optional<Uuid>>>();
    asked_ = Uuid();
}

} // namespace viewer
