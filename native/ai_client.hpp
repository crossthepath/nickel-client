#pragma once
// Nickel AI's network client: the protocol of https://nickelclient.com/api/ai/v1, for the assistant panel.
// Tests without the game: tests/ai_client_checks.cpp, against a local SSE server on 127.0.0.1.
//
// - The server runs the conversation (prompt, model, history); the client sends the player's message, streams the
//   answer (Server-Sent Events), runs the tools the model asks for and posts their results, which continues the same
//   turn. The messages kept here are for display only and are never sent back.
// - Every request runs on one background thread (net::Worker). The render thread calls Send, SubmitToolResults and
//   Cancel (never blocking), and reads Snap(): an immutable Conversation behind shared_ptr, rebuilt by the worker at
//   each event. Messages are shared_ptr too, so a snapshot copies pointers, not texts.
// - No account: "Authorization: Install <base64url key>" comes from an IdentitySource, read on the worker thread at
//   each request. The real one wraps PUBLIC's InstallIdentity and creates the key on the first AI request. The key is
//   never logged, never kept in a snapshot and never shown.
// - Hosts: nickelclient.com over HTTPS only. NICKEL_AI_LOCAL_SERVER (an http://127.0.0.1:<port> origin, loopback only)
//   points the client at a local test server, like PUBLIC's NICKEL_PUBLIC_LOCAL_SERVER.
// - 429: Retry-After (or the problem's retry_after) blocks Send, which repeats that error, until it has elapsed; 401
//   and 403 end the request with their own error kinds. Caps on each event, the whole stream, each answer's text, the
//   tool inputs and the history.
// - This layer never runs a tool and never asks the player anything: it hands the tool calls to the caller
//   (Conversation::pendingToolUses) and posts what the caller answers, once per tool call.
// Nothing here reads or writes files and nothing runs in DllMain: Stop() (or the destructor) cancels and joins the
// worker from the overlay's unload path.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ai_types.hpp"  // ToolUse, ToolResult, kMaxMessageChars, kMaxToolResultBytes (shared with the panel)
#include "https_client.hpp"
#include "json_value.hpp"
#include "public_identity.hpp"

namespace nickel::ai {

constexpr size_t kMaxIdChars = 128;                // turn, conversation and tool_use ids
constexpr size_t kMaxToolNameChars = 64;

// The header value for a request, "Install <base64url key>". Called on the worker thread before each request.
class IdentitySource {
public:
    virtual ~IdentitySource() = default;
    // False when there is no usable key (damaged, not creatable): nothing is sent.
    virtual bool Authorization(std::string* header) = 0;
};

// PUBLIC's install key (<Nickel data>\public\install.key), created on the first AI request when missing.
class InstallIdentitySource final : public IdentitySource {
public:
    explicit InstallIdentitySource(pub::InstallIdentity& identity) : identity_(identity) {}
    bool Authorization(std::string* header) override { return identity_.Authorization(header); }

private:
    pub::InstallIdentity& identity_;
};

// What the client says about itself in each new turn.
struct ClientInfo {
    std::string nickelVersion;
    std::string gameVersion;
    std::string language;  // the player's language ("fr", "en-US"...), so the answer uses it
};

struct ClientConfig {
    std::string baseUrl = "https://nickelclient.com/api/ai/v1";  // no trailing slash
    net::HostPolicy policy = net::HostPolicy{{"nickelclient.com"}, false};
    std::wstring userAgent = L"Nickel-Overlay";
    ClientInfo client;
    int timeoutMs = 60000;                  // connect, send, and the longest silence of a stream
    int defaultRetryAfterSeconds = 30;      // a 429 without Retry-After
    size_t maxEventBytes = 256 * 1024;      // one SSE event (a tool_use carrying a whole script fits)
    size_t maxStreamBytes = 4 * 1024 * 1024;// one response
    size_t maxAnswerBytes = 64 * 1024;      // the streamed text of one answer; the rest is dropped (truncated)
    size_t maxToolUsesPerStep = 16;
    size_t maxMessages = 200;               // history kept for display; the oldest go first
    // Optional log sink (worker thread, and the caller's thread for refusals): request lines and error kinds only,
    // never headers, bodies, or the key.
    std::function<void(const std::string&)> log;
};

// A local test server for trying Nickel AI in game without the real site (environment variable
// NICKEL_AI_LOCAL_SERVER, e.g. "http://127.0.0.1:8794"). Only the loopback address with a numeric port is accepted, so
// it can never send the key to another host. Returns false, leaving config unchanged, for anything else.
inline bool UseLocalTestServer(ClientConfig& config, const std::string& origin) {
    const std::string prefix = "http://127.0.0.1:";
    if (origin.rfind(prefix, 0) != 0 || origin.size() == prefix.size() || origin.size() > prefix.size() + 5) {
        return false;
    }
    unsigned port = 0;
    for (size_t i = prefix.size(); i < origin.size(); ++i) {
        if (origin[i] < '0' || origin[i] > '9') {
            return false;
        }
        port = port * 10 + static_cast<unsigned>(origin[i] - '0');
    }
    if (port == 0 || port > 65535) {
        return false;
    }
    config.baseUrl = origin + "/api/ai/v1";
    config.policy = net::HostPolicy{{}, true};
    return true;
}

// Applies NICKEL_AI_LOCAL_SERVER when it is set to an accepted origin.
inline bool UseLocalTestServerFromEnvironment(ClientConfig& config) {
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, "NICKEL_AI_LOCAL_SERVER") != 0 || value == nullptr) {
        return false;
    }
    const std::string origin(value);
    std::free(value);
    return UseLocalTestServer(config, origin);
}

