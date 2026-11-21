#pragma once

// What scripts ask of us, waiting for an answer: dialogs (llDialog: a
// message and up to 12 buttons), text boxes (llTextBox), permission
// requests (llRequestPermissions, llRequestExperiencePermissions) and web
// pages offered (llLoadURL). Only the queue and the answers; the page that
// shows them is ScriptDialogPage's, the messages sent the Session's.
//
// As the official viewer (llviewermessage.cpp process_script_dialog,
// process_script_question, process_load_url): a dialog's reply goes to the
// region that sent it, with the button's index and label (a text box's:
// index 0 and the text); "Ignore" sends nothing. A permission request is
// answered with the permissions granted, none if denied; unknown ones are
// never granted, and one asking only those isn't shown. Requests from one
// owner's scripts are throttled (more than 5 in 10 seconds are dropped;
// our own objects counted each on its own). A newer dialog from the same
// object replaces the one waiting.

#include "viewer/core/Uuid.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace viewer
{

namespace protocol::msg
{
struct ScriptDialog;
struct ScriptQuestion;
struct LoadURL;
} // namespace protocol::msg

struct ScriptNotice
{
    enum class Kind : uint8_t
    {
        Dialog,
        TextBox,
        Permissions,
        LoadUrl,
    };
    Kind kind = Kind::Dialog;
    uint64_t serial = 0; // unique while the viewer runs
    uint64_t region = 0; // the region that sent it (answers go there)
    Uuid object;         // the object (a permission request's task)
    std::string objectName;
    std::string owner; // the owner's name (a group's for group owned)
    Uuid ownerId;      // if known (web pages: no name, just this)
    bool groupOwned = false;
    std::string message;
    // Dialogs: the channel replies are said on, and the buttons' labels
    // in the script's order.
    int32_t channel = 0;
    std::vector<std::string> buttons;
    // Permission requests: the script, what it asks for (known bits), and
    // in words; whether any of them calls for care (taking money).
    Uuid item;
    int32_t questions = 0;
    std::vector<std::string> permissions;
    bool caution = false;
    Uuid experience; // llRequestExperiencePermissions: the experience
    // Web pages: where.
    std::string url;
};

// A script's permission, as llRequestPermissions' bits.
struct ScriptPermission
{
    int32_t bit = 0;
    std::string_view description; // what it lets the script do
    bool caution = false;
};
const std::vector<ScriptPermission>& scriptPermissions();

// Dialog buttons in rows of three, as llDialog lays them out: the first
// three along the bottom, the next above, and so on (the top row may be
// short). Indices into the buttons, rows from the top.
std::vector<std::vector<size_t>> dialogRows(size_t buttons);

class ScriptDialogs
{
public:
    static constexpr size_t kMaxButtons = 12;
    static constexpr size_t kMaxWaiting = 32; // the oldest go beyond this
    // llTextBox: a dialog with this one button is a text box.
    static constexpr std::string_view kTextBoxButton = "!!llTextBox!!";

    // From the region's messages. `self` is our full name ("First Last",
    // no " Resident"), for throttling our own objects each on its own.
    // `now` in seconds.
    void dialog(uint64_t region, const protocol::msg::ScriptDialog& m);
    void question(uint64_t region, const protocol::msg::ScriptQuestion& m, std::string_view self,
                  double now);
    void loadUrl(uint64_t region, const protocol::msg::LoadURL& m);

    // The one shown (the oldest waiting), and how many more wait.
    const ScriptNotice* front() const { return waiting_.empty() ? nullptr : &waiting_.front(); }
    size_t waiting() const { return waiting_.size(); }
    // Changes whenever what's waiting does.
    uint64_t version() const { return version_; }

    // Answers to the one shown, which goes.
    struct DialogReply
    {
        uint64_t region = 0;
        Uuid object;
        int32_t channel = 0;
        int32_t index = 0;
        std::string label;
    };
    struct PermissionReply
    {
        uint64_t region = 0;
        Uuid task;
        Uuid item;
        int32_t questions = 0; // granted (0: none)
    };
    // A dialog's button (its index in the script's order).
    std::optional<DialogReply> choose(size_t button);
    // A text box's text (cut to what the message holds, whole characters).
    std::optional<DialogReply> reply(std::string_view text);
    std::optional<PermissionReply> answer(bool allow);
    // A web page offered: its address, to open.
    std::optional<std::string> open();
    // Gone without an answer (Ignore; a page declined). A permission
    // request ignored is denied (the script hears no, as when the official
    // viewer's notice is closed).
    std::optional<PermissionReply> dismiss();

    // Logged out: nothing waits.
    void clear();

private:
    void add(ScriptNotice notice);

    std::deque<ScriptNotice> waiting_;
    uint64_t serial_ = 0;
    uint64_t version_ = 0;
    // Recent permission requests by owner (or our object's id): when.
    std::map<std::string, std::vector<double>> asked_;
};

} // namespace viewer
