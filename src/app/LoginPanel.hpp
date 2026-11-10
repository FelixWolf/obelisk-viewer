#pragma once

// The login window. It doesn't own the Session; it shows the current one (if
// any) and tells the App what the user asked for. The plain-text password
// lives only in the input buffer until "Log in" is pressed, and is then
// replaced by its hash.

#include "viewer/session/Credentials.hpp"
#include "viewer/session/Login.hpp"
#include "viewer/session/Session.hpp"

#include <string>

namespace viewer
{

struct LoginAction
{
    enum class Kind : uint8_t
    {
        None,
        LogIn,  // replace the current session with a new one
        LogOut, // destroy the current session
    };
    Kind kind = Kind::None;
    session::LoginParameters parameters{};
};

class LoginPanel
{
public:
    LoginPanel(session::MachineInfo machine, std::string channel, std::string version,
               std::string loginUri);
    ~LoginPanel();

    LoginAction draw(const session::Session* session);

    // Logs in with details from a credentials file (--login-file), filling
    // in the form so retries (TOS, MFA) work as if they had been typed.
    LoginAction logInWith(const session::Credentials& credentials);

private:
    LoginAction drawForm(const session::Session* session);
    void selectGrid(const std::string& loginUri);
    LoginAction makeLogIn(session::AgentName name, std::string passwordHash, std::string loginUri);
    void drawSession(const session::Session& session, LoginAction& action);
    void wipePassword();

    session::MachineInfo machine_;
    std::string channel_;
    std::string version_;

    std::string username_;
    std::string password_;
    int grid_ = 0; // index into the grid list; the last entry is "custom"
    std::string customLoginUri_;
    int start_ = 0; // last, home, region
    std::string startRegion_;
    std::string mfaToken_;
    bool accepted_ = false; // the user accepted the TOS / critical message
    std::string formError_;

    // What the last attempt sent, so a retry after an MFA or TOS prompt
    // doesn't need the password again.
    std::optional<session::LoginParameters> lastAttempt_;
};

} // namespace viewer