enum class Status : uint8_t {
    Idle,           // nothing in flight: Send is possible
    Sending,        // a request is on its way, no event yet
    Streaming,      // events are arriving
    AwaitingTools,  // the server waits for the results of pendingToolUses (SubmitToolResults)
    Failed,         // the last request failed (error says why); Send is possible again
};

enum class ErrorKind : uint8_t {
    None,
    Cancelled,      // Cancel()
    NoIdentity,     // no usable install key: nothing was sent
    Invalid,        // 400, or a base URL the host policy refuses
    Unauthorized,   // 401: the install key was refused
    Blocked,        // 403: this install is blocked
    QuotaExceeded,  // 429 quota_exceeded, or an error event with that code
    RateLimited,    // 429 rate_limited (or any other 429), or an error event with that code
    Offline,        // DNS, connection or TLS failure
    Timeout,
    TooLarge,       // an event or the stream over its cap
    Malformed,      // an event or an answer that is not what the protocol says
    Interrupted,    // the stream ended without "done"
    Server,         // an "error" event (code says which), 5xx, or another status
};

inline const char* ErrorKindName(ErrorKind kind) {
    static const char* const kNames[] = {"none", "cancelled", "no install key", "invalid", "unauthorized", "blocked",
            "quota exceeded", "rate limited", "offline", "timeout", "too large", "malformed", "interrupted", "server"};
    const size_t index = static_cast<size_t>(kind);
    return index < sizeof(kNames) / sizeof(kNames[0]) ? kNames[index] : "unknown";
}

struct ErrorInfo {
    ErrorKind kind = ErrorKind::None;
    int httpStatus = 0;
    std::string code;       // the server's code (quota_exceeded, model_unavailable...), "" when none
    std::string message;    // the server's text for the player, or what failed (for the log)
    int retryAfter = 0;     // seconds, for RateLimited and QuotaExceeded

    bool Ok() const { return kind == ErrorKind::None; }
};

struct Quota {
    int64_t used = -1;      // -1 until the server says
    int64_t limit = -1;
    std::string resetAt;    // as the server wrote it
};

enum class Role : uint8_t { User, Assistant };

struct Message {
    Role role = Role::User;
    std::string text;
    std::string turnId;             // "" for a user message whose turn never started
    std::vector<ToolUse> toolUses;  // the tool calls of this answer step
    bool streaming = false;         // text is still arriving
    bool truncated = false;         // text went over ClientConfig::maxAnswerBytes; the rest was dropped
    bool interrupted = false;       // cancelled or failed before the step ended
};

struct Turn {
    std::string id;
    std::string stopReason;         // of the last step: end_turn, tool_use, max_tokens, refusal, limit ("" while open)
    int steps = 0;                  // streams of this turn (the first, then one per tool-results post)
    size_t toolUses = 0;
};

// What the render thread reads; never changes once published.
struct Conversation {
    uint64_t version = 0;
    std::string conversationId;                      // "" before the first turn event
    std::string turnId;                              // the current (or last) turn
    std::vector<Turn> turns;
    std::vector<std::shared_ptr<const Message>> messages;
    std::vector<ToolUse> pendingToolUses;            // while status is AwaitingTools
    Quota quota;
    Status status = Status::Idle;
    ErrorInfo error;
    std::string stopReason;                          // of the last step

    bool CanSend() const { return status == Status::Idle || status == Status::Failed; }
};

enum class SubmitError : uint8_t {
    None,
    NotAwaiting,      // no tool call is waiting
    WrongTurn,        // turn_id is not the current turn
    Missing,          // a pending tool call has no result
    Unknown,          // a result for an id that is not pending
    Duplicate,        // two results for one id
    ContentTooLarge,  // over kMaxToolResultBytes
    ContentNotJson,   // the content is not JSON text
    Stopped,          // after Stop()
};

namespace detail {

// Code points of valid UTF-8 text, or SIZE_MAX when it is not valid UTF-8.
inline size_t Utf8Chars(std::string_view text) {
    size_t chars = 0;
    for (size_t i = 0; i < text.size();) {
        const size_t length = json::detail::Utf8SequenceLength(text, i);
        if (length == 0) {
            return SIZE_MAX;
        }
        i += length;
        ++chars;
    }
    return chars;
}

// The longest prefix of valid UTF-8 `text` that fits in `bytes` without cutting a code point.
inline size_t Utf8Prefix(std::string_view text, size_t bytes) {
    if (bytes >= text.size()) {
        return text.size();
    }
    size_t end = bytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        --end;
    }
    return end;
}

