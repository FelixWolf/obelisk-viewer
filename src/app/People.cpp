#include "People.hpp"

#include "viewer/protocol/Messages.hpp"

#include <algorithm>

namespace viewer
{

void Friends::load(const llsd::Value& buddyList)
{
    friends_.clear();
    for(size_t i = 0; i < buddyList.size(); i++)
    {
        const llsd::Value& b = buddyList[i];
        const Uuid id = b["buddy_id"].asUuid();
        if(id.isNull() || find(id))
        {
            continue;
        }
        friends_.push_back(
            {id, b["buddy_rights_given"].asInteger(), b["buddy_rights_has"].asInteger(), false});
    }
    version_++;
}

Friend* Friends::findMutable(const Uuid& id)
{
    const auto it = std::ranges::find(friends_, id, &Friend::id);
    return it == friends_.end() ? nullptr : &*it;
}

const Friend* Friends::find(const Uuid& id) const
{
    const auto it = std::ranges::find(friends_, id, &Friend::id);
    return it == friends_.end() ? nullptr : &*it;
}

void Friends::setOnline(std::span<const Uuid> ids, bool online)
{
    for(const Uuid& id : ids)
    {
        if(Friend* f = findMutable(id); f && f->online != online)
        {
            f->online = online;
            version_++;
        }
    }
}

void Friends::rightsChanged(const protocol::msg::ChangeUserRights& m, const Uuid& self)
{
    const bool ours = m.AgentData.AgentID == self;
    for(const auto& r : m.Rights)
    {
        // Ours: the friend is AgentRelated; theirs: the sender is.
        Friend* f = findMutable(ours ? r.AgentRelated : m.AgentData.AgentID);
        if(!f)
        {
            continue;
        }
        int32_t& rights = ours ? f->rightsGiven : f->rightsHas;
        if(rights != r.RelatedRights)
        {
            rights = r.RelatedRights;
            version_++;
        }
    }
}

void Friends::setRightsGiven(const Uuid& id, int32_t rights)
{
    if(Friend* f = findMutable(id); f && f->rightsGiven != rights)
    {
        f->rightsGiven = rights;
        version_++;
    }
}

void Friends::form(const Uuid& id)
{
    if(id.isNull() || find(id))
    {
        return;
    }
    // LLAvatarTracker::formFriendship: both see the other online. Whether
    // they are, the region tells us next.
    friends_.push_back({id, FriendRights::Online, FriendRights::Online, false});
    version_++;
}

void Friends::remove(const Uuid& id)
{
    if(std::erase_if(friends_, [&](const Friend& f) { return f.id == id; }) > 0)
    {
        version_++;
    }
}

void Friends::clear()
{
    friends_.clear();
    version_++;
}

void Nearby::update(uint64_t regionHandle, const protocol::msg::CoarseLocationUpdate& m,
                    const Uuid& self)
{
    std::vector<Person> people;
    // The ids and the locations go together, one for one (older regions
    // sent no ids: nothing to show then).
    const size_t n = std::min(m.Location.size(), m.AgentData.size());
    for(size_t i = 0; i < n; i++)
    {
        const Uuid& id = m.AgentData[i].AgentID;
        if(id.isNull() || id == self)
        {
            continue;
        }
        const auto& l = m.Location[i];
        Person p;
        p.id = id;
        p.regionHandle = regionHandle;
        // Z in units of 4 m; 255: higher than it can say.
        p.heightKnown = l.Z != 255;
        p.position = glm::vec3(l.X, l.Y, p.heightKnown ? l.Z * 4.0f : 0.0f);
        people.push_back(p);
    }
    std::vector<Person>& kept = regions_[regionHandle];
    const auto same = [](const Person& a, const Person& b)
    { return a.id == b.id && a.position == b.position && a.heightKnown == b.heightKnown; };
    if(!std::ranges::equal(kept, people, same))
    {
        kept = std::move(people);
        version_++;
    }
}

void Nearby::removeRegion(uint64_t regionHandle)
{
    if(regions_.erase(regionHandle) > 0)
    {
        version_++;
    }
}

std::vector<Nearby::Person> Nearby::all() const
{
    std::vector<Person> out;
    for(const auto& [handle, people] : regions_)
    {
        out.insert(out.end(), people.begin(), people.end());
    }
    return out;
}

void Nearby::clear()
{
    regions_.clear();
    version_++;
}

glm::dvec3 globalPosition(uint64_t regionHandle, const glm::vec3& local)
{
    return glm::dvec3(static_cast<double>(regionHandle >> 32),
                      static_cast<double>(regionHandle & 0xFFFFFFFFu), 0.0) +
           glm::dvec3(local);
}

} // namespace viewer
