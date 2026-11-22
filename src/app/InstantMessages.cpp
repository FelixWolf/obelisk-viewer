#include "InstantMessages.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/llsd/Serialization.hpp"
#include "viewer/llsd/Value.hpp"
#include "viewer/protocol/Messages.hpp"
#include "viewer/session/Session.hpp"

#include <algorithm>

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

// At most `bytes` long, not cutting a UTF-8 character.
std::string cut(std::string_view text, size_t bytes)
{
    if(text.size() <= bytes)
    {
        return std::string(text);
    }
    size_t end = bytes;
    while(end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
    {
        end--;
    }
    return std::string(text.substr(0, end));
}

// A binary bucket's text (the session's name): up to its terminator.
std::string bucketText(const std::vector<uint8_t>& bucket)
{
    const auto end = std::find(bucket.begin(), bucket.end(), uint8_t{0});
    return std::string(bucket.begin(), end);
}

std::chrono::system_clock::time_point fromUnix(int64_t seconds,
                                               std::chrono::system_clock::time_point fallback)
{
    return seconds > 0 ? std::chrono::system_clock::time_point(std::chrono::seconds(seconds))
                       : fallback;
}

} // namespace

Uuid directSession(const Uuid& self, const Uuid& other)
{
    if(self == other)
    {
        return self;
    }
    Uuid session;
    for(size_t i = 0; i < session.bytes.size(); i++)
    {
        session.bytes[i] = self.bytes[i] ^ other.bytes[i];
    }
    return session;
}

InstantMessages::InstantMessages(net::HttpClient* http) : http_(http)
{
    if(http_)
    {
        requests_ = std::make_unique<net::HttpRequestGroup>(*http_);
    }
}

InstantMessages::~InstantMessages()
{
    requests_.reset(); // no answers after this
}

const ImConversation* InstantMessages::find(const Uuid& session) const
{
    const auto it = std::ranges::find(conversations_, session, &ImConversation::session);
    return it == conversations_.end() ? nullptr : &*it;
}

ImConversation& InstantMessages::conversation(const Uuid& session, ImConversation::Kind kind,
                                              const Uuid& other, std::string_view name)
{
    auto it = std::ranges::find(conversations_, session, &ImConversation::session);
    if(it == conversations_.end())
    {
        ImConversation c;
        c.session = session;
        c.kind = kind;
        c.other = other;
        conversations_.push_back(std::move(c));
        it = conversations_.end() - 1;
        version_++;
    }
    if(it->name.empty() && !name.empty())
    {
        it->name = std::string(name);
        version_++;
    }
    return *it;
}

Uuid InstantMessages::start(const Uuid& self, const Uuid& other, std::string_view name,
                            std::chrono::system_clock::time_point now)
{
    const Uuid session = directSession(self, other);
    ImConversation& c = conversation(session, ImConversation::Kind::Direct, other, name);
    if(c.lines.empty())
    {
        c.latest = now;
        version_++;
        sortByLatest();
    }
    return session;
}

void InstantMessages::add(ImConversation& c, ImMessage message)
{
    c.latest = std::max(c.latest, message.when);
    if(!message.ours)
    {
        c.typing = false; // a line ends their typing
    }
    c.lines.push_back(std::move(message));
    while(c.lines.size() > kMaxLines)
    {
        c.lines.pop_front();
    }
    c.added++;
    version_++;
}

void InstantMessages::sortByLatest()
{
    std::ranges::stable_sort(conversations_, [](const ImConversation& a, const ImConversation& b)
                             { return a.latest > b.latest; });
}

