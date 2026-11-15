#pragma once
// Events a Luau script module can handle with module:on(name, handler).
// No game access: tests/script_events_checks.cpp.
//
// Each event has a fixed bit, so the engine can keep the set of events a module handles as a mask and answer "does any
// enabled module want this event?" with bit tests instead of string comparisons (the question is asked once per packet
// and several times per frame). Names are the public script API: they must never change or be reordered.
#include <cstdint>
#include <cstring>

namespace nickel::script {

enum class Event : uint8_t {
    Update,
    Tick,
    Render2d,
    Render3d,
    Attack,
    PacketSend,
    PacketReceive,
    Chat,
    Key,
    Move,
    Jump,
    Hurt,
    Death,
    Respawn,
    Join,
    Leave,
    Dimension,
    Screen,
    ContainerOpen,
    ContainerClose,
    PathEnd,
    Teleport,
    Hit,
};

inline constexpr int kEventCount = 23;
static_assert(static_cast<int>(Event::Hit) + 1 == kEventCount, "kEventCount counts every Event");

// Indexed by Event; the names scripts pass to module:on.
inline constexpr const char* kEventNames[kEventCount] = {
        "update",    "tick",   "render2d",      "render3d",       "attack",  "packetSend", "packetReceive", "chat",
        "key",       "move",   "jump",          "hurt",           "death",   "respawn",    "join",          "leave",
        "dimension", "screen", "containerOpen", "containerClose", "pathEnd", "teleport",   "hit",
};

using EventMask = uint32_t;
static_assert(kEventCount <= 32, "every event needs a bit in EventMask");

constexpr EventMask Bit(Event event) {
    return EventMask{1} << static_cast<int>(event);
}

constexpr const char* Name(Event event) {
    return kEventNames[static_cast<int>(event)];
}

// The event called `name` (exact, case-sensitive match), or false for a name that is not an event.
inline bool EventFromName(const char* name, Event* event) {
    if (name == nullptr) {
        return false;
    }
    for (int i = 0; i < kEventCount; ++i) {
        if (std::strcmp(name, kEventNames[i]) == 0) {
            *event = static_cast<Event>(i);
            return true;
        }
    }
    return false;
}

} // namespace nickel::script
