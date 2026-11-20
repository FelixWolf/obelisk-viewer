#pragma once

// The login window (ImGui): a view of the LoginFlow, which does the logging
// in. It doesn't own the Session; it shows the current one (if any) and
// tells the App what the user asked for. The plain-text password lives only
// in the input buffer until "Log in" is pressed, and is then replaced by
// its hash.

#include "LoginFlow.hpp"

#include "viewer/session/Credentials.hpp"
#include "viewer/session/Login.hpp"
#include "viewer/session/Session.hpp"

#include <string>

namespace viewer
{

class LoginPanel
{
public:
    LoginPanel(LoginFlow& flow, std::string loginUri);
    ~LoginPanel();

    LoginAction draw(const session::Session* session);
    // Only the session's details (regions, circuits, traffic; Log out), in
    // a window of their own: for developers, once the login screen is
    // RmlUi's.
    LoginAction drawSessionWindow(const session::Session* session);

    // Logs in with details from a credentials file (--login-file), filling
    // in the form so retries (TOS, MFA) work as if they had been typed.
    LoginAction logInWith(const session::Credentials& credentials);

    // Start location for the next login: "last", "home" or "Region[/x/y/z]".
    void setStart(const std::string& where);

private:
    LoginAction drawForm(const session::Session* session);
    void selectGrid(const std::string& loginUri);
    std::string start() const;
    void drawPrompt(LoginAction& action);
    void drawSaved(LoginAction& action);
    void drawSession(const session::Session& session, LoginAction& action);
    void wipePassword();

    LoginFlow& flow_;

    std::string username_;
    std::string password_;
    int grid_ = 0; // index into the grid list; the last entry is "custom"
    std::string customLoginUri_;
    int start_ = 0; // last, home, region
    std::string startRegion_;
    std::string mfaToken_;
    bool accepted_ = false; // the user accepted the TOS / critical message
    bool remember_ = true;  // save the account once logged in
    std::string formError_;
};

} // namespace viewer
