#include "LoginPanel.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <format>

namespace viewer
{

namespace
{

struct Grid
{
    const char* label;
    std::string_view uri;
};

constexpr std::array kGrids = {
    Grid{"Second Life", session::kAgniLoginUri},
    Grid{"Second Life Beta (Aditi)", session::kAditiLoginUri},
    Grid{"Custom", {}},
};

constexpr std::array kStartLocations = {"Last location", "Home", "Region"};

const ImVec4 kErrorColor(1.0f, 0.45f, 0.4f, 1.0f);

} // namespace

LoginPanel::LoginPanel(LoginFlow& flow, std::string loginUri) : flow_(flow)
{
    if(!loginUri.empty())
    {
        selectGrid(loginUri);
    }
}

void LoginPanel::selectGrid(const std::string& loginUri)
{
    const auto known = std::ranges::find(kGrids, std::string_view(loginUri), &Grid::uri);
    if(known != kGrids.end() && !known->uri.empty())
    {
        grid_ = static_cast<int>(known - kGrids.begin());
    }
    else
    {
        grid_ = static_cast<int>(kGrids.size()) - 1;
        customLoginUri_ = loginUri;
    }
}

void LoginPanel::setStart(const std::string& where)
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
        startRegion_ = where;
    }
}

LoginAction LoginPanel::logInWith(const session::Credentials& credentials)
{
    username_ = credentials.name.first + " " + credentials.name.last;
    selectGrid(credentials.loginUri);
    // A file's account isn't remembered: it's in the file.
    return flow_.logIn(
        {credentials.name, credentials.passwordHash, credentials.loginUri, start(), false});
}

std::string LoginPanel::start() const
{
    return start_ == 0 ? "last" : start_ == 1 ? "home" : session::startLocation(startRegion_);
}

LoginPanel::~LoginPanel()
{
    wipePassword();
}

void LoginPanel::wipePassword()
{
    std::ranges::fill(password_, '\0');
    password_.clear();
}

LoginAction LoginPanel::draw(const session::Session* session)
{
    LoginAction action;
    if(ImGui::Begin("Login"))
    {
        if(session && session->state() != session::SessionState::Failed)
        {
            drawSession(*session, action);
        }
        else
        {
            action = drawForm(session);
        }
    }
    ImGui::End();
    return action;
}

void LoginPanel::drawSession(const session::Session& session, LoginAction& action)
{
    using session::SessionState;
    const session::LoginSuccess* login = session.login();
    const SessionState state = session.state();
    if(state == SessionState::LoggingIn || !login)
    {
        ImGui::TextUnformatted("Logging in...");
        if(ImGui::Button("Cancel"))
        {
            action.kind = LoginAction::Kind::LogOut;
        }
        return;
    }

    ImGui::Text("%s %s (%s)", login->name.first.c_str(), login->name.last.c_str(),
                std::string(session::stateName(state)).c_str());
    ImGui::TextDisabled("%s", login->agentId.toString().c_str());
    if(!login->message.empty())
    {
        ImGui::TextWrapped("%s", login->message.c_str());
    }
    ImGui::BeginDisabled(state == SessionState::LoggingOut);
    if(ImGui::Button(state == SessionState::Connecting ? "Cancel" : "Log out"))
    {
        action.kind = LoginAction::Kind::LogOut;
    }
    ImGui::EndDisabled();

    ImGui::Text("Bandwidth: asking for %.0f kbps", static_cast<double>(session.currentBandwidth()));

    ImGui::SeparatorText("Regions");
    for(const auto& [id, region] : session.regions())
    {
        const std::string label =
            std::format("{}{} ({}, {})###region{}",
                        region->name.empty() ? region->endpoint.toString() : region->name,
                        region->isMain ? " [main]" : "", region->gridX(), region->gridY(), id);
        if(!ImGui::TreeNode(label.c_str()))
        {
            continue;
        }
        ImGui::Text("Simulator %s", region->endpoint.toString().c_str());
        if(const auto stats = session.circuitStats(id))
        {
            ImGui::Text("Ping %lld ms, packets in %llu / out %llu",
                        static_cast<long long>(stats->roundTrip.count()),
                        static_cast<unsigned long long>(stats->packetsIn),
                        static_cast<unsigned long long>(stats->packetsOut));
            ImGui::Text("Resent %llu, unacked %zu, duplicates %llu, dropped %llu, lost %llu",
                        static_cast<unsigned long long>(stats->resent), stats->unacked,
                        static_cast<unsigned long long>(stats->duplicates),
                        static_cast<unsigned long long>(stats->reliableDropped),
                        static_cast<unsigned long long>(stats->packetsLost));
        }
        ImGui::Text("Handshake: %s, event queue: %s", region->handshakeReceived ? "yes" : "no",
                    region->eventQueueRunning ? "running" : "stopped");
        const std::string capsLabel =
            std::format("Capabilities ({})###caps", region->capabilities.size());
        if(ImGui::TreeNode(capsLabel.c_str()))
        {
            for(const auto& [name, url] : region->capabilities)
            {
                ImGui::TextUnformatted(name.c_str());
                ImGui::SetItemTooltip("%s", url.c_str());
            }
            ImGui::TreePop();
        }
        ImGui::TreePop();
    }

    auto drawCounts = [](const char* title, const auto& counts)
    {
        const std::string label = std::format("{} ({})###{}", title, counts.size(), title);
        if(ImGui::TreeNode(label.c_str()))
        {
            for(const auto& [name, count] : counts)
            {
                ImGui::Text("%6llu  %s", static_cast<unsigned long long>(count), name.c_str());
            }
            ImGui::TreePop();
        }
    };
    ImGui::SeparatorText("Traffic");
    drawCounts("UDP messages", session.messageCounts());
    drawCounts("Event queue", session.eventCounts());
}

