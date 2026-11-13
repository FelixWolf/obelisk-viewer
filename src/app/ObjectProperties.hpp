#pragma once

// Object properties: a prim's name, description, and what its touch and sit
// actions are called (llSetTouchText, llSetSitText). Regions send them
// (ObjectProperties) only for selected prims, so asking for a prim's
// selects it and deselects it once they're in, as the official viewer does
// to fill its pie menu and inspector. Kept for a while, then asked again
// (scripts can change them).

#include "viewer/core/Uuid.hpp"
#include "viewer/protocol/CircuitManager.hpp"
#include "viewer/session/Session.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace viewer
{

struct ObjectInfo
{
    std::string name;
    std::string description;
    std::string touchName; // empty: "Touch"
    std::string sitName;   // empty: "Sit"
};

class ObjectProperties
{
public:
    // A prim's properties, asked for if need be (or again once old); null
    // until they arrive.
    const ObjectInfo* get(session::Session& session, uint64_t region, uint32_t localId,
                          const Uuid& id);
    // An ObjectProperties message from `region`: keeps the properties, and
    // deselects what was selected to get them.
    void handle(session::Session& session, uint64_t region, const protocol::CircuitEvent& event);

private:
    struct Entry
    {
        ObjectInfo info;
        bool ready = false;
        uint64_t region = 0;
        uint32_t localId = 0;
        std::chrono::steady_clock::time_point askedAt{};
        std::chrono::steady_clock::time_point receivedAt{};
    };
    std::unordered_map<Uuid, Entry> entries_;
};

} // namespace viewer
