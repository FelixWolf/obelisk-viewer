#include "CurrentOutfit.hpp"

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

std::string folderReply(const Uuid& cof, const std::string& links, int version = 3)
{
    return std::format(R"(<llsd><map>
        <key>category_id</key><uuid>{}</uuid><key>name</key><string>Current Outfit</string>
        <key>type_default</key><integer>46</integer><key>version</key><integer>{}</integer>
        <key>_embedded</key><map><key>links</key><map>{}</map></map>
        </map></llsd>)",
                       cof.toString(), version, links);
}

std::string linkXml(const Uuid& link, const Uuid& cof, const Uuid& target)
{
    return std::format(R"(<key>{0}</key><map><key>item_id</key><uuid>{0}</uuid>
        <key>parent_id</key><uuid>{1}</uuid><key>linked_id</key><uuid>{2}</uuid>
        <key>type</key><integer>24</integer><key>name</key><string>link</string></map>)",
                       link.toString(), cof.toString(), target.toString());
}

} // namespace

TEST_CASE("Current outfit: the COF follows what we attach and take off")
{
    FakeHttp http;
    inventory::Model model;
    inventory::Ais ais(http, model);
    ais.setCapability("https://ais.example/agent");
    CurrentOutfit links(http, ais, model);

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
    links.update({}, 10.0 + CurrentOutfit::kTimeout + 1.0);
    CHECK_FALSE(links.pending(hat));
}

TEST_CASE("Current outfit: wearing clothing, then asking for our look")
{
    FakeHttp http;
    inventory::Model model;
    inventory::Ais ais(http, model);
    ais.setCapability("https://ais.example/agent");
    CurrentOutfit outfit(http, ais, model);
    outfit.setAppearanceCapability("https://sim.example/appearance");

    const Uuid cof = id(1);
    const auto put = [&](int n, int type, int wearable, const char* name)
    {
        inventory::Item item;
        item.id = id(n);
        item.parent = id(2);
        item.name = name;
        item.type = type;
        item.inventoryType = 18;
        item.flags = static_cast<uint32_t>(wearable);
        model.put(item);
    };
    put(10, CurrentOutfit::kClothing, 4, "Old shirt");
    put(11, CurrentOutfit::kClothing, 4, "New shirt");
    put(12, CurrentOutfit::kClothing, 5, "Pants");

    // The COF: the old shirt and the pants worn.
    outfit.wear(id(11), CurrentOutfit::How::Replace);
    CHECK(outfit.pending(id(11)));
    outfit.update({}, 0.0);
    REQUIRE(http.sent.size() == 1);
    http.answer(0, folderReply(cof, linkXml(id(20), cof, id(10)) + linkXml(id(21), cof, id(12))));
    ais.update();
    CHECK(outfit.wearing(id(10)));
    CHECK_FALSE(outfit.wearing(id(11)));

    // Worn instead of the old shirt (not the pants): a link made, the old
    // one's removed.
    outfit.update({}, 0.1);
    REQUIRE(http.sent.size() == 3);
    CHECK(http.sent[1].request.method == net::HttpMethod::Post);
    CHECK(http.sent[2].request.method == net::HttpMethod::Delete);
    CHECK(http.sent[2].request.url.ends_with(id(20).toString()));
    outfit.update({}, 0.2); // not asked for our look until both are done
    CHECK(http.sent.size() == 3);
    http.answer(1, folderReply(cof, linkXml(id(22), cof, id(11))));
    http.answer(2, "<llsd><map/></llsd>");
    ais.update();
    CHECK(outfit.wearing(id(11)));
    CHECK_FALSE(outfit.wearing(id(10)));
    CHECK(outfit.wearing(id(12)));

    // Then the COF's version after the changes, and our look asked for.
    outfit.update({}, 0.3);
    REQUIRE(http.sent.size() == 4);
    CHECK(http.sent[3].request.url ==
          "https://ais.example/agent/category/current/children?depth=1");
    http.answer(3,
                folderReply(cof, linkXml(id(21), cof, id(12)) + linkXml(id(22), cof, id(11)), 7));
    ais.update();
    REQUIRE(http.sent.size() == 5);
    CHECK(http.sent[4].request.url == "https://sim.example/appearance");
    const auto asked = [&](size_t i)
    {
        const auto& body = http.sent[i].request.body;
        return llsd::parse(
                   std::string_view(reinterpret_cast<const char*>(body.data()), body.size()))
            .value_or(llsd::Value())["cof_version"]
            .asInteger();
    };
    CHECK(asked(4) == 7);
    CHECK(outfit.pending(id(11)));

    // A stale version: asked again with the one expected; then done.
    http.answer(4, "<llsd><map><key>success</key><boolean>0</boolean>"
                   "<key>expected</key><integer>8</integer></map></llsd>");
    outfit.update({}, 0.4);
    REQUIRE(http.sent.size() == 6);
    CHECK(asked(5) == 8);
    http.answer(5, "<llsd><map><key>success</key><boolean>1</boolean></map></llsd>");
    outfit.update({}, 0.5);
    CHECK_FALSE(outfit.pending(id(11)));

    // Taking off: its link removed, our look asked for again.
    outfit.takeOff(id(11));
    outfit.update({}, 1.0);
    REQUIRE(http.sent.size() == 7);
    CHECK(http.sent[6].request.method == net::HttpMethod::Delete);
    CHECK(http.sent[6].request.url.ends_with(id(22).toString()));
}

