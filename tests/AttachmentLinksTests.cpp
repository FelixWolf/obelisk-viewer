#include "AttachmentLinks.hpp"

#include "viewer/llsd/Serialization.hpp"

#include <doctest/doctest.h>

#include <format>
#include <string>
#include <vector>

using namespace viewer;

namespace
{

// Requests kept, answered when the test says.
class FakeHttp final : public net::HttpClient
{
public:
    struct Sent
    {
        net::HttpRequest request;
        net::HttpCallback callback;
    };
    net::HttpRequestId send(net::HttpRequest request, net::HttpCallback callback) override
    {
        sent.push_back({std::move(request), std::move(callback)});
        return sent.size();
    }
    bool cancel(net::HttpRequestId) override { return false; }

    void answer(size_t i, std::string_view body, int status = 200)
    {
        net::HttpResponse response;
        response.requestId = i + 1;
        response.status = status;
        response.body.assign(body.begin(), body.end());
        sent[i].callback(std::move(response));
    }
    std::vector<Sent> sent;
};

Uuid id(int n)
{
    return *Uuid::parse(std::format("00000000-0000-0000-0000-{:012}", n));
}

std::string folderReply(const Uuid& cof, const std::string& links)
{
    return std::format(R"(<llsd><map>
        <key>category_id</key><uuid>{}</uuid><key>name</key><string>Current Outfit</string>
        <key>type_default</key><integer>46</integer><key>version</key><integer>3</integer>
        <key>_embedded</key><map><key>links</key><map>{}</map></map>
        </map></llsd>)",
                       cof.toString(), links);
}

std::string linkXml(const Uuid& link, const Uuid& cof, const Uuid& target)
{
    return std::format(R"(<key>{0}</key><map><key>item_id</key><uuid>{0}</uuid>
        <key>parent_id</key><uuid>{1}</uuid><key>linked_id</key><uuid>{2}</uuid>
        <key>type</key><integer>24</integer><key>name</key><string>link</string></map>)",
                       link.toString(), cof.toString(), target.toString());
}

} // namespace

TEST_CASE("Attachment links: the COF follows what we attach and take off")
{
    FakeHttp http;
    inventory::Model model;
    inventory::Ais ais(http, model);
    ais.setCapability("https://ais.example/agent");
    AttachmentLinks links(ais, model);

    const Uuid cof = id(1);
    const Uuid hat = id(10);
    inventory::Item item;
    item.id = hat;
    item.parent = id(2);
    item.name = "Hat";
    item.type = inventory::kObject;
    item.inventoryType = 17;
    model.put(item);

    // Attaching: the COF and its links are asked for first.
    links.attaching(hat, 0.0);
    CHECK(links.pending(hat));
    links.update({}, 0.5);
    REQUIRE(http.sent.size() == 1);
    CHECK(http.sent[0].request.url ==
          "https://ais.example/agent/category/current/children?depth=1");
    http.answer(0, folderReply(cof, ""));
    ais.update();
    links.update({}, 1.0); // not on yet: nothing
    CHECK(http.sent.size() == 1);

    // On: a link to it, made in the COF.
    links.update({hat}, 1.5);
    CHECK_FALSE(links.pending(hat));
    REQUIRE(http.sent.size() == 2);
    const auto& made = http.sent[1].request;
    CHECK(made.method == net::HttpMethod::Post);
    CHECK(made.url.starts_with(
        std::format("https://ais.example/agent/category/{}?tid=", cof.toString())));
    const auto body = llsd::parse(
        std::string_view(reinterpret_cast<const char*>(made.body.data()), made.body.size()));
    REQUIRE(body);
    const llsd::Value& link = (*body)["links"][0];
    CHECK(link["linked_id"].asUuid() == hat);
    CHECK(link["type"].asInteger() == inventory::kLink);
    CHECK(link["inv_type"].asInteger() == 17);
    CHECK(link["name"].asString() == "Hat");
    http.answer(1, folderReply(cof, linkXml(id(20), cof, hat)));
    ais.update();
    REQUIRE(model.item(id(20)));

    // Attaching what's linked already: no second link.
    links.attaching(hat, 2.0);
    links.update({hat}, 2.5);
    CHECK(http.sent.size() == 2);

    // Taking off: once it's off, its link removed.
    links.detaching(hat, 3.0);
    links.update({hat}, 3.5);
    CHECK(http.sent.size() == 2);
    links.update({}, 4.0);
    REQUIRE(http.sent.size() == 3);
    CHECK(http.sent[2].request.method == net::HttpMethod::Delete);
    CHECK(http.sent[2].request.url ==
          std::format("https://ais.example/agent/item/{}", id(20).toString()));
    http.answer(2, "<llsd><map/></llsd>");
    ais.update();
    CHECK_FALSE(model.item(id(20)));

    // Never arriving: given up on.
    links.attaching(hat, 10.0);
    links.update({}, 10.0 + AttachmentLinks::kTimeout + 1.0);
    CHECK_FALSE(links.pending(hat));
}