// Ids go into URL paths: [A-Za-z0-9_.-], 1 to kMaxIdChars, starting with a letter or a digit (so never a dot
// segment such as "." or "..", which a proxy would resolve to another path).
inline bool ValidId(std::string_view id) {
    if (id.empty() || id.size() > kMaxIdChars) {
        return false;
    }
    const char first = id.front();
    if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || (first >= '0' && first <= '9'))) {
        return false;
    }
    for (const char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'
                || c == '-' || c == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

inline bool ValidToolName(std::string_view name) {
    if (name.empty() || name.size() > kMaxToolNameChars) {
        return false;
    }
    for (const char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

inline bool BlankText(std::string_view text) {
    for (const char c : text) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            return false;
        }
    }
    return true;
}

inline int RetryAfterHeader(const net::Response& response) {
    const std::string* header = response.FindHeader("Retry-After");
    if (header == nullptr || header->empty() || header->size() > 6) {
        return 0;
    }
    int seconds = 0;
    for (const char c : *header) {
        if (c < '0' || c > '9') {
            return 0;
        }
        seconds = seconds * 10 + (c - '0');
    }
    return seconds;
}

// A short printable string from a JSON member ("" when absent, not a string, or too long).
inline std::string ShortString(const json::Value& object, std::string_view key, size_t maxBytes) {
    const json::Value* value = object.Find(key);
    if (value == nullptr || !value->IsString() || value->AsString().size() > maxBytes) {
        return std::string();
    }
    return value->AsString();
}

inline bool ReadQuota(const json::Value& value, Quota* out) {
    if (!value.IsObject()) {
        return false;
    }
    const json::Value* used = value.Find("used");
    const json::Value* limit = value.Find("limit");
    int64_t usedValue = 0;
    int64_t limitValue = 0;
    if (used == nullptr || limit == nullptr || !used->GetInt64(&usedValue) || !limit->GetInt64(&limitValue)
            || usedValue < 0 || limitValue < 0) {
        return false;
    }
    Quota quota;
    quota.used = usedValue;
    quota.limit = limitValue;
    const json::Value* reset = value.Find("reset_at");
    if (reset == nullptr) {
        reset = value.Find("resets_at");
    }
    if (reset != nullptr && reset->IsString() && reset->AsString().size() <= 64) {
        quota.resetAt = reset->AsString();
    } else if (int64_t seconds = 0; reset != nullptr && reset->GetInt64(&seconds)) {
        quota.resetAt = std::to_string(seconds);
    }
    *out = std::move(quota);
    return true;
}

}  // namespace detail

class AssistantClient {
public:
    // identity: the install key (nullptr: every request ends as ErrorKind::NoIdentity). It must outlive the client.
    explicit AssistantClient(ClientConfig config = ClientConfig(), IdentitySource* identity = nullptr)
        : config_(std::move(config)), identity_(identity),
          http_(net::ClientOptions{config_.userAgent, config_.policy}) {
        while (!config_.baseUrl.empty() && config_.baseUrl.back() == '/') {
            config_.baseUrl.pop_back();
        }
        net::Url url;
        usable_ = net::CheckUrl(config_.policy, config_.baseUrl + "/turns", &url) == net::Error::None;
        current_ = std::make_shared<Conversation>();
    }

    ~AssistantClient() { Stop(); }

    AssistantClient(const AssistantClient&) = delete;
    AssistantClient& operator=(const AssistantClient&) = delete;

    // False when the base URL is not allowed by the host policy: every request then ends as ErrorKind::Invalid.
    bool Usable() const { return usable_; }

