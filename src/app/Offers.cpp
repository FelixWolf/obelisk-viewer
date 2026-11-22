#include "Offers.hpp"

#include "InstantMessages.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/llsd/Serialization.hpp"
#include "viewer/llsd/Value.hpp"
#include "viewer/protocol/Messages.hpp"
#include "viewer/session/Session.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <string_view>

namespace viewer
{

namespace
{

// "First Resident" -> "First".
std::string cleanName(std::string name)
{
    constexpr std::string_view kResident = " Resident";
    if(name.ends_with(kResident))
    {
        name.resize(name.size() - kResident.size());
    }
    return name;
}

// '|'-separated fields (empty ones kept).
std::vector<std::string_view> fields(std::string_view text)
{
    std::vector<std::string_view> out;
    size_t start = 0;
    for(;;)
    {
        const size_t bar = text.find('|', start);
        out.push_back(text.substr(start, bar == std::string_view::npos ? bar : bar - start));
        if(bar == std::string_view::npos)
        {
            return out;
        }
        start = bar + 1;
    }
}

int toInt(std::string_view text, int fallback)
{
    while(!text.empty() && text.front() == ' ')
    {
        text.remove_prefix(1);
    }
    int value = fallback;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc() ? value : fallback;
}

std::string_view trimmed(std::string_view text)
{
    while(!text.empty() && (text.front() == ' ' || text.front() == '\r' || text.front() == '\n'))
    {
        text.remove_prefix(1);
    }
    while(!text.empty() && (text.back() == ' ' || text.back() == '\r' || text.back() == '\n'))
    {
        text.remove_suffix(1);
    }
    return text;
}

// Up to the bucket's terminator.
std::string bucketText(const std::vector<uint8_t>& bucket)
{
    const auto end = std::find(bucket.begin(), bucket.end(), uint8_t{0});
    return std::string(bucket.begin(), end);
}

Uuid uuidAt(const std::vector<uint8_t>& bucket, size_t at)
{
    Uuid id;
    if(bucket.size() >= at + id.bytes.size())
    {
        std::copy_n(bucket.begin() + static_cast<std::ptrdiff_t>(at), id.bytes.size(),
                    id.bytes.begin());
    }
    return id;
}

// For a URL's query.
std::string escaped(std::string_view text)
{
    std::string out;
    for(const char c : text)
    {
        const auto u = static_cast<unsigned char>(c);
        if(std::isalnum(u) || c == '-' || c == '_' || c == '.' || c == '~')
        {
            out += c;
        }
        else
        {
            out += std::format("%{:02X}", u);
        }
    }
    return out;
}

} // namespace

Offers::Offers(net::HttpClient* http) : http_(http)
{
    if(http_)
    {
        requests_ = std::make_unique<net::HttpRequestGroup>(*http_);
    }
}

Offers::~Offers()
{
    requests_.reset();
}

void Offers::add(Offer offer)
{
    offer.serial = ++serial_;
    // One friendship offer from someone at a time; a newer teleport offer
    // from them replaces the one waiting.
    for(auto it = waiting_.begin(); it != waiting_.end(); ++it)
    {
        if(it->kind != offer.kind || it->from != offer.from)
        {
            continue;
        }
        if(offer.kind == Offer::Kind::Friendship)
        {
            return;
        }
        if(offer.kind == Offer::Kind::Teleport || offer.kind == Offer::Kind::TeleportRequest)
        {
            waiting_.erase(it);
            break;
        }
    }
    waiting_.push_back(std::move(offer));
    while(waiting_.size() > kMaxWaiting)
    {
        // Gone unanswered: an object's offer is declined.
        if(waiting_.front().kind == Offer::Kind::ObjectInventory)
        {
            answer(waiting_.front(), false);
        }
        waiting_.pop_front();
    }
    version_++;
}

bool Offers::handle(const protocol::msg::ImprovedInstantMessage& m, uint64_t region)
{
    const auto& b = m.MessageBlock;
    Offer o;
    o.region = region;
    o.from = m.AgentData.AgentID;
    o.fromName = cleanName(protocol::toString(b.FromAgentName));
    o.transaction = b.ID;
    o.offline = b.Offline != 0;
    o.message = protocol::toString(b.Message);
    const std::vector<uint8_t>& bucket = b.BinaryBucket;

    switch(b.Dialog)
    {
    case ImDialog::LureUser:
    case ImDialog::TeleportRequest:
    {
        o.kind =
            b.Dialog == ImDialog::LureUser ? Offer::Kind::Teleport : Offer::Kind::TeleportRequest;
        // "global x|global y|x|y|z|look x|look y|look z|rating"
        const std::string text = bucketText(bucket);
        const auto f = fields(text);
        if(o.kind == Offer::Kind::Teleport && f.size() >= 9)
        {
            const std::string_view rating = trimmed(f[8]);
            o.rating = rating == "A"    ? "Adult"
                       : rating == "M"  ? "Moderate"
                       : rating == "PG" ? "General"
                                        : "";
        }
        add(std::move(o));
        return true;
    }
    case ImDialog::GodlikeLureUser:
    {
        // A summons: no asking.
        log::info("summoned by '{}'", o.fromName);
        OfferAnswer a;
        a.lure = b.ID;
        a.godlike = true;
        answers_.push_back(std::move(a));
        return true;
    }
    case ImDialog::FriendshipOffered:
        o.kind = Offer::Kind::Friendship;
        add(std::move(o));
        return true;
    case ImDialog::GroupInvitation:
    {
        // The bucket: the fee (big-endian) and the role.
        if(bucket.size() != 20)
        {
            log::warn("a group invitation from '{}' that can't be read", o.fromName);
            return true;
        }
        o.kind = Offer::Kind::GroupInvite;
        o.fee = static_cast<int32_t>(uint32_t{bucket[0]} << 24 | uint32_t{bucket[1]} << 16 |
                                     uint32_t{bucket[2]} << 8 | uint32_t{bucket[3]});
        o.group = b.FromGroup ? o.from : b.RegionID;
        add(std::move(o));
        return true;
    }
    case ImDialog::InventoryOffered:
    {
        // The bucket: the asset type and the item (ours already).
        if(bucket.size() != 17)
        {
            log::warn("an inventory offer from '{}' that can't be read", o.fromName);
            return true;
        }
        o.kind = Offer::Kind::Inventory;
        o.assetType = static_cast<int8_t>(bucket[0]);
        o.item = uuidAt(bucket, 1);
        o.itemName = o.message;
        if(o.transaction.isNull())
        {
            o.transaction = b.RegionID;
        }
        // Taken into its folder as it comes (the official viewer's too).
        OfferAnswer a;
        a.im = OfferAnswer::Im{o.from, o.transaction, ImDialog::InventoryAccepted,
                               folderTypeOf(o.assetType)};
        answers_.push_back(std::move(a));
        add(std::move(o));
        return true;
    }
    case ImDialog::TaskInventoryOffered:
    {
        o.kind = Offer::Kind::ObjectInventory;
        o.fromObject = true; // from: its owner; the name: the object's
        // The bucket: the asset type (older regions: "type|...").
        o.assetType = bucket.size() == 1 ? static_cast<int8_t>(bucket[0])
                                         : toInt(fields(bucketText(bucket))[0], -1);
        o.itemName = o.message;
        if(o.transaction.isNull())
        {
            o.transaction = b.RegionID;
        }
        add(std::move(o));
        return true;
    }
    case ImDialog::GroupNotice:
    case ImDialog::GroupNoticeRequested:
    {
        if(b.Dialog == ImDialog::GroupNoticeRequested)
        {
            return true; // asked for from a group's notices list (not here yet)
        }
        o.kind = Offer::Kind::GroupNotice;
        bool attached = false;
        if(!b.RegionID.isNull())
        {
            // The group; the bucket: "asset type|wearable type|name".
            o.group = b.RegionID;
            attached = bucket.size() > 1;
            if(attached)
            {
                const auto f = fields(bucketText(bucket));
                o.assetType = toInt(f[0], -1);
                o.itemName = f.size() > 2 ? std::string(f[2]) : std::string();
            }
        }
        else
        {
            // The bucket: attached, asset type, the group, the name.
            if(bucket.size() < 19 || bucket.back() != 0)
            {
                log::warn("a group notice that can't be read");
                return true;
            }
            attached = bucket[0] != 0;
            o.assetType = attached ? static_cast<int8_t>(bucket[1]) : -1;
            o.group = uuidAt(bucket, 2);
            o.itemName = bucketText(std::vector<uint8_t>(bucket.begin() + 18, bucket.end()));
        }
        if(!attached)
        {
            o.assetType = -1;
            o.itemName.clear();
        }
        // The message: "subject|text".
        const size_t bar = o.message.find('|');
        o.subject = o.message.substr(0, bar);
        o.message = bar == std::string::npos ? std::string() : o.message.substr(bar + 1);
        add(std::move(o));
        return true;
    }
    case ImDialog::GotoUrl:
    {
        o.kind = Offer::Kind::Url;
        o.url = bucketText(bucket);
        if(o.url.empty())
        {
            return true;
        }
        add(std::move(o));
        return true;
    }
    case ImDialog::MessageBox:
    case ImDialog::FromTaskAsAlert:
        o.kind = Offer::Kind::Message;
        o.fromObject = b.Dialog == ImDialog::FromTaskAsAlert;
        add(std::move(o));
        return true;
    case ImDialog::InventoryAccepted:
    case ImDialog::TaskInventoryAccepted:
        log::info("'{}' accepted what we gave", o.fromName);
        return true;
    case ImDialog::InventoryDeclined:
    case ImDialog::TaskInventoryDeclined:
        log::info("'{}' declined what we gave", o.fromName);
        return true;
    case ImDialog::LureAccepted:
    case ImDialog::LureDeclined:
        log::info("'{}' {} our teleport offer", o.fromName,
                  b.Dialog == ImDialog::LureAccepted ? "accepted" : "declined");
        return true;
    case ImDialog::FriendshipAccepted:
        log::info("'{}' accepted our friendship offer", o.fromName);
        friendships_.push_back(o.from);
        return true;
    case ImDialog::GroupInvitationAccept:
    case ImDialog::GroupInvitationDecline:
    case ImDialog::GroupNoticeInventoryAccepted:
    case ImDialog::GroupNoticeInventoryDeclined:
        return true; // others' answers, not ours to see
    default:
        return false;
    }
}

void Offers::answer(const Offer& o, bool accepted)
{
    OfferAnswer a;
    switch(o.kind)
    {
    case Offer::Kind::Teleport:
        if(accepted)
        {
            a.lure = o.transaction;
        }
        else
        {
            a.im = OfferAnswer::Im{o.from, o.transaction, ImDialog::LureDeclined, std::nullopt};
        }
        break;
    case Offer::Kind::TeleportRequest:
        if(!accepted)
        {
            return; // nothing said
        }
        a.offerTeleportTo = o.from;
        break;
    case Offer::Kind::Friendship:
        if(accepted)
        {
            friendships_.push_back(o.from);
        }
        // Offered while we were away: by capability (no transaction then).
        if(o.offline || o.transaction.isNull())
        {
            byCapability_.push_back({ByCapability::Kind::Friendship, accepted, o.from});
            return;
        }
        a.friendship = accepted;
        a.transaction = o.transaction;
        break;
    case Offer::Kind::GroupInvite:
        if(o.offline && o.transaction.isNull())
        {
            byCapability_.push_back({ByCapability::Kind::Group, accepted, o.group});
            return;
        }
        a.im = OfferAnswer::Im{o.group, o.transaction,
                               accepted ? ImDialog::GroupInvitationAccept
                                        : ImDialog::GroupInvitationDecline,
                               std::nullopt};
        break;
    case Offer::Kind::Inventory:
        if(accepted)
        {
            return; // kept: it's already ours
        }
        if(o.item.isNull())
        {
            return;
        }
        a.trash = o.item;
        break;
    case Offer::Kind::ObjectInventory:
        if(o.transaction.isNull())
        {
            return;
        }
        a.im = accepted ? OfferAnswer::Im{o.from, o.transaction, ImDialog::TaskInventoryAccepted,
                                          folderTypeOf(o.assetType)}
                        : OfferAnswer::Im{o.from, o.transaction, ImDialog::TaskInventoryDeclined,
                                          std::nullopt};
        break;
    case Offer::Kind::GroupNotice:
        if(o.assetType < 0 || o.transaction.isNull())
        {
            return; // nothing attached: nothing to say
        }
        a.im = accepted
                   ? OfferAnswer::Im{o.from, o.transaction, ImDialog::GroupNoticeInventoryAccepted,
                                     folderTypeOf(o.assetType)}
                   : OfferAnswer::Im{o.from, o.transaction, ImDialog::GroupNoticeInventoryDeclined,
                                     std::nullopt};
        break;
    case Offer::Kind::Url:
        if(!accepted)
        {
            return;
        }
        a.url = o.url;
        break;
    case Offer::Kind::Message:
        return;
    }
    answers_.push_back(std::move(a));
}

void Offers::accept()
{
    if(waiting_.empty())
    {
        return;
    }
    const Offer o = std::move(waiting_.front());
    waiting_.pop_front();
    version_++;
    answer(o, true);
}

void Offers::decline()
{
    if(waiting_.empty())
    {
        return;
    }
    const Offer o = std::move(waiting_.front());
    waiting_.pop_front();
    version_++;
    answer(o, false);
}

void Offers::update(const session::Session& session)
{
    if(!requests_)
    {
        byCapability_.clear();
        return;
    }
    const auto* login = session.login();
    for(auto it = byCapability_.begin(); it != byCapability_.end();)
    {
        const bool friendship = it->kind == ByCapability::Kind::Friendship;
        const auto url = session.capability(
            friendship ? (it->accept ? "AcceptFriendship" : "DeclineFriendship")
                       : (it->accept ? "AcceptGroupInvite" : "DeclineGroupInvite"));
        if(!url || !login)
        {
            ++it; // when the region says how
            continue;
        }
        net::HttpRequest request;
        request.url = std::string(*url);
        std::string what;
        if(friendship)
        {
            request.url += "?from=" + it->id.toString();
            if(it->accept)
            {
                const std::string name = login->name.first + " " + login->name.last;
                request.url += "&agent_name=%22" + escaped(name) + "%22";
                request.method = net::HttpMethod::Post;
                const std::string xml = llsd::toXml(llsd::Value());
                request.body.assign(xml.begin(), xml.end());
                request.headers.add("Content-Type", std::string(llsd::kXmlContentType));
            }
            else
            {
                request.method = net::HttpMethod::Delete;
            }
            what = it->accept ? "accepting friendship" : "declining friendship";
        }
        else
        {
            llsd::Value body;
            body["group"] = it->id;
            request.method = net::HttpMethod::Post;
            const std::string xml = llsd::toXml(body);
            request.body.assign(xml.begin(), xml.end());
            request.headers.add("Content-Type", std::string(llsd::kXmlContentType));
            what = it->accept ? "joining a group" : "declining a group";
        }
        request.headers.add("Accept", std::string(llsd::kXmlContentType));
        requests_->send(std::move(request), [replies = replies_, what](net::HttpResponse&& r)
                        { replies->push({what, std::move(r)}); });
        it = byCapability_.erase(it);
    }
    std::vector<Reply> replies;
    replies_->drain(replies);
    for(Reply& r : replies)
    {
        const auto body = llsd::parse(r.response.bodyText());
        if(!r.response.ok() || !body || !(*body)["success"].asBoolean())
        {
            log::warn("{}: failed (HTTP {})", r.what, r.response.status);
        }
        else
        {
            log::info("{}: done", r.what);
        }
    }
}

void Offers::clear()
{
    waiting_.clear();
    answers_.clear();
    friendships_.clear();
    byCapability_.clear();
    if(requests_)
    {
        requests_->cancelAll();
    }
    version_++;
}

} // namespace viewer
