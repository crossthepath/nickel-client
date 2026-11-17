// The packet capture of Nickel AI's packet tools (capture_packets, read_packets in ai_tools.hpp): the serialized
// packets between the game and its server, as they cross the network layer (game_packet_capture.hpp records them),
// kept in a bounded buffer while a capture runs. No game code here, so tests/packet_capture_checks.cpp runs it as is.
//
// A record is one packet: its direction, its id (the low 10 bits of the varint header), its full size and its bytes
// after the header (at most kMaxStoredBytes; `truncated` when cut). The buffer keeps the newest kMaxPackets records
// and kMaxTotalBytes of payload, dropping the oldest. A capture stops by itself after its duration (at most
// kMaxCaptureSeconds), so a forgotten capture does not keep recording.
// Thread-safe: Record runs on the game's network threads, the rest on the render thread.
#ifndef NICKEL_PACKET_CAPTURE_HPP
#define NICKEL_PACKET_CAPTURE_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace nickel::packets {

constexpr size_t kMaxPackets = 4000;
constexpr size_t kMaxStoredBytes = 8 * 1024;
constexpr size_t kMaxTotalBytes = 16 * 1024 * 1024;
constexpr int kMaxCaptureSeconds = 30 * 60;
constexpr int kDefaultCaptureSeconds = 5 * 60;

struct Record {
    uint64_t seq = 0;      // 1, 2, 3... in arrival order, never reused within a capture
    int64_t atMs = 0;      // milliseconds since the capture started
    bool outgoing = false; // sent by the client (true) or received from the server
    int id = -1;           // packet id
    uint32_t size = 0;     // full serialized size, header included
    bool truncated = false;
    std::string payload;   // the bytes after the header (at most kMaxStoredBytes)
};

// The varint header of a serialized packet: the packet id is its low 10 bits. False when the header is malformed.
inline bool ReadHeader(const unsigned char* data, size_t size, int* id, size_t* headerBytes) {
    uint32_t value = 0;
    for (size_t at = 0; at < size && at < 5; ++at) {
        value |= static_cast<uint32_t>(data[at] & 0x7f) << (7 * at);
        if (!(data[at] & 0x80)) {
            *id = static_cast<int>(value & 0x3ff);
            *headerBytes = at + 1;
            return true;
        }
    }
    return false;
}

inline std::string Hex(const std::string& bytes, size_t limit) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    const size_t n = std::min(bytes.size(), limit);
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        const auto b = static_cast<unsigned char>(bytes[i]);
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

struct Status {
    bool capturing = false;
    int64_t elapsedMs = 0;
    int64_t leftMs = 0;    // until the capture stops by itself
    size_t kept = 0;       // records in the buffer
    uint64_t seen = 0;     // packets recorded since the capture started (kept or dropped since)
    uint64_t dropped = 0;  // oldest records dropped for room
    std::vector<int> ids;  // the capture's id filter (empty: every packet)
};

struct Query {
    int direction = 0;          // 0 both, 1 outgoing, -1 incoming
    std::vector<int> ids;       // empty: every id
    uint64_t fromSeq = 0;       // records with seq >= fromSeq
    size_t limit = 50;
};

class Capture {
public:
    // Starts (or restarts) a capture for `seconds` (clamped to 1..kMaxCaptureSeconds), keeping only `ids` when not
    // empty. The buffer is cleared.
    void Start(int64_t nowMs, int seconds, const std::vector<int>& ids) {
        std::lock_guard<std::mutex> lock(guard_);
        records_.clear();
        totalBytes_ = 0;
        seen_ = 0;
        dropped_ = 0;
        nextSeq_ = 1;
        startMs_ = nowMs;
        untilMs_ = nowMs + 1000LL * std::clamp(seconds, 1, kMaxCaptureSeconds);
        ids_ = std::set<int>(ids.begin(), ids.end());
        capturing_ = true;
    }

    // Stops recording; the records stay readable until the next Start or Clear.
    void Stop() {
        std::lock_guard<std::mutex> lock(guard_);
        capturing_ = false;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(guard_);
        records_.clear();
        totalBytes_ = 0;
    }

    // True while recording (cheap check for the hook before it copies anything).
    bool Capturing(int64_t nowMs) {
        std::lock_guard<std::mutex> lock(guard_);
        return CapturingLocked(nowMs);
    }

    // One serialized packet (header included) seen at `nowMs`.
    void Record(int64_t nowMs, bool outgoing, const unsigned char* data, size_t size) {
        int id = -1;
        size_t header = 0;
        if (!data || !size || !ReadHeader(data, size, &id, &header)) {
            return;
        }
        std::lock_guard<std::mutex> lock(guard_);
        if (!CapturingLocked(nowMs) || (!ids_.empty() && !ids_.count(id))) {
            return;
        }
        nickel::packets::Record r;
        r.seq = nextSeq_++;
        r.atMs = nowMs - startMs_;
        r.outgoing = outgoing;
        r.id = id;
        r.size = static_cast<uint32_t>(std::min<size_t>(size, UINT32_MAX));
        const size_t keep = std::min(size - header, kMaxStoredBytes);
        r.truncated = keep < size - header;
        r.payload.assign(reinterpret_cast<const char*>(data + header), keep);
        totalBytes_ += r.payload.size();
        records_.push_back(std::move(r));
        ++seen_;
        while (!records_.empty() && (records_.size() > kMaxPackets || totalBytes_ > kMaxTotalBytes)) {
            totalBytes_ -= records_.front().payload.size();
            records_.pop_front();
            ++dropped_;
        }
    }

