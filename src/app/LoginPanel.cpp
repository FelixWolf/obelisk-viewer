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

LoginPanel::LoginPanel(session::MachineInfo machine, std::string channel, std::string version,
                       std::string loginUri)
    : machine_(std::move(machine)), channel_(std::move(channel)), version_(std::move(version))
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

LoginAction LoginPanel::logInWith(const session::Credentials& credentials)
{
    username_ = credentials.name.first + " " + credentials.name.last;
    selectGrid(credentials.loginUri);
    return makeLogIn(credentials.name, credentials.passwordHash, credentials.loginUri);
}

LoginAction LoginPanel::makeLogIn(session::AgentName name, std::string passwordHash,
                                  std::string loginUri)
{
    LoginAction action;
    session::LoginParameters& p = action.parameters;
    p.loginUri = std::move(loginUri);
    p.name = std::move(name);
    p.passwordHash = std::move(passwordHash);
    p.start = start_ == 0   ? "last"
              : start_ == 1 ? "home"
                            : std::format("uri:{}&128&128&0", startRegion_);
    p.channel = channel_;
    p.version = version_;
    p.machine = machine_;
    lastAttempt_ = p;
    action.kind = LoginAction::Kind::LogIn;
    return action;
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
            ImGui::Text("Resent %llu, unacked %zu, duplicates %llu, dropped %llu",
                        static_cast<unsigned long long>(stats->resent), stats->unacked,
                        static_cast<unsigned long long>(stats->duplicates),
                        static_cast<unsigned long long>(stats->reliableDropped));
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

LoginAction LoginPanel::drawForm(const session::Session* session)
{
    LoginAction action;
    const session::LoginFailure* failure = session ? session->failure() : nullptr;

    // Prompts the server can answer a login with; the retry reuses the last
    // attempt's parameters (including the password hash).
    if(failure && lastAttempt_)
    {
        if(failure->reason == "mfa_challenge")
        {
            ImGui::TextWrapped("%s", failure->message.c_str());
            const bool entered = ImGui::InputText("Authenticator code", &mfaToken_,
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
            if((ImGui::Button("Continue") || entered) && !mfaToken_.empty())
            {
                action.kind = LoginAction::Kind::LogIn;
                action.parameters = *lastAttempt_;
                action.parameters.mfaToken = mfaToken_;
                mfaToken_.clear();
            }
            ImGui::Separator();
        }
        else if(failure->reason == "tos" || failure->reason == "critical")
        {
            const bool tos = failure->reason == "tos";
            ImGui::BeginChild("message", ImVec2(0, 200), ImGuiChildFlags_Borders);
            ImGui::TextWrapped("%s", failure->message.c_str());
            ImGui::EndChild();
            ImGui::Checkbox(tos ? "I agree to the Terms of Service" : "I have read this message",
                            &accepted_);
            ImGui::BeginDisabled(!accepted_);
            if(ImGui::Button("Continue"))
            {
                action.kind = LoginAction::Kind::LogIn;
                action.parameters = *lastAttempt_;
                (tos ? action.parameters.agreeToTos : action.parameters.readCritical) = true;
                accepted_ = false;
            }
            ImGui::EndDisabled();
            ImGui::Separator();
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
            ImGui::TextWrapped("%s", failure->message.c_str());
            ImGui::PopStyleColor();
            ImGui::TextDisabled("(%s)", failure->reason.c_str());
            ImGui::Separator();
        }
    }
    if(action.kind == LoginAction::Kind::LogIn)
    {
        lastAttempt_ = action.parameters;
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
        ImGui::InputText("Region name", &startRegion_);
    }

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
            action = makeLogIn(std::move(*name), session::hashPassword(password_), loginUri);
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