    // Cancels the request in flight and joins the worker; requests made afterwards are refused.
    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
            ++generation_;
        }
        worker_.Stop();
    }

    // Bumped by every published snapshot: copy one only when it changed.
    uint64_t Version() const { return version_.load(); }

    std::shared_ptr<const Conversation> Snap() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return current_;
    }

    // Seconds left of a Retry-After, or 0.
    int RateLimitRemaining() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return RemainingLocked();
    }

    // The client info of the next turns (the language may change in Settings).
    void SetClientInfo(ClientInfo info) {
        std::lock_guard<std::mutex> lock(mutex_);
        config_.client = std::move(info);
    }

    // Starts a turn with the player's message (1 to 4000 code points of UTF-8, not blank). False when it was refused
    // at once: busy (a turn in flight or waiting for tools: the snapshot is unchanged), a bad message (unchanged), or a
    // Retry-After still running, the base URL refused, or Stop() (the snapshot's error says why).
    bool Send(const std::string& message) {
        const size_t chars = detail::Utf8Chars(message);
        if (chars == 0 || chars == SIZE_MAX || chars > kMaxMessageChars || detail::BlankText(message)) {
            return false;
        }
        std::string body;
        uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_ || !current_->CanSend()) {
                return false;
            }
            auto next = std::make_shared<Conversation>(*current_);
            next->error = ErrorInfo();
            next->pendingToolUses.clear();
            next->stopReason.clear();
            if (!usable_) {
                next->status = Status::Failed;
                next->error.kind = ErrorKind::Invalid;
                next->error.message = "the base URL is not allowed";
                PublishLocked(std::move(next));
                return false;
            }
            if (const int wait = RemainingLocked(); wait > 0) {
                // The error that started the wait, so a daily quota stays a daily quota with the server's text.
                next->status = Status::Failed;
                next->error = blockError_;
                next->error.retryAfter = wait;
                PublishLocked(std::move(next));
                return false;
            }
            auto user = std::make_shared<Message>();
            user->role = Role::User;
            user->text = message;
            AppendMessageLocked(*next, std::move(user));
            next->status = Status::Sending;
            body = TurnBody(next->conversationId, message);
            generation = ++generation_;
            toolPostUnanswered_ = false;
            PublishLocked(std::move(next));
        }
        Post(generation, "/turns", std::move(body), "");
        return true;
    }

    // Posts the results of every pending tool call of turnId (in any order), which continues the turn. Refused, with
    // nothing changed, unless each pending call has exactly one result. Content is JSON text; a refusal by the player
    // is is_error with {"error":"declined",...} (MakeErrorResult).
    SubmitError SubmitToolResults(const std::string& turnId, const std::vector<ToolResult>& results) {
        std::string body;
        uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_) {
                return SubmitError::Stopped;
            }
            if (current_->status != Status::AwaitingTools) {
                return SubmitError::NotAwaiting;
            }
            if (turnId != current_->turnId) {
                return SubmitError::WrongTurn;
            }
            const std::vector<ToolUse>& pending = current_->pendingToolUses;
            std::vector<bool> answered(pending.size(), false);
            for (const ToolResult& result : results) {
                size_t index = pending.size();
                for (size_t i = 0; i < pending.size(); ++i) {
                    if (pending[i].id == result.toolUseId) {
                        index = i;
                    }
                }
                if (index == pending.size()) {
                    return SubmitError::Unknown;
                }
                if (answered[index]) {
                    return SubmitError::Duplicate;
                }
                answered[index] = true;
                if (result.content.size() > kMaxToolResultBytes) {
                    return SubmitError::ContentTooLarge;
                }
                json::Value parsed;
                if (!json::Parse(result.content, &parsed)) {
                    return SubmitError::ContentNotJson;
                }
            }
            for (const bool done : answered) {
                if (!done) {
                    return SubmitError::Missing;
                }
            }
            json::Value list = json::Value::MakeArray();
            for (const ToolResult& result : results) {
                json::Value item = json::Value::MakeObject();
                item.Set("tool_use_id", json::Value::MakeString(result.toolUseId));
                item.Set("content", json::Value::MakeString(result.content));
                item.Set("is_error", json::Value::MakeBool(result.isError));
                list.Push(std::move(item));
            }
            json::Value root = json::Value::MakeObject();
            root.Set("results", std::move(list));
            body = json::Serialize(root);
            auto next = std::make_shared<Conversation>(*current_);
            next->pendingToolUses.clear();
            next->status = Status::Sending;
            next->error = ErrorInfo();
            if (!next->turns.empty() && next->turns.back().id == turnId) {
                next->turns.back().steps += 1;
                next->turns.back().stopReason.clear();
            }
            generation = ++generation_;
            toolPostUnanswered_ = true;  // until the server streams something back
            PublishLocked(std::move(next));
        }
        Post(generation, "/turns/" + turnId + "/tool-results", std::move(body), turnId);
        return SubmitError::None;
    }

    // Stops the turn in flight (the request is aborted; text already received stays, marked interrupted) or drops
    // the pending tool calls. Events arriving afterwards are ignored. Nothing happens when idle.
    // A streamed step that is aborted ends on the server too (it sees the client leave). A step whose tool calls are
    // still unanswered there (AwaitingTools, or a tool-results post the server has not answered yet) would keep the
    // server's conversation busy (409 conflict) until it expires, so the conversation id is dropped: the next message
    // opens a new conversation on the server. The history shown here is kept.
    void Cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        if (running_) {
            running_->Cancel();
            running_.reset();
        }
        const bool unanswered = toolPostUnanswered_;
        toolPostUnanswered_ = false;
        const Status status = current_->status;
        if (status == Status::Idle || status == Status::Failed) {
            return;
        }
        auto next = std::make_shared<Conversation>(*current_);
        EndStreamingLocked(*next, true);
        if (status == Status::AwaitingTools || unanswered) {
            next->conversationId.clear();
        }
        next->pendingToolUses.clear();
        next->status = Status::Idle;
        next->error = ErrorInfo();
        next->error.kind = ErrorKind::Cancelled;
        PublishLocked(std::move(next));
    }

    // Forgets the conversation (a new chat): cancels what is in flight, clears the history and the conversation id.
    // The quota is kept.
    void NewConversation() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        toolPostUnanswered_ = false;
        if (running_) {
            running_->Cancel();
            running_.reset();
        }
        auto next = std::make_shared<Conversation>();
        next->quota = current_->quota;
        PublishLocked(std::move(next));
    }

    // A tool result for an error: {"error": code, "message": message}, is_error true.
    static ToolResult MakeErrorResult(const std::string& toolUseId, const std::string& code,
            const std::string& message) {
        json::Value value = json::Value::MakeObject();
        value.Set("error", json::Value::MakeString(code));
        value.Set("message", json::Value::MakeString(message));
        ToolResult result;
        result.toolUseId = toolUseId;
        result.content = json::Serialize(value);
        result.isError = true;
        return result;
    }

