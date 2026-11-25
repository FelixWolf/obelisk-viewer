#include "MicrophoneList.hpp"

#include "viewer/audio/Microphone.hpp"

#include <algorithm>
#include <utility>

namespace viewer
{

namespace
{

constexpr double kEverySeconds = 3.0;

} // namespace

std::vector<SettingChoice> microphoneChoices(const std::vector<std::string>& names,
                                             std::string_view chosen)
{
    std::vector<SettingChoice> choices{{"", "System default"}};
    for(const std::string& name : names)
    {
        if(!name.empty() &&
           std::ranges::find(choices, name, &SettingChoice::value) == choices.end())
        {
            choices.push_back({name, name});
        }
    }
    if(std::ranges::find(choices, chosen, &SettingChoice::value) == choices.end())
    {
        // Unplugged, or part of a name (as it once was typed).
        const bool part = audio::Microphone::match(names, std::string(chosen)).has_value();
        choices.push_back({std::string(chosen),
                           std::string(chosen) + (part ? " (part of a name)"
                                                       : " (not connected: using the default)")});
    }
    return choices;
}

void MicrophoneList::update(double now)
{
    std::vector<std::vector<std::string>> answers;
    if(answers_->drain(answers))
    {
        names_ = std::move(answers.back());
        asked_ = false;
        nextAt_ = now + kEverySeconds;
    }
    if(!asked_ && now >= nextAt_)
    {
        asked_ = true;
        jobs_.submit([answers = answers_] { answers->push(audio::Microphone::names()); });
    }
}

} // namespace viewer
