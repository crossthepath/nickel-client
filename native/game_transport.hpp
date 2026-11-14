// Transport of the current connection, detected from the game's own outgoing UDP datagrams, so modules can adapt to the
// server: RakNet or NetherNet (WebRTC). The Winsock send functions are hooked (MinHook, export addresses) and only the
// first bytes of each datagram are looked at, nothing is changed:
// - RakNet connected datagrams start with the valid flag (0x80): data frames 0x80-0x8f, ACK 0xc0, NAK 0xa0; they count only
//   towards an address the game opened a RakNet connection to.
// - NetherNet carries the game over DTLS (records with content type 20-25 followed by the version byte 0xfe).
// Broadcast and multicast destinations (LAN discovery on the server list) are ignored. Without recent traffic of either
// kind (local world, menus) the transport is None.
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include "MinHook.h"
#pragma comment(lib, "ws2_32.lib")

namespace game {
enum class Transport : int { None = 0, RakNet = 1, NetherNet = 2 };

namespace transport_detail {
inline std::atomic<uint64_t> lastRakNet{0}, lastNetherNet{0};
inline std::atomic<uint64_t> rakNetCount{0}, netherNetCount{0};

inline bool Excluded(const sockaddr* to) {
    if (!to) return false;
    if (to->sa_family == AF_INET) {
        const uint8_t* ip = (const uint8_t*)&((const sockaddr_in*)to)->sin_addr;
        return ip[3] == 255 || (ip[0] >= 224 && ip[0] <= 239);   // broadcast, multicast
    }
    if (to->sa_family == AF_INET6) return ((const sockaddr_in6*)to)->sin6_addr.u.Byte[0] == 0xff;   // multicast
    return false;
}

// RakNet peers: addresses the game opened a RakNet connection to (Open Connection Request 1 / 2, ids 0x05 / 0x07,
// followed by the 16-byte offline magic). Only datagrams to these count, so other UDP traffic whose first byte happens
// to look like a RakNet frame (DNS ids, voice, telemetry) is never taken for a connection.
inline const uint8_t kOfflineMagic[16] = {0x00, 0xff, 0xff, 0x00, 0xfe, 0xfe, 0xfe, 0xfe, 0xfd, 0xfd, 0xfd, 0xfd, 0x12, 0x34, 0x56, 0x78};
struct Peer { uint16_t family = 0, port = 0; uint8_t ip[16]{}; };
inline SRWLOCK peerLock = SRWLOCK_INIT;
inline Peer peers[8]{};
inline int peerNext = 0;
inline bool PeerOf(const sockaddr* to, Peer* out) {
    if (!to) return false;
    *out = Peer{};
    out->family = to->sa_family;
    if (to->sa_family == AF_INET) { auto* a = (const sockaddr_in*)to; out->port = a->sin_port; memcpy(out->ip, &a->sin_addr, 4); return true; }
    if (to->sa_family == AF_INET6) { auto* a = (const sockaddr_in6*)to; out->port = a->sin6_port; memcpy(out->ip, &a->sin6_addr, 16); return true; }
    return false;
}
inline bool SamePeer(const Peer& a, const Peer& b) { return a.family == b.family && a.port == b.port && !memcmp(a.ip, b.ip, 16); }

inline void Classify(const uint8_t* b, size_t n, const sockaddr* to) {
    if (!b || n < 2 || Excluded(to)) return;
    Peer peer{};
    const bool addressed = PeerOf(to, &peer);
    if (addressed && ntohs(peer.port) == 53) return;   // DNS
    const uint64_t now = GetTickCount64();
    const uint8_t f = b[0];
    // NetherNet: DTLS records (content type 20-25, then the DTLS version byte 0xfe). STUN alone does not count.
    if (f >= 20 && f <= 25 && b[1] == 0xfe) { lastNetherNet = now; ++netherNetCount; return; }
    if (!addressed) return;
    if ((f == 0x05 || f == 0x07) && n >= 17 && !memcmp(b + 1, kOfflineMagic, 16)) {
        AcquireSRWLockExclusive(&peerLock);
        bool known = false;
        for (auto& p : peers) known |= SamePeer(p, peer);
        if (!known) { peers[peerNext] = peer; peerNext = (peerNext + 1) % 8; }
        ReleaseSRWLockExclusive(&peerLock);
        return;
    }
    if ((f & 0xf0) == 0x80 || f == 0xc0 || f == 0xa0) {
        AcquireSRWLockShared(&peerLock);
        bool known = false;
        for (auto& p : peers) known |= SamePeer(p, peer);
        ReleaseSRWLockShared(&peerLock);
        if (known) { lastRakNet = now; ++rakNetCount; }
    }
}

inline bool Datagram(SOCKET s) { int type = 0, size = sizeof(type); return getsockopt(s, SOL_SOCKET, SO_TYPE, (char*)&type, &size) == 0 && type == SOCK_DGRAM; }

using SendTo = int(WSAAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
using WsaSendTo = int(WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const sockaddr*, int, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using WsaSendMsg = int(WSAAPI*)(SOCKET, LPWSAMSG, DWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using Send = int(WSAAPI*)(SOCKET, const char*, int, int);
using WsaSend = int(WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
inline SendTo sendToOriginal = nullptr;
inline WsaSendTo wsaSendToOriginal = nullptr;
inline WsaSendMsg wsaSendMsgOriginal = nullptr;
inline Send sendOriginal = nullptr;
inline WsaSend wsaSendOriginal = nullptr;

inline int WSAAPI SendToDetour(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen) {
    if (len > 0) Classify((const uint8_t*)buf, (size_t)len, to);
    return sendToOriginal(s, buf, len, flags, to, tolen);
}
inline int WSAAPI WsaSendToDetour(SOCKET s, LPWSABUF bufs, DWORD count, LPDWORD sent, DWORD flags, const sockaddr* to, int tolen, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE done) {
    if (bufs && count) Classify((const uint8_t*)bufs[0].buf, bufs[0].len, to);
    return wsaSendToOriginal(s, bufs, count, sent, flags, to, tolen, ov, done);
}
inline int WSAAPI WsaSendMsgDetour(SOCKET s, LPWSAMSG msg, DWORD flags, LPDWORD sent, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE done) {
    if (msg && msg->lpBuffers && msg->dwBufferCount) Classify((const uint8_t*)msg->lpBuffers[0].buf, msg->lpBuffers[0].len, msg->name);
    return wsaSendMsgOriginal(s, msg, flags, sent, ov, done);
}
// Connected sockets: only datagram sockets count (TCP payloads are never classified).
inline int WSAAPI SendDetour(SOCKET s, const char* buf, int len, int flags) {
    if (len > 0 && Datagram(s)) { sockaddr_storage peer{}; int size = sizeof(peer); Classify((const uint8_t*)buf, (size_t)len, getpeername(s, (sockaddr*)&peer, &size) == 0 ? (const sockaddr*)&peer : nullptr); }
    return sendOriginal(s, buf, len, flags);
}
inline int WSAAPI WsaSendDetour(SOCKET s, LPWSABUF bufs, DWORD count, LPDWORD sent, DWORD flags, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE done) {
    if (bufs && count && Datagram(s)) { sockaddr_storage peer{}; int size = sizeof(peer); Classify((const uint8_t*)bufs[0].buf, bufs[0].len, getpeername(s, (sockaddr*)&peer, &size) == 0 ? (const sockaddr*)&peer : nullptr); }
    return wsaSendOriginal(s, bufs, count, sent, flags, ov, done);
}

template <class T> inline bool Hook(HMODULE ws2, const char* name, void* detour, T* original) {
    void* target = (void*)GetProcAddress(ws2, name);
    return target && MH_CreateHook(target, detour, (void**)original) == MH_OK && MH_EnableHook(target) == MH_OK;
}
}  // namespace transport_detail

// Installs the send hooks once (MinHook must already be initialised). Returns how many of the five were installed.
inline int InstallTransportDetection(void (*log)(const char*)) {
    using namespace transport_detail;
    static int installed = -1;
    if (installed >= 0) return installed;
    installed = 0;
    HMODULE ws2 = GetModuleHandleW(L"ws2_32.dll");
    if (!ws2) ws2 = LoadLibraryW(L"ws2_32.dll");
    if (!ws2) { if (log) log("Transport: ws2_32.dll not available, detection off"); return installed; }
    installed += Hook(ws2, "sendto", (void*)SendToDetour, &sendToOriginal);
    installed += Hook(ws2, "WSASendTo", (void*)WsaSendToDetour, &wsaSendToOriginal);
    installed += Hook(ws2, "WSASendMsg", (void*)WsaSendMsgDetour, &wsaSendMsgOriginal);
    installed += Hook(ws2, "send", (void*)SendDetour, &sendOriginal);
    installed += Hook(ws2, "WSASend", (void*)WsaSendDetour, &wsaSendOriginal);
    if (log) { char line[96]; wsprintfA(line, "Transport: detection installed (%d/5 send hooks)", installed); log(line); }
    return installed;
}

// Transport with traffic in the last 3 s; when both have some (switching servers), the most recent one.
inline Transport CurrentTransport() {
    using namespace transport_detail;
    const uint64_t now = GetTickCount64(), rak = lastRakNet.load(), nn = lastNetherNet.load();
    const bool rakLive = rak && now - rak < 3000, nnLive = nn && now - nn < 3000;
    if (rakLive && nnLive) return rak > nn ? Transport::RakNet : Transport::NetherNet;
    return rakLive ? Transport::RakNet : nnLive ? Transport::NetherNet : Transport::None;
}
inline bool OnRakNet() { return CurrentTransport() == Transport::RakNet; }
inline bool OnNetherNet() { return CurrentTransport() == Transport::NetherNet; }
inline const char* TransportName(Transport t) { return t == Transport::RakNet ? "RakNet" : t == Transport::NetherNet ? "NetherNet" : "None"; }

// Called once per frame: logs each change of transport.
inline void PollTransport(void (*log)(const char*)) {
    static Transport last = Transport::None;
    const Transport now = CurrentTransport();
    if (now == last) return;
    last = now;
    if (log) { char line[64]; wsprintfA(line, "Transport: %s", TransportName(now)); log(line); }
}
}  // namespace game