private:
    // What one stream saw, kept by the worker job.
    struct StreamState {
        bool sawTurn = false;
        bool sawDone = false;
        bool failed = false;     // an error was published: stop reading
        bool sawEvent = false;   // any event, known or not: the server has taken the request
        std::string turnId;      // of the turn event, when one was seen
        std::vector<ToolUse> toolUses;
    };

    std::string TurnBody(const std::string& conversationId, const std::string& message) const {
        json::Value client = json::Value::MakeObject();
        client.Set("nickel_version", json::Value::MakeString(config_.client.nickelVersion));
        client.Set("game_version", json::Value::MakeString(config_.client.gameVersion));
        client.Set("language", json::Value::MakeString(config_.client.language));
        json::Value root = json::Value::MakeObject();
        root.Set("conversation_id",
                conversationId.empty() ? json::Value() : json::Value::MakeString(conversationId));
        root.Set("message", json::Value::MakeString(message));
        root.Set("client", std::move(client));
        return json::Serialize(root);
    }

    void Log(const std::string& line) const {
        if (config_.log) {
            try {
                config_.log(line);
            } catch (...) {
            }
        }
    }

    int RemainingLocked() const {
        const auto now = std::chrono::steady_clock::now();
        if (now >= blockedUntil_) {
            return 0;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(blockedUntil_ - now).count();
        return static_cast<int>((left + 999) / 1000);
    }

    void BlockForLocked(const ErrorInfo& error) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(error.retryAfter);
        if (until > blockedUntil_) {
            blockedUntil_ = until;
            blockError_ = error;
        }
    }

    void PublishLocked(std::shared_ptr<Conversation> next) {
        next->version = ++publishCount_;
        current_ = std::move(next);
        version_.store(publishCount_);
    }

    void AppendMessageLocked(Conversation& conversation, std::shared_ptr<const Message> message) const {
        conversation.messages.push_back(std::move(message));
        const size_t cap = config_.maxMessages > 0 ? config_.maxMessages : 1;
        if (conversation.messages.size() > cap) {
            conversation.messages.erase(conversation.messages.begin(),
                    conversation.messages.begin() + static_cast<std::ptrdiff_t>(conversation.messages.size() - cap));
        }
    }

    // The assistant message of the current step, created when the step has none yet.
    Message& StreamingMessageLocked(Conversation& conversation) const {
        if (!conversation.messages.empty()) {
            const Message& last = *conversation.messages.back();
            if (last.role == Role::Assistant && last.streaming) {
                auto copy = std::make_shared<Message>(last);
                Message& ref = *copy;
                conversation.messages.back() = std::move(copy);
                return ref;
            }
        }
        auto created = std::make_shared<Message>();
        created->role = Role::Assistant;
        created->turnId = conversation.turnId;
        created->streaming = true;
        Message& ref = *created;
        AppendMessageLocked(conversation, std::move(created));
        return ref;
    }

    void EndStreamingLocked(Conversation& conversation, bool interrupted) const {
        if (conversation.messages.empty()) {
            return;
        }
        const Message& last = *conversation.messages.back();
        if (last.role == Role::Assistant && last.streaming) {
            auto copy = std::make_shared<Message>(last);
            copy->streaming = false;
            copy->interrupted = interrupted;
            conversation.messages.back() = std::move(copy);
        }
    }

    // Publishes a failure of the request of `generation` (dropped when stale).
    // The conversation id is dropped, so the next message starts a new conversation on the server, when that
    // conversation is gone (expired, not found), full, or busy there: a 409 conflict, or a tool-results post that
    // failed before the server streamed anything back (its step may still wait for these results, and nothing here
    // posts them again).
    void Fail(uint64_t generation, ErrorInfo error) {
        Log(std::string("ai: request failed: ") + ErrorKindName(error.kind)
                + (error.code.empty() ? std::string() : " (" + error.code + ")"));
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != generation_) {
            return;
        }
        const bool unanswered = toolPostUnanswered_;
        toolPostUnanswered_ = false;
        auto next = std::make_shared<Conversation>(*current_);
        EndStreamingLocked(*next, true);
        next->pendingToolUses.clear();
        next->status = Status::Failed;
        if (unanswered || error.httpStatus == 409 || error.code == "conflict" || error.code == "conversation_full"
                || error.code == "conversation_expired" || error.code == "conversation_not_found"
                || error.code == "expired" || error.code == "turn_expired") {
            next->conversationId.clear();
        }
        if (error.retryAfter > 0
                && (error.kind == ErrorKind::RateLimited || error.kind == ErrorKind::QuotaExceeded)) {
            BlockForLocked(error);
        }
        next->error = std::move(error);
        PublishLocked(std::move(next));
    }

    static ErrorKind FromNet(net::Error error) {
        switch (error) {
            case net::Error::Cancelled:
                return ErrorKind::Cancelled;
            case net::Error::Timeout:
                return ErrorKind::Timeout;
            case net::Error::TooLarge:
            case net::Error::StreamFailed:
                return ErrorKind::TooLarge;
            case net::Error::BadUrl:
            case net::Error::HostNotAllowed:
            case net::Error::BadRequest:
                return ErrorKind::Invalid;
            case net::Error::Connect:
            case net::Error::Tls:
                return ErrorKind::Offline;
            case net::Error::NotEventStream:
                return ErrorKind::Malformed;
            default:
                return ErrorKind::Offline;
        }
    }

    // A problem+json answer (or any non-2xx) into an ErrorInfo.
    ErrorInfo FromProblem(const net::Response& response, Quota* quota, bool* hasQuota) const {
        ErrorInfo error;
        error.httpStatus = response.status;
        json::Value problem;
        json::Limits limits;
        limits.maxBytes = 64 * 1024;
        if (json::Parse(response.body, &problem, nullptr, limits) && problem.IsObject()) {
            error.code = detail::ShortString(problem, "code", 64);
            error.message = detail::ShortString(problem, "detail", 1000);
            if (error.message.empty()) {
                error.message = detail::ShortString(problem, "message", 1000);
            }
            if (error.message.empty()) {
                error.message = detail::ShortString(problem, "title", 1000);
            }
            int64_t seconds = 0;
            if (const json::Value* retry = problem.Find("retry_after"); retry != nullptr && retry->GetInt64(&seconds)
                    && seconds > 0) {
                error.retryAfter = static_cast<int>((std::min)(seconds, int64_t{86400}));
            }
            if (const json::Value* value = problem.Find("quota"); value != nullptr) {
                *hasQuota = detail::ReadQuota(*value, quota);
            }
        }
        switch (response.status) {
            case 400:
                error.kind = ErrorKind::Invalid;
                break;
            case 401:
                error.kind = ErrorKind::Unauthorized;
                break;
            case 403:
                error.kind = ErrorKind::Blocked;
                break;
            case 429:
                error.kind = error.code == "quota_exceeded" ? ErrorKind::QuotaExceeded : ErrorKind::RateLimited;
                break;
            default:
                error.kind = ErrorKind::Server;
                break;
        }
        if (response.status == 429) {
            const int header = detail::RetryAfterHeader(response);
            int wait = header > 0 ? header : error.retryAfter;
            if (wait <= 0) {
                wait = config_.defaultRetryAfterSeconds;
            }
            error.retryAfter = (std::min)(wait, 86400);
        }
        return error;
    }

    void Post(uint64_t generation, std::string path, std::string body, std::string turnId) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto token = worker_.Post([this, generation, path = std::move(path), body = std::move(body),
                                          turnId = std::move(turnId)](net::CancelToken& cancel) {
            Run(cancel, generation, path, body, turnId);
        });
        if (token == nullptr) {
            // Stopped meanwhile: the request never runs, so the snapshot must not stay in Sending.
            if (generation == generation_) {
                auto next = std::make_shared<Conversation>(*current_);
                next->status = Status::Idle;
                next->error = ErrorInfo();
                next->error.kind = ErrorKind::Cancelled;
                PublishLocked(std::move(next));
            }
            return;
        }
        if (generation == generation_) {
            running_ = std::move(token);
        } else {
            token->Cancel();
        }
    }

    // The worker job of one request: the stream of a new turn, or of a turn continued by tool results.
    void Run(net::CancelToken& cancel, uint64_t generation, const std::string& path, const std::string& body,
            const std::string& continuedTurn) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != generation_) {
                return;
            }
        }
        std::string authorization;
        if (identity_ == nullptr || !identity_->Authorization(&authorization)) {
            ErrorInfo error;
            error.kind = ErrorKind::NoIdentity;
            error.message = "no install key";
            Fail(generation, std::move(error));
            return;
        }
        net::Request request;
        request.method = "POST";
        request.url = config_.baseUrl + path;
        request.body = body;
        request.maxBytes = config_.maxStreamBytes;
        request.timeoutMs = config_.timeoutMs;
        request.headers.push_back({"Accept", "text/event-stream"});
        request.headers.push_back({"Content-Type", "application/json"});
        request.headers.push_back({"Authorization", std::move(authorization)});
        StreamState state;
        state.sawTurn = !continuedTurn.empty();  // a continued turn may or may not repeat its turn event
        net::Response response = http_.Stream(
                request,
                [&](const net::SseEvent& event) { return OnEvent(generation, continuedTurn, event, &state, &cancel); },
                &cancel);
        SecureZeroMemory(request.headers.back().value.data(), request.headers.back().value.size());
        Log("ai: POST " + (continuedTurn.empty() ? std::string("/turns") : std::string("/turns/{id}/tool-results"))
                + " -> " + std::to_string(response.status)
                + (response.error == net::Error::None ? std::string() : std::string(" ") + net::ErrorName(response.error)));
        if (state.failed || state.sawDone) {
            return;
        }
        if (response.error != net::Error::None) {
            if (response.error == net::Error::Cancelled && cancel.Cancelled()) {
                return;  // Cancel() or Stop() already published what the player sees
            }
            ErrorInfo error;
            error.kind = FromNet(response.error);
            error.httpStatus = response.status;
            error.message = net::ErrorName(response.error);
            Fail(generation, std::move(error));
            return;
        }
        if (response.status < 200 || response.status >= 300) {
            Quota quota;
            bool hasQuota = false;
            ErrorInfo error = FromProblem(response, &quota, &hasQuota);
            if (hasQuota) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (generation == generation_) {
                    auto next = std::make_shared<Conversation>(*current_);
                    next->quota = quota;
                    PublishLocked(std::move(next));
                }
            }
            Fail(generation, std::move(error));
            return;
        }
        if (!state.sawDone) {
            ErrorInfo error;
            error.kind = ErrorKind::Interrupted;
            error.httpStatus = response.status;
            error.message = "the stream ended before done";
            Fail(generation, std::move(error));
        }
    }

    // Stops the stream with a Malformed (or other) error.
    bool StreamError(uint64_t generation, StreamState* state, ErrorKind kind, std::string message) {
        state->failed = true;
        ErrorInfo error;
        error.kind = kind;
        error.httpStatus = 200;
        error.message = std::move(message);
        Fail(generation, std::move(error));
        return false;
    }

    // One SSE event; false stops reading.
    bool OnEvent(uint64_t generation, const std::string& continuedTurn, const net::SseEvent& event,
            StreamState* state, net::CancelToken* cancel) {
        if (cancel->Cancelled()) {
            return false;
        }
        if (state->sawDone) {
            return false;  // nothing is read after done
        }
        if (!state->sawEvent) {
            // The server answers: a step continued by tool results no longer waits for them there.
            state->sawEvent = true;
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation == generation_) {
                toolPostUnanswered_ = false;
            }
        }
        if (event.data.size() > config_.maxEventBytes) {
            return StreamError(generation, state, ErrorKind::TooLarge, "an event over the cap");
        }
        const std::string& type = event.type;
        const bool known = type == "turn" || type == "text" || type == "tool_use" || type == "done" || type == "error"
                || type == "quota";
        if (!known) {
            return true;  // newer servers may add events: ignored
        }
        json::Value data;
        json::Limits limits;
        limits.maxBytes = config_.maxEventBytes;
        limits.maxDepth = 64;
        if (!json::Parse(event.data, &data, nullptr, limits) || !data.IsObject()) {
            return StreamError(generation, state, ErrorKind::Malformed, "event data is not a JSON object: " + type);
        }
        if (type == "error") {
            ErrorInfo error;
            error.httpStatus = 200;
            error.code = detail::ShortString(data, "code", 64);
            error.message = detail::ShortString(data, "message", 1000);
            int64_t seconds = 0;
            if (const json::Value* retry = data.Find("retry_after"); retry != nullptr && retry->GetInt64(&seconds)
                    && seconds > 0) {
                error.retryAfter = static_cast<int>((std::min)(seconds, int64_t{86400}));
            }
            error.kind = error.code == "quota_exceeded" ? ErrorKind::QuotaExceeded
                    : error.code == "rate_limited"      ? ErrorKind::RateLimited
                                                        : ErrorKind::Server;
            state->failed = true;
            Fail(generation, std::move(error));
            return false;
        }
        if (type == "turn") {
            const std::string turnId = detail::ShortString(data, "turn_id", kMaxIdChars);
            const std::string conversationId = detail::ShortString(data, "conversation_id", kMaxIdChars);
            if (!detail::ValidId(turnId) || !detail::ValidId(conversationId)) {
                return StreamError(generation, state, ErrorKind::Malformed, "turn event without valid ids");
            }
            if (!continuedTurn.empty() && turnId != continuedTurn) {
                return StreamError(generation, state, ErrorKind::Malformed, "turn event of another turn");
            }
            if (continuedTurn.empty() && state->sawTurn && turnId != state->turnId) {
                return StreamError(generation, state, ErrorKind::Malformed, "a second turn in one stream");
            }
            Quota quota;
            const json::Value* quotaValue = data.Find("quota");
            const bool hasQuota = quotaValue != nullptr && detail::ReadQuota(*quotaValue, &quota);
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != generation_) {
                return false;
            }
            auto next = std::make_shared<Conversation>(*current_);
            next->conversationId = conversationId;
            next->turnId = turnId;
            next->status = Status::Streaming;
            if (hasQuota) {
                next->quota = quota;
            }
            if (continuedTurn.empty() && !state->sawTurn) {
                Turn turn;
                turn.id = turnId;
                turn.steps = 1;
                next->turns.push_back(turn);
                if (next->turns.size() > config_.maxMessages) {
                    next->turns.erase(next->turns.begin());
                }
                // The user message that started this turn.
                for (auto it = next->messages.rbegin(); it != next->messages.rend(); ++it) {
                    if ((*it)->role == Role::User) {
                        if ((*it)->turnId.empty()) {
                            auto copy = std::make_shared<Message>(**it);
                            copy->turnId = turnId;
                            *it = std::move(copy);
                        }
                        break;
                    }
                }
            }
            state->sawTurn = true;
            state->turnId = turnId;
            PublishLocked(std::move(next));
            return true;
        }
        if (type == "quota") {
            Quota quota;
            if (!detail::ReadQuota(data, &quota)) {
                return StreamError(generation, state, ErrorKind::Malformed, "quota event");
            }
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != generation_) {
                return false;
            }
            auto next = std::make_shared<Conversation>(*current_);
            next->quota = quota;
            PublishLocked(std::move(next));
            return true;
        }
        if (!state->sawTurn) {
            return StreamError(generation, state, ErrorKind::Malformed, type + " event before the turn event");
        }
        if (type == "text") {
            const json::Value* delta = data.Find("delta");
            if (delta == nullptr || !delta->IsString()) {
                return StreamError(generation, state, ErrorKind::Malformed, "text event without delta");
            }
            if (delta->AsString().empty()) {
                return true;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != generation_) {
                return false;
            }
            auto next = std::make_shared<Conversation>(*current_);
            next->status = Status::Streaming;
            Message& message = StreamingMessageLocked(*next);
            if (!message.truncated) {
                const std::string& text = delta->AsString();
                const size_t room = config_.maxAnswerBytes > message.text.size()
                        ? config_.maxAnswerBytes - message.text.size()
                        : 0;
                const size_t keep = detail::Utf8Prefix(text, room);
                message.text.append(text.data(), keep);
                message.truncated = keep < text.size();
            }
            PublishLocked(std::move(next));
            return true;
        }
        if (type == "tool_use") {
            ToolUse use;
            use.id = detail::ShortString(data, "id", kMaxIdChars);
            use.name = detail::ShortString(data, "name", kMaxToolNameChars);
            const json::Value* input = data.Find("input");
            if (!detail::ValidId(use.id) || !detail::ValidToolName(use.name) || input == nullptr || !input->IsObject()) {
                return StreamError(generation, state, ErrorKind::Malformed, "tool_use event");
            }
            for (const ToolUse& seen : state->toolUses) {
                if (seen.id == use.id) {
                    return StreamError(generation, state, ErrorKind::Malformed, "a repeated tool_use id");
                }
            }
            if (state->toolUses.size() >= config_.maxToolUsesPerStep) {
                return StreamError(generation, state, ErrorKind::TooLarge, "too many tool calls in one step");
            }
            use.input = *input;
            use.inputJson = json::Serialize(use.input);
            state->toolUses.push_back(use);
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != generation_) {
                return false;
            }
            auto next = std::make_shared<Conversation>(*current_);
            next->status = Status::Streaming;
            StreamingMessageLocked(*next).toolUses.push_back(std::move(use));
            if (!next->turns.empty()) {
                next->turns.back().toolUses += 1;
            }
            PublishLocked(std::move(next));
            return true;
        }
        // done
        const std::string reason = detail::ShortString(data, "stop_reason", 32);
        const bool validReason = reason == "end_turn" || reason == "tool_use" || reason == "max_tokens"
                || reason == "refusal" || reason == "limit";
        if (!validReason) {
            return StreamError(generation, state, ErrorKind::Malformed, "done without a known stop_reason");
        }
        if ((reason == "tool_use") != !state->toolUses.empty()) {
            return StreamError(generation, state, ErrorKind::Malformed,
                    reason == "tool_use" ? "done tool_use without tool calls" : "tool calls without done tool_use");
        }
        state->sawDone = true;
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != generation_) {
            return false;
        }
        auto next = std::make_shared<Conversation>(*current_);
        EndStreamingLocked(*next, false);
        next->stopReason = reason;
        if (!next->turns.empty()) {
            next->turns.back().stopReason = reason;
        }
        if (reason == "tool_use") {
            next->pendingToolUses = state->toolUses;
            next->status = Status::AwaitingTools;
        } else {
            next->status = Status::Idle;
        }
        running_.reset();
        PublishLocked(std::move(next));
        return false;  // the step is over: stop reading
    }

    ClientConfig config_;
    IdentitySource* identity_;
    net::HttpsClient http_;
    bool usable_ = false;

    mutable std::mutex mutex_;
    std::shared_ptr<const Conversation> current_;
    uint64_t publishCount_ = 0;
    std::atomic<uint64_t> version_{0};
    uint64_t generation_ = 0;                 // bumped by each request, Cancel, NewConversation and Stop
    std::shared_ptr<net::CancelToken> running_;
    std::chrono::steady_clock::time_point blockedUntil_{};
    ErrorInfo blockError_;                    // the 429 (or error event) that set blockedUntil_
    bool toolPostUnanswered_ = false;         // the request in flight posts tool results; no event came back yet
    bool stopped_ = false;

    net::Worker worker_;  // last: destroyed (and joined) first
};

}  // namespace nickel::ai
