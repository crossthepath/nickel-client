#pragma once
// Server-Sent Events parser (the "text/event-stream" format of the HTML standard, section 9.2.6), fed with chunks of
// bytes as they arrive. Used by the streaming mode of https_client.hpp (Nickel AI answers). Pure: no network, no
// Windows API; tests/https_client_checks.cpp feeds it byte by byte and in odd chunks.
//
// What the standard says and this parser does:
// - lines end with CRLF, LF or CR, and a CRLF may be split across two chunks;
// - a UTF-8 byte order mark at the start of the stream is skipped;
// - a line starting with ':' is a comment (keep-alive), ignored;
// - "field: value" (one space after the colon is removed), or "field" alone with an empty value;
// - data lines are joined with '\n'; event sets the type; id sets the last event id (unless it contains NUL);
//   retry sets the reconnection time when it is digits only; other fields are ignored;
// - an empty line dispatches the event, unless no data line was seen; the type defaults to "message";
// - an event still open when the stream ends is discarded (the caller does not call Feed again).
// One limit the standard does not have: a line or an event larger than maxEventBytes stops the parser (Failed()), so
// a server cannot make the client buffer without bound.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace nickel::net {

struct SseEvent {
    std::string type;  // "message" when the event had no event field
    std::string data;
    std::string id;    // last event id at dispatch time
};

class SseParser {
public:
    explicit SseParser(size_t maxEventBytes = 1024 * 1024) : maxEventBytes_(maxEventBytes) {}

    // Feeds the next bytes of the stream; onEvent(const SseEvent&) is called for each complete event, and returns
    // false to stop reading (the remaining bytes of the chunk are then ignored). Returns false once the parser has
    // failed or the callback asked to stop.
    template <typename OnEvent>
    bool Feed(std::string_view chunk, OnEvent&& onEvent) {
        if (failed_ || stopped_) {
            return false;
        }
        for (const char c : chunk) {
            if (!Byte(c, onEvent)) {
                return false;
            }
        }
        return true;
    }

    bool Failed() const { return failed_; }
    const std::string& LastEventId() const { return lastEventId_; }
    int64_t RetryMs() const { return retryMs_; }  // -1 until the server sends a retry field

private:
    template <typename OnEvent>
    bool Byte(char c, OnEvent& onEvent) {
        if (bomState_ < 3) {
            static const char kBom[] = "\xEF\xBB\xBF";
            if (c == kBom[bomState_]) {
                ++bomState_;
                return true;
            }
            // Not a BOM after all: the bytes already matched were text.
            const int matched = bomState_;
            bomState_ = 3;
            for (int i = 0; i < matched; ++i) {
                if (!Byte(kBom[i], onEvent)) {
                    return false;
                }
            }
        }
        if (afterCr_) {
            afterCr_ = false;
            if (c == '\n') {
                return true;  // the LF of a CRLF whose CR already ended the line
            }
        }
        if (c == '\r' || c == '\n') {
            afterCr_ = c == '\r';
            return EndLine(onEvent);
        }
        line_ += c;
        if (line_.size() + data_.size() > maxEventBytes_) {
            failed_ = true;
            return false;
        }
        return true;
    }

    template <typename OnEvent>
    bool EndLine(OnEvent& onEvent) {
        if (line_.empty()) {
            return Dispatch(onEvent);
        }
        if (line_[0] != ':') {
            Field();
        }
        line_.clear();
        if (data_.size() > maxEventBytes_) {
            failed_ = true;
            return false;
        }
        return true;
    }

    void Field() {
        const size_t colon = line_.find(':');
        const std::string_view line(line_);
        const std::string_view name = line.substr(0, colon);
        std::string_view value;
        if (colon != std::string::npos) {
            value = line.substr(colon + 1);
            if (!value.empty() && value[0] == ' ') {
                value.remove_prefix(1);
            }
        }
        if (name == "data") {
            data_.append(value.data(), value.size());
            data_ += '\n';
            hasData_ = true;
        } else if (name == "event") {
            type_.assign(value.data(), value.size());
        } else if (name == "id") {
            if (value.find('\0') == std::string_view::npos) {
                lastEventId_.assign(value.data(), value.size());
            }
        } else if (name == "retry") {
            SetRetry(value);
        }
    }

    void SetRetry(std::string_view value) {
        if (value.empty() || value.size() > 12) {
            return;
        }
        int64_t ms = 0;
        for (const char c : value) {
            if (c < '0' || c > '9') {
                return;
            }
            ms = ms * 10 + (c - '0');
        }
        retryMs_ = ms;
    }

    template <typename OnEvent>
    bool Dispatch(OnEvent& onEvent) {
        if (!hasData_) {
            type_.clear();
            return true;
        }
        SseEvent event;
        event.type = type_.empty() ? "message" : type_;
        event.data = std::move(data_);
        event.data.pop_back();  // the '\n' after the last data line
        event.id = lastEventId_;
        data_.clear();
        type_.clear();
        hasData_ = false;
        if (!onEvent(static_cast<const SseEvent&>(event))) {
            stopped_ = true;
            return false;
        }
        return true;
    }

    size_t maxEventBytes_;
    std::string line_;
    std::string data_;
    std::string type_;
    std::string lastEventId_;
    int64_t retryMs_ = -1;
    int bomState_ = 0;  // 0-2: bytes of a leading BOM matched so far; 3: past the start of the stream
    bool afterCr_ = false;
    bool hasData_ = false;
    bool failed_ = false;
    bool stopped_ = false;
};

} // namespace nickel::net
