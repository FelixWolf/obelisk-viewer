#pragma once

// The login screen (RmlUi, assets/ui/login.rml): a view of the LoginFlow,
// which does the logging in, as the ImGui LoginPanel was. In the middle,
// the remembered accounts (their profile pictures, from the disk cache:
// AccountPicture) to log in with a click, and Add account: the form
// (username, password, grid, where to start, remember). Over them, what
// the grid asked back (an authenticator code, the terms of service, a
// critical message) or why a login failed, and "Logging in..." while one
// is on its way. It doesn't own the Session: it tells the App what was
// asked for (LoginAction). The typed password is kept only until "Log in"
// takes its hash.

#include "LoginFlow.hpp"
#include "Updater.hpp"

#include "viewer/core/Branding.hpp"
#include "viewer/gui/Gui.hpp"
#include "viewer/session/Credentials.hpp"
#include "viewer/session/Session.hpp"

#include <RmlUi/Core/DataModelHandle.h>

#include <glm/glm.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace viewer
{

class LoginScreen
{
public:
    // The page's size in pixels (fitted to the window, as the main menu).
    static constexpr glm::ivec2 kSize{720, 900};

    // Null if the interface can't be set up (the ImGui panel stays then).
    static std::unique_ptr<LoginScreen> create(gui::Gui& gui, LoginFlow& flow,
                                               const std::string& loginUri);
    ~LoginScreen();
    LoginScreen(const LoginScreen&) = delete;
    LoginScreen& operator=(const LoginScreen&) = delete;

    // Shown while there's no session, it's logging in, or it failed.
    static bool wanted(const session::Session* session);

    // The page's values from the flow and session, then its layout (once
    // a frame while shown, before render()); what the page asked for.
    LoginAction update(const session::Session* session);
    void render(SDL_GPUCommandBuffer* commands) { surface_->render(commands); }
    gui::Surface& surface() { return *surface_; }
    void reload() { surface_->reload(); }

    // Logs in with details from a credentials file (--login-file), filling
    // in the form so retries (terms, codes) work as if they'd been typed.
    LoginAction logInWith(const session::Credentials& credentials);
    // Where the next login starts: "last", "home" or "Region[/x/y/z]".
    void setStart(const std::string& where);

    // The update shown: coming, or ready to restart into.
    void setUpdate(const Updater::Status& status);
    void setUpdateProblem(const std::string& problem);
    // Restart to update was clicked (once).
    bool takeRestart() { return std::exchange(restart_, false); }

    // A remembered account, as the page shows it.
    struct Account
    {
        Rml::String name;
        Rml::String grid;    // empty: Second Life (the usual one)
        Rml::String image;   // asset://... ; empty: none known
        Rml::String initial; // shown without a picture
        bool operator==(const Account&) const = default;
    };

private:
    LoginScreen() = default;
    void selectGrid(const std::string& loginUri);
    std::string start() const;
    void submitForm();
    void wipeSecrets();
    template <typename T>
    void put(const char* variable, T& field, T value);

    LoginFlow* flow_ = nullptr;
    std::unique_ptr<gui::Surface> surface_;
    Rml::DataModelHandle model_;
    Rml::String appName_{branding::kName};
    LoginAction action_; // asked for by the page this frame

    // The page's values (data-model="login").
    Rml::String view_ = "accounts"; // "accounts" or "form"
    std::vector<Account> accounts_;
    bool busy_ = false;
    Rml::String busyText_;
    Rml::String prompt_; // "", "code", "terms", "critical", "error"
    Rml::String promptText_;
    Rml::String promptReason_;
    Rml::String code_;
    bool accepted_ = false;
    Rml::String username_;
    Rml::String password_;
    int grid_ = 0; // kKnownGrids; the last: custom
    Rml::String customUri_;
    int start_ = 0; // last, home, region
    Rml::String region_;
    bool remember_ = true;
    Rml::String formError_;
    std::vector<Rml::String> gridNames_;
    Rml::String updateText_; // empty: no update
    bool updateReady_ = false;
    Rml::String updateProblem_;
    bool restart_ = false;
};

} // namespace viewer
