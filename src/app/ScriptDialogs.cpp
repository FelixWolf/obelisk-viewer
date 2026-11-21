#include "ScriptDialogs.hpp"

#include "viewer/core/Log.hpp"
#include "viewer/protocol/Messages.hpp"

#include <algorithm>

namespace viewer
{

namespace
{

// Requests from one owner's scripts: at most this many this close together.
constexpr size_t kThrottleCount = 5;
constexpr double kThrottleSeconds = 10.0;

// What a Variable 1 field holds (its length byte counts the terminator).
constexpr size_t kMaxLabel = 254;

// "First Resident" -> "First" (how names are shown now).
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

} // namespace

const std::vector<ScriptPermission>& scriptPermissions()
{
    // llRequestPermissions' PERMISSION_* bits.
    static const std::vector<ScriptPermission> kPermissions = {
        {1 << 1, "Take Linden dollars (L$) from you", true},
        {1 << 2, "Act on your movement controls", false},
        {1 << 3, "Remap your movement controls", false},
        {1 << 4, "Animate your avatar", false},
        {1 << 5, "Attach to your avatar", false},
        {1 << 6, "Give up its ownership and become public", false},
        {1 << 7, "Link to and unlink from other objects", false},
        {1 << 8, "Add and remove joints with other objects", false},
        {1 << 9, "Change its permissions", false},
        {1 << 10, "Follow where your camera is", false},
        {1 << 11, "Move your camera", false},
        {1 << 12, "Teleport you", false},
        {1 << 13, "Take you into an experience", false},
        {1 << 14, "Manage estate access without telling anyone", false},
        {1 << 15, "Replace your avatar's standard animations", false},
        {1 << 16, "Return objects on your behalf", false},
        {1 << 17, "Make your avatar sit", false},
        {1 << 18, "Change your environment (sky and water)", false},
        {1 << 19, "Use privileged access to land", true},
    };
    return kPermissions;
}

std::vector<std::vector<size_t>> dialogRows(size_t buttons)
{
    std::vector<std::vector<size_t>> rows;
    for(size_t first = 0; first < buttons; first += 3)
    {
        std::vector<size_t> row;
        for(size_t i = first; i < std::min(first + 3, buttons); i++)
        {
            row.push_back(i);
        }
        rows.insert(rows.begin(), std::move(row)); // later rows go above
    }
    return rows;
}

void ScriptDialogs::add(ScriptNotice notice)
{
    notice.serial = ++serial_;
    waiting_.push_back(std::move(notice));
    while(waiting_.size() > kMaxWaiting)
    {
        waiting_.pop_front();
    }
    version_++;
}

void ScriptDialogs::dialog(uint64_t region, const protocol::msg::ScriptDialog& m)
{
    ScriptNotice n;
    n.region = region;
    n.object = m.Data.ObjectID;
    n.objectName = protocol::toString(m.Data.ObjectName);
    const std::string first = protocol::toString(m.Data.FirstName);
    const std::string last = protocol::toString(m.Data.LastName);
    // A group's object: no first name, the group's in the last.
    n.groupOwned = first.empty();
    n.owner = n.groupOwned ? last : cleanName(last.empty() ? first : first + " " + last);
    if(!m.OwnerData.empty())
    {
        n.ownerId = m.OwnerData[0].OwnerID;
    }
    n.message = protocol::toString(m.Data.Message);
    n.channel = m.Data.ChatChannel;
    for(const auto& b : m.Buttons)
    {
        if(n.buttons.size() == kMaxButtons)
        {
            log::warn("script dialog from '{}': more than {} buttons, the rest left out",
                      n.objectName, kMaxButtons);
            break;
        }
        n.buttons.push_back(protocol::toString(b.ButtonLabel));
    }
    n.kind = n.buttons.size() == 1 && n.buttons[0] == kTextBoxButton ? ScriptNotice::Kind::TextBox
                                                                     : ScriptNotice::Kind::Dialog;
    // A newer dialog from the same object replaces the one waiting (it's
    // usually the next page of the same menu).
    std::erase_if(waiting_,
                  [&](const ScriptNotice& w)
                  {
                      return (w.kind == ScriptNotice::Kind::Dialog ||
                              w.kind == ScriptNotice::Kind::TextBox) &&
                             w.object == n.object;
                  });
    add(std::move(n));
}

void ScriptDialogs::question(uint64_t region, const protocol::msg::ScriptQuestion& m,
                             std::string_view self, double now)
{
    const std::string owner = cleanName(protocol::toString(m.Data.ObjectOwner));
    // Throttled by owner; our own objects each on their own (resetting many
    // scripts at once asks again for each).
    const std::string key = !self.empty() && owner == self ? m.Data.TaskID.toString() : owner;
    std::vector<double>& times = asked_[key];
    std::erase_if(times, [&](double t) { return now - t > kThrottleSeconds; });
    if(times.size() >= kThrottleCount)
    {
        log::info("permission request from '{}' ({}) dropped: too many", owner,
                  protocol::toString(m.Data.ObjectName));
        return;
    }
    times.push_back(now);
    std::erase_if(asked_, [](const auto& entry) { return entry.second.empty(); });

    ScriptNotice n;
    n.kind = ScriptNotice::Kind::Permissions;
    n.region = region;
    n.object = m.Data.TaskID;
    n.item = m.Data.ItemID;
    n.objectName = protocol::toString(m.Data.ObjectName);
    n.owner = owner;
    n.experience = m.Experience.ExperienceID;
    for(const ScriptPermission& p : scriptPermissions())
    {
        if(m.Data.Questions & p.bit)
        {
            n.questions |= p.bit;
            n.permissions.emplace_back(p.description);
            n.caution = n.caution || p.caution;
        }
    }
    if(n.questions != m.Data.Questions)
    {
        log::warn("'{}' asks for permissions not known ({:#x}): never granted", n.objectName,
                  m.Data.Questions & ~n.questions);
    }
    if(n.questions == 0)
    {
        return; // nothing we know to ask
    }
    add(std::move(n));
}

void ScriptDialogs::loadUrl(uint64_t region, const protocol::msg::LoadURL& m)
{
    ScriptNotice n;
    n.kind = ScriptNotice::Kind::LoadUrl;
    n.region = region;
    n.object = m.Data.ObjectID;
    n.objectName = protocol::toString(m.Data.ObjectName);
    n.ownerId = m.Data.OwnerID;
    n.groupOwned = m.Data.OwnerIsGroup;
    n.message = protocol::toString(m.Data.Message);
    n.url = protocol::toString(m.Data.URL);
    // Only web pages (the simulator checks too).
    if(!n.url.starts_with("http://") && !n.url.starts_with("https://"))
    {
        log::warn("'{}' offered a page that isn't on the web: {}", n.objectName, n.url);
        return;
    }
    add(std::move(n));
}

std::optional<ScriptDialogs::DialogReply> ScriptDialogs::choose(size_t button)
{
    const ScriptNotice* n = front();
    if(!n || n->kind != ScriptNotice::Kind::Dialog || button >= n->buttons.size())
    {
        return std::nullopt;
    }
    DialogReply r{n->region, n->object, n->channel, static_cast<int32_t>(button),
                  n->buttons[button]};
    waiting_.pop_front();
    version_++;
    return r;
}

std::optional<ScriptDialogs::DialogReply> ScriptDialogs::reply(std::string_view text)
{
    const ScriptNotice* n = front();
    if(!n || n->kind != ScriptNotice::Kind::TextBox)
    {
        return std::nullopt;
    }
    DialogReply r{n->region, n->object, n->channel, 0, cut(text, kMaxLabel)};
    waiting_.pop_front();
    version_++;
    return r;
}

std::optional<ScriptDialogs::PermissionReply> ScriptDialogs::answer(bool allow)
{
    const ScriptNotice* n = front();
    if(!n || n->kind != ScriptNotice::Kind::Permissions)
    {
        return std::nullopt;
    }
    PermissionReply r{n->region, n->object, n->item, allow ? n->questions : 0};
    waiting_.pop_front();
    version_++;
    return r;
}

std::optional<std::string> ScriptDialogs::open()
{
    const ScriptNotice* n = front();
    if(!n || n->kind != ScriptNotice::Kind::LoadUrl)
    {
        return std::nullopt;
    }
    std::string url = n->url;
    waiting_.pop_front();
    version_++;
    return url;
}

std::optional<ScriptDialogs::PermissionReply> ScriptDialogs::dismiss()
{
    if(waiting_.empty())
    {
        return std::nullopt;
    }
    if(waiting_.front().kind == ScriptNotice::Kind::Permissions)
    {
        return answer(false);
    }
    waiting_.pop_front();
    version_++;
    return std::nullopt;
}

void ScriptDialogs::clear()
{
    waiting_.clear();
    asked_.clear();
    version_++;
}

} // namespace viewer
