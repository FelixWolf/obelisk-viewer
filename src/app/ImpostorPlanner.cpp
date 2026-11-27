#include "ImpostorPlanner.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace viewer
{

void ImpostorPlanner::clear()
{
    plans_.clear();
    taken_.clear();
    used_.fill(false);
}

render::Impostor ImpostorPlanner::frame(const glm::vec3& eye, const glm::vec3& c, float radius,
                                        uint32_t slot)
{
    const glm::vec3 toward = c - eye;
    const float distance = glm::length(toward);
    const glm::vec3 forward = toward / distance;
    // Upright (no roll), unless looking straight down or up.
    const glm::vec3 worldUp =
        std::abs(forward.z) > 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::mat4 view = glm::lookAt(eye, c, worldUp);
    // A square frustum just around the sphere.
    const float sine = std::min(radius / distance, 0.99f);
    const float tangent = sine / std::sqrt(1.0f - sine * sine);
    const float nearPlane = std::max(distance - radius, 0.05f);
    const float farPlane = distance + radius;
    const glm::mat4 projection =
        glm::frustum(-tangent * nearPlane, tangent * nearPlane, -tangent * nearPlane,
                     tangent * nearPlane, nearPlane, farPlane);
    // The card: the frustum's square where the sphere's center is (as
    // glm::lookAt's axes: right is forward x up).
    const glm::vec3 right = glm::normalize(glm::cross(forward, worldUp));
    const glm::vec3 up = glm::cross(right, forward);
    const float half = tangent * distance;
    render::Impostor card;
    card.slot = slot;
    card.capture = true;
    card.captureViewProjection = projection * view;
    card.eye = eye;
    card.center = c;
    card.right = right * half;
    card.up = up * half;
    return card;
}

void ImpostorPlanner::update(std::span<const Candidate> candidates, const glm::vec3& eye,
                             const Options& options, double now)
{
    // Nearest first.
    std::vector<std::pair<float, const Candidate*>> sorted;
    sorted.reserve(candidates.size());
    for(const Candidate& c : candidates)
    {
        sorted.emplace_back(glm::length(c.center - eye), &c);
    }
    std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.first < b.first; });

    // Who's to be one, and how often their picture is taken.
    struct Want
    {
        const Candidate* candidate = nullptr;
        float distance = 0.0f;
        double period = 0.0;
    };
    std::unordered_map<Uuid, Want> wanted;
    if(options.maxFull > 0)
    {
        for(size_t rank = options.maxFull; rank < sorted.size(); rank++)
        {
            const auto [distance, c] = sorted[rank];
            // Not up close, nor where the picture's camera would be inside
            // the sphere (the official viewer's least: its size and a meter).
            if(distance < std::max(options.minDistance, 1.0f + c->radius) ||
               distance <= c->radius * 1.05f)
            {
                continue;
            }
            const double period = rank < 3 * size_t{options.maxFull}   ? kPeriods[0]
                                  : rank < 4 * size_t{options.maxFull} ? kPeriods[1]
                                                                       : kPeriods[2];
            wanted[c->id] = {c, distance, period};
        }
    }

    // Those no longer impostors give back their slots.
    for(auto it = plans_.begin(); it != plans_.end();)
    {
        if(!wanted.contains(it->first))
        {
            used_[it->second.slot] = false;
            taken_.erase(it->first);
            it = plans_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // Whose picture is due, and how much: new ones first (they're drawn
    // whole until it's taken), then the most overdue.
    struct Due
    {
        Uuid id;
        double score = 0.0;
        bool isNew = false;
    };
    std::vector<Due> due;
    for(const auto& [id, w] : wanted)
    {
        const auto taken = taken_.find(id);
        if(taken == taken_.end())
        {
            // Nearer first among new ones.
            due.push_back({id, 1e9 - static_cast<double>(w.distance), true});
            continue;
        }
        const Taken& t = taken->second;
        const glm::vec3 c = w.candidate->center;
        const float then = glm::length(t.eye - t.center);
        const float cosine = glm::dot(glm::normalize(t.eye - t.center), glm::normalize(eye - c));
        const float angle = std::acos(std::clamp(cosine, -1.0f, 1.0f));
        const double score = std::max(
            {(now - t.at) / w.period, static_cast<double>(angle / options.maxAngle),
             static_cast<double>(std::abs(w.distance - then) / then / options.maxDistanceChange)});
        if(score >= 1.0)
        {
            due.push_back({id, score, false});
        }
    }
    std::ranges::sort(due, [](const Due& a, const Due& b)
                      { return a.isNew != b.isNew ? a.isNew : a.score > b.score; });

    // A free slot, big ones for those shown big.
    auto freeSlot = [&](bool big) -> std::optional<uint32_t>
    {
        const auto search = [&](uint32_t from, uint32_t to) -> std::optional<uint32_t>
        {
            for(uint32_t s = from; s < to; s++)
            {
                if(!used_[s])
                {
                    return s;
                }
            }
            return std::nullopt;
        };
        const auto bigSlot = search(0, render::kImpostorBigSlots);
        const auto smallSlot = search(render::kImpostorBigSlots, render::kImpostorSlots);
        return big ? (bigSlot ? bigSlot : smallSlot) : (smallSlot ? smallSlot : bigSlot);
    };

    // Those due, a few a frame.
    for(auto& [id, plan] : plans_)
    {
        plan.capture = false;
        plan.card.capture = false;
    }
    uint32_t captures = 0;
    for(const Due& d : due)
    {
        if(captures >= options.capturesPerFrame)
        {
            break;
        }
        const Want& w = wanted.at(d.id);
        const Candidate& c = *w.candidate;
        const bool big = c.pixels > static_cast<float>(render::kImpostorSmallSize) * 1.25f;
        auto plan = plans_.find(d.id);
        std::optional<uint32_t> slot;
        if(plan != plans_.end())
        {
            slot = plan->second.slot;
            // Shown bigger (or smaller) than its slot: a better one, if free.
            const bool isBig = *slot < render::kImpostorBigSlots;
            if(isBig != big)
            {
                if(const auto other = freeSlot(big);
                   other && (*other < render::kImpostorBigSlots) == big)
                {
                    used_[*slot] = false;
                    slot = other;
                    used_[*slot] = true;
                }
            }
        }
        else
        {
            slot = freeSlot(big);
            if(!slot)
            {
                continue; // no room: drawn whole
            }
            used_[*slot] = true;
        }
        Plan& p = plans_[d.id];
        p.slot = *slot;
        p.capture = true;
        p.card = frame(eye, c.center, c.radius, *slot);
        taken_[d.id] = {eye, c.center, now};
        captures++;
    }

    // The rest show their last picture, where the avatar is now.
    for(auto& [id, plan] : plans_)
    {
        if(!plan.capture)
        {
            plan.card.offset = wanted.at(id).candidate->center - plan.card.center;
        }
    }
}

} // namespace viewer