TEST_CASE("Current outfit: wearing an outfit instead of what's worn")
{
    FakeHttp http;
    inventory::Model model;
    inventory::Ais ais(http, model);
    ais.setCapability("https://ais.example/agent");
    CurrentOutfit outfit(http, ais, model);
    outfit.setAppearanceCapability("https://sim.example/appearance");

    const Uuid cof = id(1);
    const Uuid party = id(3);
    const auto put = [&](int n, int type, int wearable, const char* name)
    {
        inventory::Item item;
        item.id = id(n);
        item.parent = id(2);
        item.name = name;
        item.type = type;
        item.inventoryType = type == inventory::kObject ? 17 : 18;
        item.flags = static_cast<uint32_t>(wearable);
        model.put(item);
    };
    put(10, CurrentOutfit::kBodyPart, 0, "Old shape");
    put(11, CurrentOutfit::kBodyPart, 1, "Skin");
    put(12, CurrentOutfit::kClothing, 4, "Shirt");
    put(13, inventory::kObject, 0, "Old hat");
    put(14, CurrentOutfit::kBodyPart, 0, "Party shape");
    put(15, CurrentOutfit::kClothing, 5, "Party pants");
    put(16, inventory::kObject, 0, "Party hat");
    put(17, inventory::kGesture, 0, "Wave");
    put(18, 7, 0, "A notecard");
    model.put(inventory::Folder{party, id(4), "Party", inventory::kOutfit, 1});
    const auto link = [&](int n, const Uuid& folder, int target, const char* desc = "")
    {
        inventory::Item l;
        l.id = id(n);
        l.parent = folder;
        l.name = "link";
        l.description = desc;
        l.type = inventory::kLink;
        l.linkedId = id(target);
        model.put(l);
    };
    link(30, party, 14);
    link(31, party, 15, "@1");
    link(32, party, 16);
    link(33, party, 17);
    link(34, party, 18);

    outfit.wearOutfit(party);
    CHECK(outfit.outfitPending());
    outfit.update({id(13)}, 0.0);
    REQUIRE(http.sent.size() == 1); // the COF first
    http.answer(0,
                folderReply(cof, linkXml(id(20), cof, id(10)) + linkXml(id(21), cof, id(11)) +
                                     linkXml(id(22), cof, id(12)) + linkXml(id(23), cof, id(13))));
    ais.update();

    // The COF replaced: the outfit's shape, the skin worn (it has none), its
    // pants (their order kept), hat and gesture, and a link to it.
    outfit.update({id(13)}, 0.1);
    REQUIRE(http.sent.size() == 2);
    const auto& slam = http.sent[1].request;
    CHECK(slam.method == net::HttpMethod::Put);
    CHECK(slam.url.starts_with(
        std::format("https://ais.example/agent/category/{}/links?tid=", cof.toString())));
    const auto body = llsd::parse(
        std::string_view(reinterpret_cast<const char*>(slam.body.data()), slam.body.size()));
    REQUIRE(body);
    REQUIRE(body->size() == 6);
    std::vector<Uuid> targets;
    for(size_t i = 0; i < body->size(); i++)
    {
        targets.push_back(body->at(i)["linked_id"].asUuid());
    }
    CHECK(targets == std::vector<Uuid>{id(14), id(11), id(15), id(16), id(17), party});
    CHECK(body->at(2)["desc"].asString() == "@1");
    CHECK(body->at(5)["type"].asInteger() == inventory::kFolderLink);
    CHECK(outfit.takeAttachmentChanges({id(13)}).attach.empty()); // not until it's done

    // Done: the attachments to match (the outfit's hat on, the old one off),
    // and our look asked for from the COF as it is now.
    http.answer(1, "<llsd><map/></llsd>");
    ais.update();
    const auto changes = outfit.takeAttachmentChanges({id(13)});
    CHECK(changes.attach == std::vector<Uuid>{id(16)});
    CHECK(changes.detach == std::vector<Uuid>{id(13)});
    CHECK(outfit.takeAttachmentChanges({id(13)}).detach.empty()); // once
    outfit.update({id(13)}, 0.2);
    REQUIRE(http.sent.size() == 3);
    CHECK(http.sent[2].request.url ==
          "https://ais.example/agent/category/current/children?depth=1");
    http.answer(2, folderReply(cof, linkXml(id(40), cof, id(14)) + linkXml(id(41), cof, id(11))));
    ais.update();
    CHECK_FALSE(model.item(id(22))); // what's gone from the COF, gone from the model
    CHECK(outfit.wearing(id(14)));
    CHECK_FALSE(outfit.wearing(id(10)));
    REQUIRE(http.sent.size() == 4);
    CHECK(http.sent[3].request.url == "https://sim.example/appearance");
}
