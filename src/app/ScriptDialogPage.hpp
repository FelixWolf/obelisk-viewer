#pragma once

// The page that shows what scripts ask of us (ScriptDialogs' oldest
// waiting): a dialog's message and buttons (as llDialog lays them out), a
// text box, a permission request (Allow, Deny) or a web page offered (Open,
// Not now); and, when no script asks, what's offered (Offers' oldest: a
// teleport, friendship, inventory, ...: two answers, or one). Over the
// window's top right on the desktop; on a panel beside the view in first
// person and in a headset (VrControls' notice). What's clicked is taken by
// the app (takeAction), which answers.

#include "Offers.hpp"
#include "ScriptDialogs.hpp"

#include "viewer/gui/Gui.hpp"

#include <RmlUi/Core/DataModelHandle.h>

#include <glm/glm.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace viewer
{

class ScriptDialogPage
{
public:
    static constexpr glm::ivec2 kSize{560, 520};
    static constexpr float kMetersPerPixel = 0.0006f; // about 34 cm across

    // Null if the page can't be made.
    static std::unique_ptr<ScriptDialogPage> create(gui::Gui& gui);

    // Names of avatars by id (empty until known), for web pages' owners.
    void setNames(std::function<std::string(const Uuid&)> names) { names_ = std::move(names); }

    // Shows what's waiting (filled again when it changes); once a frame,
    // before render(). `headset`: links open on the desktop (the page says
    // so); `keyboard`: text is typed on the VR keyboard (a button opens
    // it); `pointHint`: first person on the desktop (hold Tab to point).
    void update(const ScriptDialogs& dialogs, const Offers& offers, bool headset, bool keyboard,
                bool pointHint);
    void render(SDL_GPUCommandBuffer* commands) { surface_->render(commands); }
    void reload() { surface_->reload(); }
    gui::Surface& surface() { return *surface_; }

    struct Action
    {
        enum class Kind : uint8_t
        {
            Choose, // a dialog's button (`button`)
            Send,   // a text box's text (`text`)
            Allow,
            Deny,
            Open,     // the web page
            Ignore,   // gone without an answer
            Keyboard, // type the text box's text on the VR keyboard
            Accept,   // an offer's first answer
            Decline,  // its other
        };
        Kind kind = Kind::Ignore;
        bool offer = false;  // about an offer (else a script's notice)
        uint64_t serial = 0; // the notice it's about
        size_t button = 0;
        std::string text;
    };
    // What was clicked (once).
    std::optional<Action> takeAction();

    struct Button
    {
        Rml::String label;
        int index = 0;
    };
    struct Row
    {
        std::vector<Button> buttons;
    };

private:
    explicit ScriptDialogPage(gui::Gui& gui) : gui_(gui) {}
    bool setUp();
    void fill(const ScriptNotice* notice, size_t waiting);
    void fill(const Offer& offer, size_t waiting);
    // The name to show for an avatar, if known.
    std::string nameOf(const Uuid& id) const { return names_ ? names_(id) : std::string(); }
    void act(Action::Kind kind, size_t button = 0);

    gui::Gui& gui_;
    std::unique_ptr<gui::Surface> surface_;
    Rml::DataModelHandle model_;
    std::function<std::string(const Uuid&)> names_;
    std::string shownOwner_; // a web page's owner's name as shown (it may arrive later)
    uint64_t serial_ = 0;    // the notice shown
    bool offer_ = false;     // it's an offer
    std::optional<std::pair<uint64_t, uint64_t>> shownVersions_; // scripts', offers'
    std::optional<Action> action_;

    Rml::String kind_; // dialog, textbox, permissions, url; empty: none
    Rml::String objectName_;
    Rml::String owner_;
    Rml::String message_;
    std::vector<Row> rows_; // a dialog's buttons as laid out: rows from the top
    std::vector<Rml::String> permissions_;
    bool caution_ = false;
    bool experience_ = false;
    Rml::String url_;
    // An offer: what it is (under its title), details, its answers' labels
    // (no decline: one answer).
    Rml::String subtitle_;
    std::vector<Rml::String> details_;
    Rml::String acceptLabel_;
    Rml::String declineLabel_;
    int more_ = 0; // waiting after this one
    Rml::String reply_;
    bool headset_ = false;
    bool keyboard_ = false;
    bool pointHint_ = false;
};

} // namespace viewer
