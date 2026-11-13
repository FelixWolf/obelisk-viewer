#include "ObjectProperties.hpp"

#include "viewer/protocol/Messages.hpp"

namespace viewer
{

namespace
{

// Properties are asked for again after this long (scripts change them).
constexpr auto kKeepFor = std::chrono::seconds(60);
// No answer: ask again after this long.
constexpr auto kRetryAfter = std::chrono::seconds(5);

} // namespace

const ObjectInfo* ObjectProperties::get(session::Session& session, uint64_t region,
                                        uint32_t localId, const Uuid& id)
{
    const auto now = std::chrono::steady_clock::now();
    Entry& e = entries_[id];
    const bool stale = e.ready && now - e.receivedAt > kKeepFor;
    const bool unanswered = !e.ready && now - e.askedAt > kRetryAfter;
    if((stale || unanswered) && now - e.askedAt > kRetryAfter)
    {
        e.region = region;
        e.localId = localId;
        e.askedAt = now;
        const uint32_t ids[] = {localId};
        session.selectObjects(region, ids);
    }
    return e.ready ? &e.info : nullptr;
}

void ObjectProperties::handle(session::Session& session, uint64_t region,
                              const protocol::CircuitEvent& event)
{
    const auto m = event.decode<protocol::msg::ObjectProperties>();
    if(!m)
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    std::vector<uint32_t> deselect;
    for(const auto& b : m->ObjectData)
    {
        const auto it = entries_.find(b.ObjectID);
        if(it == entries_.end())
        {
            continue; // not ours to keep (selected some other way)
        }
        Entry& e = it->second;
        e.info.name = protocol::toString(b.Name);
        e.info.description = protocol::toString(b.Description);
        e.info.touchName = protocol::toString(b.TouchName);
        e.info.sitName = protocol::toString(b.SitName);
        e.ready = true;
        e.receivedAt = now;
        if(e.region == region)
        {
            deselect.push_back(e.localId);
        }
    }
    session.deselectObjects(region, deselect);
}

} // namespace viewer
