// Packet capture in the game (packet_capture.hpp keeps the records; Nickel AI's capture_packets / read_packets and
// get_server_info read them). Installed once Nickel AI is on (the session of a server starts at the join), never
// while it is off.
// - MC 0x226b050 (outer peer, const std::string* data, bool outgoing): the game's connection wrapper calls it with the
//   serialized bytes of every packet, [varint header: id in the low 10 bits][payload], from its send (0x226b000,
//   outgoing = 1) and its receive (0x226b560, outgoing = 0), for every connection type (RakNet, NetherNet, local).
// - The detour only copies the bytes, for the server connection (peer = *(connection+0x118), the connection being the
//   first element of [network+0xe8, +0xf0), as game_lag.hpp reads it), and always calls the original; it calls no
//   other game code. The server peer is read on the render thread every frame while a capture runs (CaptureTick).
#pragma once
#include "game_lag.hpp"
#include "packet_capture.hpp"
#include <MinHook.h>
#include <atomic>

namespace game {
inline std::atomic<uint64_t> captureServerPeer{0};
inline std::atomic<bool> captureFault{false};
using CaptureFunction = uint64_t (*)(uint64_t, const std::string*, uint64_t);
inline CaptureFunction captureOriginal = nullptr;

inline int64_t CaptureNowMs() { return static_cast<int64_t>(GetTickCount64()); }

inline void CaptureWork(uint64_t peer, const std::string* data, bool outgoing) {
    if (!data || !peer || peer != captureServerPeer.load()) {
        return;
    }
    const int64_t now = CaptureNowMs();
    const auto* bytes = reinterpret_cast<const unsigned char*>(data->data());
    nickel::packets::SharedSession().Record(now, outgoing, bytes, data->size());
    auto& capture = nickel::packets::Shared();
    if (capture.Capturing(now)) {
        capture.Record(now, outgoing, bytes, data->size());
    }
}
inline void CaptureGuarded(uint64_t peer, const std::string* data, bool outgoing) {
    __try {
        CaptureWork(peer, data, outgoing);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        captureFault = true;
    }
}
inline uint64_t CaptureDetour(uint64_t peer, const std::string* data, uint64_t outgoing) {
    if (!captureFault) {
        CaptureGuarded(peer, data, (outgoing & 0xff) != 0);
    }
    return captureOriginal(peer, data, outgoing);
}

inline std::atomic<bool> captureInstalled{false};

// The server connection's outer peer, or 0 outside a server (render thread, once a frame once the hook is in); a new
// peer starts a new session (get_server_info).
inline void CaptureTick() {
    if (!captureInstalled) {
        return;
    }
    Context c{};
    uint64_t peer = 0;
    if (Client(&c)) {
        const uint64_t net = LagNetwork(c);
        uint64_t begin{}, end{}, connection{};
        uint8_t closed{};
        if (net && Read(net + 0xe8, &begin) && Read(net + 0xf0, &end) && begin != end && Read(begin, &connection)
            && connection && Read(connection + 0x199, &closed) && !closed) {
            Read(connection + 0x118, &peer);
        }
    }
    captureServerPeer = peer;
    nickel::packets::SharedSession().Begin(peer, CaptureNowMs());
}

// 1 installed, -1 not available (unexpected code or MinHook refused); installs once.
inline int InstallCaptureHook(void (*log)(const char*)) {
    static int state = 0;
    if (state) {
        return state;
    }
    state = -1;
    uintptr_t base{};
    uint32_t size{};
    if (!Supported(&base, &size)) {
        return state;
    }
    static const uint8_t bytes[47] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57, 0x53,
                                      0x48, 0x81, 0xec, 0x28, 0x03, 0x00, 0x00, 0x48, 0x8d, 0xac, 0x24, 0x80,
                                      0x00, 0x00, 0x00, 0x48, 0xc7, 0x85, 0xa0, 0x02, 0x00, 0x00, 0xfe, 0xff,
                                      0xff, 0xff, 0x44, 0x89, 0xc3, 0x48, 0x89, 0xd7, 0x48, 0x89, 0xce};
    void* target = reinterpret_cast<void*>(base + Rva(0x226b050));
    if (!CodeIs(base + Rva(0x226b050), bytes, sizeof(bytes))) {
        if (log) log("Packet capture: unexpected code at the packet bytes function");
        return state;
    }
    if (MH_CreateHook(target, reinterpret_cast<void*>(&CaptureDetour), reinterpret_cast<void**>(&captureOriginal)) != MH_OK) {
        if (log) log("Packet capture: hook not installed");
        return state;
    }
    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        if (log) log("Packet capture: hook not installed (could not be enabled, removed)");
        return state;
    }
    if (log) log("Packet capture: hook installed");
    captureInstalled = true;
    state = 1;
    return state;
}
} // namespace game