std::optional<InstantMessages::FromObject>
InstantMessages::handle(const protocol::msg::ImprovedInstantMessage& m, const Uuid& self,
                        std::chrono::system_clock::time_point now)
{
    const auto& b = m.MessageBlock;
    const Uuid& from = m.AgentData.AgentID;
    const std::string name = cleanName(protocol::toString(b.FromAgentName));
    std::string text = protocol::toString(b.Message);
    const bool saved = b.Offline != 0;
    switch(b.Dialog)
    {
    case ImDialog::NothingSpecial:
    case ImDialog::BusyAutoResponse:
    {
        if(from.isNull() || b.ToAgentID.isNull())
        {
            log::info("a message to everyone from '{}': {}", name, text);
            return std::nullopt;
        }
        // Ours sent back (a second viewer of ours): it's the other's
        // conversation.
        const Uuid other = from == self ? b.ToAgentID : from;
        const Uuid session = b.ID.isNull() ? directSession(self, other) : b.ID;
        ImConversation& c =
            conversation(session, ImConversation::Kind::Direct, other, from == self ? "" : name);
        ImMessage line;
        line.when = saved ? fromUnix(b.Timestamp, now) : now;
        line.from = from;
        line.fromName = name;
        line.text = std::move(text);
        line.ours = from == self;
        line.saved = saved;
        line.response = b.Dialog == ImDialog::BusyAutoResponse;
        add(c, std::move(line));
        sortByLatest();
        return std::nullopt;
    }
    case ImDialog::SessionSend:
    {
        // Only for chats we're in (we accepted their invitation).
        auto it = std::ranges::find(conversations_, b.ID, &ImConversation::session);
        if(it == conversations_.end())
        {
            return std::nullopt;
        }
        if(it->name.empty())
        {
            it->name = bucketText(b.BinaryBucket);
        }
        ImMessage line;
        line.when = saved ? fromUnix(b.Timestamp, now) : now;
        line.from = from;
        line.fromName = name;
        line.text = std::move(text);
        line.ours = from == self;
        line.saved = saved;
        add(*it, std::move(line));
        sortByLatest();
        return std::nullopt;
    }
    case ImDialog::TypingStart:
    case ImDialog::TypingStop:
    {
        const Uuid session = b.ID.isNull() ? directSession(self, from) : b.ID;
        auto it = std::ranges::find(conversations_, session, &ImConversation::session);
        const bool typing = b.Dialog == ImDialog::TypingStart;
        if(it == conversations_.end())
        {
            if(!typing)
            {
                return std::nullopt;
            }
            // Someone starts writing to us: their conversation, with them
            // typing.
            it = conversations_.begin() +
                 (&conversation(session, ImConversation::Kind::Direct, from, name) -
                  conversations_.data());
        }
        if(it->typing != typing)
        {
            it->typing = typing;
            version_++;
        }
        return std::nullopt;
    }
    case ImDialog::FromTask:
        // An object's: to local chat (its owner is the sender; the object
        // is the session).
        return FromObject{b.ID, from, name, std::move(text)};
    default:
        log::info("instant message of a kind not handled yet (dialog {}) from '{}': {}",
                  static_cast<int>(b.Dialog), name, text);
        return std::nullopt;
    }
}

std::optional<Uuid> InstantMessages::invitation(const llsd::Value& body, const Uuid& self,
                                                std::chrono::system_clock::time_point now)
{
    if(!body.has("instantmessage"))
    {
        return std::nullopt; // voice, or an older kind
    }
    const llsd::Value& p = body["instantmessage"]["message_params"];
    const Uuid session = p["id"].asUuid();
    const Uuid from = p["from_id"].asUuid();
    if(session.isNull())
    {
        return std::nullopt;
    }
    const llsd::Binary bucket = p["data"]["binary_bucket"].asBinary();
    std::string name = bucketText(bucket);
    if(name.empty())
    {
        name = body["session_name"].asString();
    }
    ImConversation& c = conversation(session, ImConversation::Kind::Session, {}, name);
    if(from != self)
    {
        ImMessage line;
        line.saved = p["offline"].asInteger() != 0;
        line.when = line.saved ? fromUnix(p["timestamp"].asInteger(), now) : now;
        line.from = from;
        line.fromName = cleanName(p["from_name"].asString());
        line.text = p["message"].asString();
        add(c, std::move(line));
        sortByLatest();
    }
    if(http_)
    {
        toAccept_.push_back(session);
    }
    return session;
}

std::vector<InstantMessages::FromObject> InstantMessages::offline(const llsd::Value& reply,
                                                                  const Uuid& self)
{
    std::vector<FromObject> objects;
    // Either an array of messages, or a map holding one.
    const llsd::Value& messages = reply.isArray() ? reply : reply["messages"];
    for(size_t i = 0; i < messages.size(); i++)
    {
        const llsd::Value& msg = messages[i];
        const auto dialog = static_cast<uint8_t>(msg["dialog"].asInteger());
        const Uuid from = msg["from_agent_id"].asUuid();
        const std::string name = cleanName(msg["from_agent_name"].asString());
        const auto when = fromUnix(msg["timestamp"].asInteger(), std::chrono::system_clock::now());
        Uuid session = msg["transaction-id"].asUuid();
        if(dialog == ImDialog::FromTask)
        {
            objects.push_back({session.isNull() ? msg["asset_id"].asUuid() : session, from, name,
                               msg["message"].asString()});
            continue;
        }
        if(dialog != ImDialog::NothingSpecial || from.isNull())
        {
            offlineOthers_.push_back(offlineMessage(msg));
            continue;
        }
        if(session.isNull())
        {
            session = directSession(self, from);
        }
        ImConversation& c = conversation(session, ImConversation::Kind::Direct, from, name);
        ImMessage line;
        line.when = when;
        line.from = from;
        line.fromName = name;
        line.text = msg["message"].asString();
        line.saved = true;
        add(c, std::move(line));
    }
    sortByLatest();
    return objects;
}

