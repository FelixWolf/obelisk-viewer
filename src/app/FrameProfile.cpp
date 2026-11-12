#include "FrameProfile.hpp"

#include <format>

namespace viewer
{

std::string FrameProfile::describe(const Times& times)
{
    std::string out = std::format("{:.2f} ms:", sum(times));
    for(size_t s = 0; s < kStageCount; s++)
    {
        if(times[s] >= 0.005f)
        {
            out += std::format(" {} {:.2f}", kNames[s], times[s]);
        }
    }
    return out;
}

} // namespace viewer
