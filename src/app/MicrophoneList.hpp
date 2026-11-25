#pragma once

// The microphones there are, for the Settings tab: asked for on a worker
// (the system's sound service may take a moment to answer), again every few
// seconds while they're shown, so one plugged in appears.

#include "Settings.hpp"

#include "viewer/core/JobSystem.hpp"
#include "viewer/core/MessageQueue.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace viewer
{

// What to choose from: the system's default (""), each microphone by name
// (once), and the one chosen if it isn't among them, saying so.
std::vector<SettingChoice> microphoneChoices(const std::vector<std::string>& names,
                                             std::string_view chosen);

class MicrophoneList
{
public:
    explicit MicrophoneList(JobSystem& jobs) : jobs_(jobs) {}

    // Once a frame while they're shown (`now` in seconds).
    void update(double now);
    // Nothing until they're known.
    const std::optional<std::vector<std::string>>& names() const { return names_; }

private:
    using Answers = MessageQueue<std::vector<std::string>>;

    JobSystem& jobs_;
    std::shared_ptr<Answers> answers_ = std::make_shared<Answers>(); // (a job may outlast us)
    std::optional<std::vector<std::string>> names_;
    bool asked_ = false;
    double nextAt_ = 0.0;
};

} // namespace viewer