    Status Read(int64_t nowMs) {
        std::lock_guard<std::mutex> lock(guard_);
        Status s;
        s.capturing = CapturingLocked(nowMs);
        s.elapsedMs = startMs_ ? nowMs - startMs_ : 0;
        s.leftMs = s.capturing ? untilMs_ - nowMs : 0;
        s.kept = records_.size();
        s.seen = seen_;
        s.dropped = dropped_;
        s.ids.assign(ids_.begin(), ids_.end());
        return s;
    }

    // The records matching `q`, oldest first, at most q.limit.
    std::vector<nickel::packets::Record> Find(const Query& q) {
        std::lock_guard<std::mutex> lock(guard_);
        std::vector<nickel::packets::Record> out;
        const std::set<int> ids(q.ids.begin(), q.ids.end());
        for (const auto& r : records_) {
            if (out.size() >= q.limit) {
                break;
            }
            if (r.seq < q.fromSeq || (q.direction > 0 && !r.outgoing) || (q.direction < 0 && r.outgoing)
                || (!ids.empty() && !ids.count(r.id))) {
                continue;
            }
            out.push_back(r);
        }
        return out;
    }

private:
    bool CapturingLocked(int64_t nowMs) {
        if (capturing_ && nowMs >= untilMs_) {
            capturing_ = false;
        }
        return capturing_;
    }

    std::mutex guard_;
    std::deque<nickel::packets::Record> records_;
    size_t totalBytes_ = 0;
    uint64_t seen_ = 0, dropped_ = 0, nextSeq_ = 1;
    int64_t startMs_ = 0, untilMs_ = 0;
    std::set<int> ids_;
    bool capturing_ = false;
};

// What the current server session showed, kept from the moment the player joins (Nickel AI's get_server_info): how
// many packets of each id came and went, and the latest copies of the packets that describe the server (StartGame,
// the item registry with custom items, resource packs, commands, player list, scoreboard, forms...). A new server
// connection starts a new session.
constexpr size_t kMaxKeptBytes = 64 * 1024;

inline bool KeptPacket(int id, size_t* copies) {
    switch (id) {
        case 11: case 6: case 7: case 162: case 76: case 72: case 62: case 103: case 198: case 330: case 119:
        case 107: case 74:
            *copies = 1; return true;     // StartGame, packs, item registry, commands, game rules, game mode, server
                                          // settings, camera presets, data store, actor ids, objective, boss bar
        case 63: case 108: case 88: case 100: case 186:
            *copies = 8; return true;     // player list, scores, titles, forms, toasts: the latest few
        default:
            return false;
    }
}

struct SessionView {
    bool active = false;
    int64_t sinceMs = 0;                        // since the session started
    std::vector<std::pair<int, uint64_t>> in;   // id, count received
    std::vector<std::pair<int, uint64_t>> out;  // id, count sent
    std::vector<nickel::packets::Record> kept;  // oldest first
};

class Session {
public:
    // A new connection (or none: peer 0) clears everything.
    void Begin(uint64_t peer, int64_t nowMs) {
        std::lock_guard<std::mutex> lock(guard_);
        if (peer == peer_) {
            return;
        }
        peer_ = peer;
        startMs_ = nowMs;
        std::fill(std::begin(in_), std::end(in_), 0);
        std::fill(std::begin(out_), std::end(out_), 0);
        kept_.clear();
        seq_ = 1;
    }

    void Record(int64_t nowMs, bool outgoing, const unsigned char* data, size_t size) {
        int id = -1;
        size_t header = 0;
        if (!data || !size || !ReadHeader(data, size, &id, &header)) {
            return;
        }
        std::lock_guard<std::mutex> lock(guard_);
        if (!peer_) {
            return;
        }
        (outgoing ? out_ : in_)[id & 0x3ff] += 1;
        size_t copies = 0;
        if (outgoing || !KeptPacket(id, &copies)) {
            return;
        }
        nickel::packets::Record r;
        r.seq = seq_++;
        r.atMs = nowMs - startMs_;
        r.id = id;
        r.size = static_cast<uint32_t>(std::min<size_t>(size, UINT32_MAX));
        const size_t keep = std::min(size - header, kMaxKeptBytes);
        r.truncated = keep < size - header;
        r.payload.assign(reinterpret_cast<const char*>(data + header), keep);
        auto& list = kept_[id];
        list.push_back(std::move(r));
        while (list.size() > copies) {
            list.pop_front();
        }
    }

    SessionView View(int64_t nowMs) {
        std::lock_guard<std::mutex> lock(guard_);
        SessionView v;
        v.active = peer_ != 0;
        v.sinceMs = peer_ ? nowMs - startMs_ : 0;
        for (int id = 0; id < 1024; ++id) {
            if (in_[id]) v.in.push_back({id, in_[id]});
            if (out_[id]) v.out.push_back({id, out_[id]});
        }
        auto byCount = [](const auto& a, const auto& b) { return a.second > b.second; };
        std::sort(v.in.begin(), v.in.end(), byCount);
        std::sort(v.out.begin(), v.out.end(), byCount);
        for (const auto& [id, list] : kept_) {
            v.kept.insert(v.kept.end(), list.begin(), list.end());
        }
        std::sort(v.kept.begin(), v.kept.end(), [](const auto& a, const auto& b) { return a.seq < b.seq; });
        return v;
    }

private:
    std::mutex guard_;
    uint64_t peer_ = 0, seq_ = 1;
    int64_t startMs_ = 0;
    uint64_t in_[1024] = {}, out_[1024] = {};
    std::map<int, std::deque<nickel::packets::Record>> kept_;
};

inline Session& SharedSession() {
    static auto* session = new Session();
    return *session;
}

// The capture of the running game (game_packet_capture.hpp feeds it; ai_menu.hpp gives it to the tools).
inline Capture& Shared() {
    static auto* capture = new Capture();
    return *capture;
}

} // namespace nickel::packets

#endif // NICKEL_PACKET_CAPTURE_HPP
