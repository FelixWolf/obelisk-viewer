#pragma once

// Logging in, apart from any UI: starts logins (typed, a saved account, a
// credentials file), holds what the grid asked back (an authenticator code
// for two-factor accounts, the terms of service, a critical message, or why
// it failed) until it's answered, retrying with the answer, and remembers
// accounts (SavedLogins) once they've logged in, with the MFA hash the grid
// returns, so the code isn't asked for again for a while. A UI (the ImGui
// login panel now; our own later) only shows it and passes answers on.

#include "SavedLogins.hpp"

#include "viewer/session/Login.hpp"
#include "viewer/session/Session.hpp"

#include <optional>
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

class LoginFlow
{
public:
    LoginFlow(SavedLogins& saved, session::MachineInfo machine, std::string channel,
              std::string version);

    struct Request
    {
        session::AgentName name;
        std::string passwordHash; // hashPassword(); never the plain password
        std::string loginUri;
        std::string start = "last"; // as LoginParameters::start
        bool remember = false;      // save the account once it's logged in
    };
    LoginAction logIn(Request request);
    // A remembered account, with its saved secrets.
    LoginAction logInSaved(const SavedAccount& account, std::string start);

    // What the grid asked back, awaiting an answer.
    enum class Prompt : uint8_t
    {
        None,
        Code,     // two-factor: the authenticator's code
        Terms,    // the terms of service, to agree to
        Critical, // a message to read
        Error,    // the login failed (message says why)
    };
    Prompt prompt() const { return prompt_; }
    const std::string& message() const { return message_; }
    const std::string& reason() const { return reason_; }
    LoginAction answerCode(std::string code);
    LoginAction accept(); // the terms or message
    void dismiss();

    // Each frame, with the current session (if any): its failure becomes a
    // prompt; once it's logged in, the account is saved if asked.
    void observe(const session::Session* session);
    // The same from what a session (`id`: which) shows (tests).
    void observe(const void* id, const session::LoginFailure* failure,
                 const session::LoginSuccess* success);

    SavedLogins& saved() { return saved_; }

private:
    LoginAction retry();

    SavedLogins& saved_;
    session::MachineInfo machine_;
    std::string channel_;
    std::string version_;

    std::optional<session::LoginParameters> attempt_; // the last, for retries
    bool remember_ = false;
    bool savedAttempt_ = false; // saved after this attempt's success
    const void* observed_ = nullptr;
    bool failureSeen_ = false;
    Prompt prompt_ = Prompt::None;
    std::string message_;
    std::string reason_;
};

} // namespace viewer
