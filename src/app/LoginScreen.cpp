#include "LoginScreen.hpp"

#include "viewer/core/Log.hpp"

#include <RmlUi/Core.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <utility>

namespace viewer
{

namespace
{

// An event's first argument as a flag (none: true): Enter in a field says
// whether it was Enter (a "change" event's linebreak), a button says
// nothing.
bool firstFlag(const Rml::VariantList& arguments)
{
    return arguments.empty() || arguments[0].Get<bool>();
}

int firstIndex(const Rml::VariantList& arguments)
{
    return arguments.empty() ? -1 : arguments[0].Get<int>(-1);
}

} // namespace

template <typename T>
void LoginScreen::put(const char* variable, T& field, T value)
{
    if(field != value)
    {
        field = std::move(value);
        model_.DirtyVariable(variable);
    }
}

std::unique_ptr<LoginScreen> LoginScreen::create(gui::Gui& gui, LoginFlow& flow,
                                                 const std::string& loginUri)
{
    std::unique_ptr<LoginScreen> screen(new LoginScreen);
    LoginScreen* s = screen.get();
    s->flow_ = &flow;
    s->surface_ = gui.createSurface("login", kSize);
    if(!s->surface_)
    {
        return nullptr;
    }
    for(const KnownGrid& g : kKnownGrids)
    {
        s->gridNames_.emplace_back(g.label);
    }
    if(!loginUri.empty())
    {
        s->selectGrid(loginUri);
    }

    Rml::DataModelConstructor model = s->surface_->context()->CreateDataModel("login");
    if(!model)
    {
        log::error("the login screen's data model couldn't be made");
        return nullptr;
    }
    model.Bind("appName", &s->appName_);
    model.RegisterArray<std::vector<Rml::String>>();
    if(auto account = model.RegisterStruct<Account>())
    {
        account.RegisterMember("name", &Account::name);
        account.RegisterMember("grid", &Account::grid);
        account.RegisterMember("image", &Account::image);
        account.RegisterMember("initial", &Account::initial);
    }
    model.RegisterArray<std::vector<Account>>();
    model.Bind("view", &s->view_);
    model.Bind("accounts", &s->accounts_);
    model.Bind("busy", &s->busy_);
    model.Bind("busyText", &s->busyText_);
    model.Bind("prompt", &s->prompt_);
    model.Bind("promptText", &s->promptText_);
    model.Bind("promptReason", &s->promptReason_);
    model.Bind("code", &s->code_);
    model.Bind("accepted", &s->accepted_);
    model.Bind("username", &s->username_);
    model.Bind("password", &s->password_);
    model.Bind("grid", &s->grid_);
    model.Bind("grids", &s->gridNames_);
    model.Bind("customUri", &s->customUri_);
    model.Bind("start", &s->start_);
    model.Bind("region", &s->region_);
    model.Bind("remember", &s->remember_);
    model.Bind("formError", &s->formError_);

    // A remembered account: logged in with its saved secrets.
    model.BindEventCallback("logInAccount",
                            [s](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
                            {
                                const auto& accounts = s->flow_->saved().accounts();
                                const int i = firstIndex(a);
                                if(i >= 0 && i < static_cast<int>(accounts.size()))
                                {
                                    const SavedAccount account = accounts[static_cast<size_t>(i)];
                                    s->username_ = account.name.first + " " + account.name.last;
                                    s->selectGrid(account.loginUri);
                                    s->action_ = s->flow_->logInSaved(account, s->start());
                                }
                            });
    // Forgotten: its secrets out of the credential store, it off the list.
    // (Inside the account's card, whose click logs in: not passed on.)
    model.BindEventCallback("forget",
                            [s](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList& a)
                            {
                                event.StopPropagation();
                                const auto& accounts = s->flow_->saved().accounts();
                                const int i = firstIndex(a);
                                if(i >= 0 && i < static_cast<int>(accounts.size()))
                                {
                                    const SavedAccount account = accounts[static_cast<size_t>(i)];
                                    s->flow_->saved().forget(account);
                                }
                            });
    model.BindEventCallback("submit",
                            [s](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
                            {
                                if(firstFlag(a))
                                {
                                    s->submitForm();
                                }
                            });
    model.BindEventCallback("answerCode",
                            [s](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
                            {
                                if(firstFlag(a) && !s->code_.empty())
                                {
                                    s->action_ = s->flow_->answerCode(s->code_);
                                    s->code_.clear();
                                    s->model_.DirtyVariable("code");
                                }
                            });
    model.BindEventCallback("accept",
                            [s](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                if(s->accepted_)
                                {
                                    s->action_ = s->flow_->accept();
                                    s->accepted_ = false;
                                    s->model_.DirtyVariable("accepted");
                                }
                            });
    model.BindEventCallback("dismiss", [s](Rml::DataModelHandle, Rml::Event&,
                                           const Rml::VariantList&) { s->flow_->dismiss(); });
    model.BindEventCallback("cancel",
                            [s](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            { s->action_.kind = LoginAction::Kind::LogOut; });
    s->model_ = model.GetModelHandle();

    if(!s->surface_->load("login.rml"))
    {
        log::error("the login screen's document couldn't be read");
        return nullptr;
    }
    return screen;
}

LoginScreen::~LoginScreen()
{
    wipeSecrets();
}

bool LoginScreen::wanted(const session::Session* session)
{
    using session::SessionState;
    if(!session)
    {
        return true;
    }
    const SessionState state = session->state();
    return state == SessionState::LoggingIn || state == SessionState::Connecting ||
           state == SessionState::Failed;
}

void LoginScreen::selectGrid(const std::string& loginUri)
{
    const auto known = std::ranges::find(kKnownGrids, std::string_view(loginUri), &KnownGrid::uri);
    if(known != kKnownGrids.end() && !known->uri.empty())
    {
        grid_ = static_cast<int>(known - kKnownGrids.begin());
    }
    else
    {
        grid_ = static_cast<int>(kKnownGrids.size()) - 1;
        customUri_ = loginUri;
    }
    if(model_)
    {
        model_.DirtyVariable("grid");
        model_.DirtyVariable("customUri");
    }
}

void LoginScreen::setStart(const std::string& where)
{
    if(where.empty() || where == "last")
    {
        start_ = 0;
    }
    else if(where == "home")
    {
        start_ = 1;
    }
    else
    {
        start_ = 2;
        region_ = where;
    }
    model_.DirtyVariable("start");
    model_.DirtyVariable("region");
}

std::string LoginScreen::start() const
{
    return start_ == 0 ? "last" : start_ == 1 ? "home" : session::startLocation(region_);
}

LoginAction LoginScreen::logInWith(const session::Credentials& credentials)
{
    username_ = credentials.name.first + " " + credentials.name.last;
    model_.DirtyVariable("username");
    selectGrid(credentials.loginUri);
    // A file's account isn't remembered: it's in the file.
    return flow_->logIn(
        {credentials.name, credentials.passwordHash, credentials.loginUri, start(), false});
}

void LoginScreen::submitForm()
{
    auto name = session::parseAgentName(username_);
    const bool custom = kKnownGrids[static_cast<size_t>(grid_)].uri.empty();
    const std::string loginUri =
        custom ? customUri_ : std::string(kKnownGrids[static_cast<size_t>(grid_)].uri);
    std::string error;
    if(!name)
    {
        error = name.error();
    }
    else if(password_.empty())
    {
        error = "Enter a password.";
    }
    else if(loginUri.empty())
    {
        error = "Enter a login URI.";
    }
    else if(start_ == 2 && region_.empty())
    {
        error = "Enter a region name.";
    }
    else
    {
        action_ = flow_->logIn(
            {std::move(*name), session::hashPassword(password_), loginUri, start(), remember_});
        wipeSecrets();
    }
    put("formError", formError_, Rml::String(error));
}

void LoginScreen::wipeSecrets()
{
    std::ranges::fill(password_, '\0');
    password_.clear();
    if(model_)
    {
        model_.DirtyVariable("password");
    }
}

LoginAction LoginScreen::update(const session::Session* session)
{
    using session::SessionState;
    // The accounts, most recently used first.
    std::vector<Account> accounts;
    for(const SavedAccount& a : flow_->saved().accounts())
    {
        Account account;
        // Residents since 2010 have only a username: "Resident" isn't shown.
        account.name = a.name.last == "Resident" || a.name.last == "resident"
                           ? a.name.first
                           : a.name.first + " " + a.name.last;
        const auto grid =
            std::ranges::find(kKnownGrids, std::string_view(a.loginUri), &KnownGrid::uri);
        account.grid = a.loginUri == session::kAgniLoginUri ? ""
                       : grid != kKnownGrids.end()          ? grid->label
                                                            : a.loginUri;
        account.image =
            a.image.isNull() ? "" : std::format("asset://{}?size=256", a.image.toString());
        account.initial = a.name.first.empty()
                              ? "?"
                              : Rml::String(1, static_cast<char>(std::toupper(
                                                   static_cast<unsigned char>(a.name.first[0]))));
        accounts.push_back(std::move(account));
    }
    put("accounts", accounts_, std::move(accounts));
    // No accounts: the form is all there is.
    if(accounts_.empty() && view_ != "form")
    {
        put("view", view_, Rml::String("form"));
    }

    // A login on its way.
    const bool busy = session && (session->state() == SessionState::LoggingIn ||
                                  session->state() == SessionState::Connecting);
    put("busy", busy_, busy);
    if(busy)
    {
        const session::LoginSuccess* login = session->login();
        put("busyText", busyText_,
            Rml::String(login ? std::format("Arriving as {}...", login->name.first)
                              : std::string("Logging in...")));
    }

    // What the grid asked back.
    using Prompt = LoginFlow::Prompt;
    const Prompt p = flow_->prompt();
    put("prompt", prompt_,
        Rml::String(p == Prompt::Code       ? "code"
                    : p == Prompt::Terms    ? "terms"
                    : p == Prompt::Critical ? "critical"
                    : p == Prompt::Error    ? "error"
                                            : ""));
    put("promptText", promptText_, Rml::String(p == Prompt::None ? "" : flow_->message()));
    put("promptReason", promptReason_, Rml::String(p == Prompt::Error ? flow_->reason() : ""));

    surface_->update();
    return std::exchange(action_, LoginAction{});
}

} // namespace viewer
