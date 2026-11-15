// Script events (native/script_events.hpp). The names scripts pass to module:on are the public API, so they are
// pinned here in the order and spelling the engine has always accepted; every event has its own bit; unknown names
// are refused.
#include "script_events.hpp"

#include <cstdio>
#include <cstring>

namespace {

using nickel::script::Bit;
using nickel::script::Event;
using nickel::script::EventFromName;
using nickel::script::EventMask;
using nickel::script::kEventCount;
using nickel::script::Name;

// The list accepted by module:on before events had an enum (script_engine.hpp, scriptEvents[]).
const char* const kAcceptedNames[] = {
        "update",    "tick",   "render2d",      "render3d",       "attack",  "packetSend", "packetReceive", "chat",
        "key",       "move",   "jump",          "hurt",           "death",   "respawn",    "join",          "leave",
        "dimension", "screen", "containerOpen", "containerClose", "pathEnd", "teleport",   "hit",
};

int failed = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++failed;
    }
}

} // namespace

int main() {
    Check(sizeof(kAcceptedNames) / sizeof(kAcceptedNames[0]) == kEventCount, "23 events, as before");

    bool sameNames = true;
    bool roundTrip = true;
    for (int i = 0; i < kEventCount; ++i) {
        const Event event = static_cast<Event>(i);
        sameNames &= std::strcmp(Name(event), kAcceptedNames[i]) == 0;
        Event parsed{};
        roundTrip &= EventFromName(kAcceptedNames[i], &parsed) && parsed == event;
    }
    Check(sameNames, "every event keeps its name and position");
    Check(roundTrip, "every accepted name maps to its event");

    EventMask all = 0;
    bool distinct = true;
    for (int i = 0; i < kEventCount; ++i) {
        const EventMask bit = Bit(static_cast<Event>(i));
        distinct &= bit != 0 && (all & bit) == 0 && (bit & (bit - 1)) == 0;
        all |= bit;
    }
    Check(distinct, "each event has a single bit of its own");
    Check(all == (EventMask{1} << kEventCount) - 1, "the bits are 0 to 22");

    Event untouched = Event::Tick;
    Check(!EventFromName("Update", &untouched), "names are case-sensitive");
    Check(!EventFromName("", &untouched), "an empty name is refused");
    Check(!EventFromName("render", &untouched), "a prefix of a name is refused");
    Check(!EventFromName("hit2", &untouched), "a longer name is refused");
    Check(!EventFromName(nullptr, &untouched), "no name is refused");
    Check(untouched == Event::Tick, "a refused name leaves the output alone");

    // The masks the menu tests when it wires the game hooks (menu.cpp, DrawHud).
    const EventMask receive = Bit(Event::Chat) | Bit(Event::PacketReceive) | Bit(Event::ContainerOpen)
                              | Bit(Event::ContainerClose) | Bit(Event::Teleport);
    Check((receive & Bit(Event::PacketSend)) == 0, "the receive hook is not wired for packetSend alone");
    Check((receive & Bit(Event::Teleport)) != 0, "the receive hook is wired for teleport");

    std::printf("%s\n", failed ? "script event checks FAILED" : "script event checks passed");
    return failed ? 1 : 0;
}