std::optional<InstantMessages::Outgoing>
InstantMessages::send(const Uuid& session, std::string_view text, const Uuid& self,
                      std::string_view ourName, std::chrono::system_clock::time_point now)
{
    auto it = std::ranges::find(conversations_, session, &ImConversation::session);
    if(it == conversations_.end() || text.empty())
    {
        return std::nullopt;
    }
    Outgoing out;
    out.session = session;
    out.text = cut(text, kMaxText);
    if(it->kind == ImConversation::Kind::Direct)
    {
        out.to = it->other;
        out.dialog = ImDialog::NothingSpecial;
        // Shown at once: the region doesn't send it back.
        ImMessage line;
        line.when = now;
        line.from = self;
        line.fromName = cleanName(std::string(ourName));
        line.text = out.text;
        line.ours = true;
        add(*it, std::move(line));
        sortByLatest();
    }
    else
    {
        out.to = session;
        out.dialog = ImDialog::SessionSend;
    }
    return out;
}

void InstantMessages::update(const session::Session& session)
{
    if(!requests_)
    {
        return;
    }
    const auto* login = session.login();
    // What came while we were away, once the region can say.
    if(!offlineAsked_ && login && session.state() == session::SessionState::Connected)
    {
        if(const auto url = session.capability("ReadOfflineMsgs"))
        {
            offlineAsked_ = true;
            net::HttpRequest request{.method = net::HttpMethod::Get, .url = std::string(*url)};
            request.headers.add("Accept", std::string(llsd::kXmlContentType));
            requests_->send(std::move(request),
                            [answers = answers_, generation = generation_](net::HttpResponse&& r)
                            { answers->push({generation, true, {}, std::move(r)}); });
        }
    }
    // Invitations to accept.
    if(!toAccept_.empty())
    {
        if(const auto url = session.capability("ChatSessionRequest"))
        {
            for(const Uuid& id : toAccept_)
            {
                llsd::Value body;
                body["method"] = "accept invitation";
                body["session-id"] = id;
                net::HttpRequest request{.method = net::HttpMethod::Post, .url = std::string(*url)};
                request.headers.add("Content-Type", std::string(llsd::kXmlContentType));
                request.headers.add("Accept", std::string(llsd::kXmlContentType));
                const std::string xml = llsd::toXml(body);
                request.body.assign(xml.begin(), xml.end());
                requests_->send(std::move(request), [answers = answers_, generation = generation_,
                                                     id](net::HttpResponse&& r)
                                { answers->push({generation, false, id, std::move(r)}); });
            }
            toAccept_.clear();
        }
    }
    std::vector<Answer> answers;
    answers_->drain(answers);
    for(Answer& a : answers)
    {
        if(a.generation != generation_)
        {
            continue;
        }
        if(!a.response.ok())
        {
            log::warn("{}: HTTP {}", a.offline ? "saved messages" : "joining a chat",
                      a.response.status);
            continue;
        }
        if(a.offline && login)
        {
            if(const auto reply = llsd::parse(a.response.bodyText()))
            {
                auto objects = offline(*reply, login->agentId);
                offlineFromObjects_.insert(offlineFromObjects_.end(), objects.begin(),
                                           objects.end());
                log::info("saved messages read");
            }
        }
    }
}

std::vector<protocol::msg::ImprovedInstantMessage> InstantMessages::takeOfflineOthers()
{
    return std::exchange(offlineOthers_, {});
}

protocol::msg::ImprovedInstantMessage offlineMessage(const llsd::Value& record)
{
    protocol::msg::ImprovedInstantMessage m;
    m.AgentData.AgentID = record["from_agent_id"].asUuid();
    auto& b = m.MessageBlock;
    const llsd::Value& group = record["from_group"];
    b.FromGroup = group.type() == llsd::Value::Type::String ? group.asString() == "Y"
                                                            : group.asInteger() != 0;
    b.ToAgentID = record["to_agent_id"].asUuid();
    b.Offline = record.has("offline") ? static_cast<uint8_t>(record["offline"].asInteger()) : 1;
    b.Dialog = static_cast<uint8_t>(record["dialog"].asInteger());
    b.ID = record["transaction-id"].asUuid();
    if(b.ID.isNull() && b.Dialog == ImDialog::FromTask)
    {
        b.ID = record["asset_id"].asUuid();
    }
    b.Timestamp = static_cast<uint32_t>(record["timestamp"].asInteger());
    b.FromAgentName = protocol::fromString(record["from_agent_name"].asString());
    b.Message = protocol::fromString(record["message"].asString());
    b.ParentEstateID = record.has("parent_estate_id")
                           ? static_cast<uint32_t>(record["parent_estate_id"].asInteger())
                           : 1;
    b.RegionID = record["region_id"].asUuid();
    if(record.has("binary_bucket"))
    {
        const llsd::Binary bucket = record["binary_bucket"].asBinary();
        b.BinaryBucket.assign(bucket.begin(), bucket.end());
    }
    else
    {
        b.BinaryBucket = {0};
    }
    return m;
}

void InstantMessages::clear()
{
    conversations_.clear();
    toAccept_.clear();
    offlineFromObjects_.clear();
    offlineOthers_.clear();
    offlineAsked_ = false;
    generation_++;
    if(requests_)
    {
        requests_->cancelAll();
    }
    version_++;
}

} // namespace viewer