void LoginPanel::drawPrompt(LoginAction& action)
{
    using Prompt = LoginFlow::Prompt;
    switch(flow_.prompt())
    {
    case Prompt::None:
        return;
    case Prompt::Code:
    {
        ImGui::TextWrapped("%s", flow_.message().c_str());
        const bool entered = ImGui::InputText("Authenticator code", &mfaToken_,
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        if((ImGui::Button("Continue") || entered) && !mfaToken_.empty())
        {
            action = flow_.answerCode(mfaToken_);
            mfaToken_.clear();
        }
        ImGui::SameLine();
        if(ImGui::Button("Cancel"))
        {
            flow_.dismiss();
        }
        break;
    }
    case Prompt::Terms:
    case Prompt::Critical:
    {
        const bool tos = flow_.prompt() == Prompt::Terms;
        ImGui::BeginChild("message", ImVec2(0, 200), ImGuiChildFlags_Borders);
        ImGui::TextWrapped("%s", flow_.message().c_str());
        ImGui::EndChild();
        ImGui::Checkbox(tos ? "I agree to the Terms of Service" : "I have read this message",
                        &accepted_);
        ImGui::BeginDisabled(!accepted_);
        if(ImGui::Button("Continue"))
        {
            action = flow_.accept();
            accepted_ = false;
        }
        ImGui::EndDisabled();
        break;
    }
    case Prompt::Error:
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        ImGui::TextWrapped("%s", flow_.message().c_str());
        ImGui::PopStyleColor();
        ImGui::TextDisabled("(%s)", flow_.reason().c_str());
        break;
    }
    ImGui::Separator();
}

void LoginPanel::drawSaved(LoginAction& action)
{
    const auto& accounts = flow_.saved().accounts();
    if(accounts.empty())
    {
        return;
    }
    ImGui::SeparatorText("Saved accounts");
    std::optional<SavedAccount> forget;
    for(size_t i = 0; i < accounts.size(); i++)
    {
        const SavedAccount& a = accounts[i];
        const auto grid = std::ranges::find(kGrids, std::string_view(a.loginUri), &Grid::uri);
        const std::string label =
            std::format("{} {} ({})###saved{}", a.name.first, a.name.last,
                        grid != kGrids.end() ? grid->label : a.loginUri.c_str(), i);
        if(ImGui::Button(label.c_str()))
        {
            username_ = a.name.first + " " + a.name.last;
            selectGrid(a.loginUri);
            action = flow_.logInSaved(a, start());
        }
        ImGui::SameLine();
        if(ImGui::SmallButton(std::format("Forget###forget{}", i).c_str()))
        {
            forget = a;
        }
    }
    if(forget)
    {
        flow_.saved().forget(*forget);
    }
    ImGui::SeparatorText("Log in");
}

LoginAction LoginPanel::drawForm(const session::Session* session)
{
    (void)session;
    LoginAction action;
    drawPrompt(action);
    if(action.kind == LoginAction::Kind::LogIn)
    {
        return action;
    }
    drawSaved(action);
    if(action.kind == LoginAction::Kind::LogIn)
    {
        return action;
    }

    ImGui::InputText("Username", &username_);
    const bool entered =
        ImGui::InputText("Password", &password_,
                         ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);

    const char* gridLabel = kGrids[static_cast<size_t>(grid_)].label;
    if(ImGui::BeginCombo("Grid", gridLabel))
    {
        for(size_t i = 0; i < kGrids.size(); i++)
        {
            if(ImGui::Selectable(kGrids[i].label, grid_ == static_cast<int>(i)))
            {
                grid_ = static_cast<int>(i);
            }
        }
        ImGui::EndCombo();
    }
    const bool custom = kGrids[static_cast<size_t>(grid_)].uri.empty();
    if(custom)
    {
        ImGui::InputText("Login URI", &customLoginUri_);
    }

    ImGui::Combo("Start at", &start_, kStartLocations.data(),
                 static_cast<int>(kStartLocations.size()));
    if(start_ == 2)
    {
        ImGui::InputText("Region[/x/y/z]", &startRegion_);
    }

    ImGui::Checkbox("Remember me", &remember_);
    ImGui::SetItemTooltip("Saves the account in the system's credential store once it has "
                          "logged in (the password's hash, never the password)");

    if(ImGui::Button("Log in") || entered)
    {
        formError_.clear();
        auto name = session::parseAgentName(username_);
        const std::string loginUri =
            custom ? customLoginUri_ : std::string(kGrids[static_cast<size_t>(grid_)].uri);
        if(!name)
        {
            formError_ = name.error();
        }
        else if(password_.empty())
        {
            formError_ = "Enter a password.";
        }
        else if(loginUri.empty())
        {
            formError_ = "Enter a login URI.";
        }
        else if(start_ == 2 && startRegion_.empty())
        {
            formError_ = "Enter a region name.";
        }
        else
        {
            action = flow_.logIn(
                {std::move(*name), session::hashPassword(password_), loginUri, start(), remember_});
            wipePassword();
        }
    }
    if(!formError_.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        ImGui::TextWrapped("%s", formError_.c_str());
        ImGui::PopStyleColor();
    }
    return action;
}

} // namespace viewer
