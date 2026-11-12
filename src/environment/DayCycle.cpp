#include "viewer/environment/DayCycle.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace viewer::environment
{

std::expected<DayCycle, std::string> DayCycle::fromEnvironment(const llsd::Value& environment)
{
    if(!environment.isMap())
    {
        return std::unexpected("no environment map");
    }
    const llsd::Value& day = environment["day_cycle"];
    if(!day.isMap())
    {
        return std::unexpected("no day cycle (only an asset id?)");
    }

    DayCycle cycle;
    cycle.name = day["name"].asString();
    if(environment.has("day_length") && environment["day_length"].asInteger() > 0)
    {
        cycle.dayLength = environment["day_length"].asInteger();
    }
    if(environment.has("day_offset"))
    {
        cycle.dayOffset = environment["day_offset"].asInteger();
    }
    const llsd::Value& altitudes = environment["track_altitudes"];
    for(size_t i = 0; i < cycle.trackAltitudes.size() && i < altitudes.size(); i++)
    {
        cycle.trackAltitudes[i] = static_cast<float>(altitudes[i].asReal());
    }

    // Sky frames by name.
    std::map<std::string, size_t, std::less<>> byName;
    if(const auto* frames = day["frames"].get<llsd::Map>())
    {
        for(const auto& [name, frame] : *frames)
        {
            if(frame["type"].asString() == "sky")
            {
                byName.emplace(name, cycle.skies.size());
                cycle.skies.push_back(SkySettings::fromLlsd(frame));
            }
        }
    }

    const llsd::Value& tracks = day["tracks"];
    for(size_t t = 0; t < kSkyTracks && t + 1 < tracks.size(); t++)
    {
        const llsd::Value& track = tracks[t + 1];
        for(size_t k = 0; k < track.size(); k++)
        {
            const auto it = byName.find(track[k]["key_name"].asString());
            if(it == byName.end())
            {
                continue; // a water frame, or missing
            }
            cycle.tracks[t].push_back(
                {std::clamp(static_cast<float>(track[k]["key_keyframe"].asReal()), 0.0f, 1.0f),
                 it->second});
        }
        std::sort(cycle.tracks[t].begin(), cycle.tracks[t].end(),
                  [](const Keyframe& a, const Keyframe& b) { return a.position < b.position; });
    }
    if(cycle.tracks[0].empty())
    {
        return std::unexpected("the day cycle has no sky on its ground track");
    }
    return cycle;
}

double DayCycle::position(double unixSeconds) const
{
    if(dayLength <= 0.0)
    {
        return 0.0;
    }
    const double p = std::fmod(unixSeconds + dayOffset, dayLength) / dayLength;
    return p < 0.0 ? p + 1.0 : p;
}

SkySettings DayCycle::skyAt(double position, float altitude) const
{
    // The highest track the altitude reaches that has keyframes; track 1
    // (index 0) otherwise.
    size_t track = 0;
    for(size_t i = 0; i < trackAltitudes.size(); i++)
    {
        if(altitude >= trackAltitudes[i] && !tracks[i + 1].empty())
        {
            track = i + 1;
        }
    }
    const std::vector<Keyframe>& keys = tracks[track];
    if(keys.empty())
    {
        return SkySettings{};
    }
    if(keys.size() == 1)
    {
        return skies[keys[0].sky];
    }

    // The keyframes on either side, wrapping around the end of the day.
    const auto p = static_cast<float>(position);
    auto after = std::upper_bound(keys.begin(), keys.end(), p,
                                  [](float v, const Keyframe& k) { return v < k.position; });
    const Keyframe& next = after == keys.end() ? keys.front() : *after;
    const Keyframe& previous = after == keys.begin() ? keys.back() : *(after - 1);
    float span = next.position - previous.position;
    float into = p - previous.position;
    if(span <= 0.0f)
    {
        span += 1.0f;
    }
    if(into < 0.0f)
    {
        into += 1.0f;
    }
    return blend(skies[previous.sky], skies[next.sky], span > 0.0f ? into / span : 0.0f);
}

} // namespace viewer::environment
