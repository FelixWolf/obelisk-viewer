#include "LoginFlow.hpp"

#include "viewer/core/Log.hpp"

#include <utility>

namespace viewer
{

LoginFlow::LoginFlow(SavedLogins& saved, session::MachineInfo machine, std::string channel,
                     std::string version)
    : saved_(saved), machine_(std::move(machine)), channel_(std::move(channel)),
      version_(std::move(version))
{
}

LoginAction LoginFlow::logIn(Request request)
{
    LoginAction action;
    action.kind = LoginAction::Kind::LogIn;
    session::LoginParameters& p = action.parameters;
    p.loginUri = std::move(request.loginUri);
    p.name = std::move(request.name);
    p.passwordHash = std::move(request.passwordHash);
    p.start = std::move(request.start);
    p.channel = channel_;
    p.version = version_;
    p.machine = machine_;
    // A remembered account's MFA hash, so its code isn't asked for again.
    if(const auto secret = saved_.secret({p.loginUri, p.name, {}}))
    {
        p.mfaHash = secret->mfaHash;
    }
    attempt_ = p;
    remember_ = request.remember;
    savedAttempt_ = false;
    failureSeen_ = false; // (the new session may reuse the old one's address)
    prompt_ = Prompt::None;
    return action;
}

LoginAction LoginFlow::logInSaved(const SavedAccount& account, std::string start)
{
    const auto secret = saved_.secret(account);
    if(!secret || secret->passwordHash.empty())
    {
        prompt_ = Prompt::Error;
        reason_ = "saved";
        message_ = "The credential store has no password for this account. Log in with the "
                   "password to save it again.";
        return {};
    }
    return logIn({account.name, secret->passwordHash, account.loginUri, std::move(start), true});
}

LoginAction LoginFlow::retry()
{
    LoginAction action;
    if(attempt_)
    {
        action.kind = LoginAction::Kind::LogIn;
        action.parameters = *attempt_;
        savedAttempt_ = false;
        failureSeen_ = false;
    }
    prompt_ = Prompt::None;
    return action;
}

LoginAction LoginFlow::answerCode(std::string code)
{
    if(prompt_ != Prompt::Code || code.empty() || !attempt_)
    {
        return {};
    }
    attempt_->mfaToken = std::move(code);
    LoginAction action = retry();
    attempt_->mfaToken.clear(); // good once
    return action;
}

LoginAction LoginFlow::accept()
{
    if(!attempt_ || (prompt_ != Prompt::Terms && prompt_ != Prompt::Critical))
    {
        return {};
    }
    (prompt_ == Prompt::Terms ? attempt_->agreeToTos : attempt_->readCritical) = true;
    return retry();
}

void LoginFlow::dismiss()
{
    prompt_ = Prompt::None;
}

void LoginFlow::observe(const session::Session* session)
{
    observe(session, session ? session->failure() : nullptr, session ? session->login() : nullptr);
}

void LoginFlow::observe(const void* id, const session::LoginFailure* failure,
                        const session::LoginSuccess* login)
{
    if(id != observed_)
    {
        observed_ = id;
        failureSeen_ = false;
    }
    if(!id)
    {
        return;
    }
    if(failure && !failureSeen_)
    {
        failureSeen_ = true;
        reason_ = failure->reason;
        message_ = failure->message;
        if(reason_ == "mfa_challenge")
        {
            prompt_ = Prompt::Code;
            // A saved MFA hash that's no longer good: forgotten with it.
            if(attempt_)
            {
                attempt_->mfaHash.clear();
            }
        }
        else if(reason_ == "tos")
        {
            prompt_ = Prompt::Terms;
        }
        else if(reason_ == "critical")
        {
            prompt_ = Prompt::Critical;
        }
        else
        {
            prompt_ = Prompt::Error;
        }
    }
    if(login && attempt_ && remember_ && !savedAttempt_)
    {
        savedAttempt_ = true;
        SavedSecret secret{attempt_->passwordHash,
                           login->mfaHash.empty() ? attempt_->mfaHash : login->mfaHash};
        if(saved_.save({attempt_->loginUri, attempt_->name, {}}, secret))
        {
            log::info("login saved to the credential store");
        }
    }
}

} // namespace viewer
