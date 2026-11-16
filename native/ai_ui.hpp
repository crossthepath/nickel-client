// Nickel AI panel of the menu: the chat (player messages, the assistant's streamed answer), the quota line, the
// "thinking / using tools" indicator, error and offline states, and the confirmation cards of action tools.
//
// Two parts in one file:
// - the page logic (nickel::ai::ChatPage and helpers): no ImGui, no Windows API, checked by tests/ai_ui_checks.cpp on
//   a fake ChatClient and a fake ToolRunner. Included at the top of menu.cpp like any header.
// - the drawing (DrawAi, the Settings toggle, the sidebar icon): compiled only when NICKEL_AI_UI_DRAW is defined, which
//   menu.cpp does when it includes this file a second time inside its anonymous namespace, next to public_ui.hpp (it
//   reuses Label, UiButton, Animate and the theme).
//
// Rules the page enforces (docs/design/nickel-ai/permissions-and-safety.md, protocol in ai_types.hpp):
// - Nickel AI is off by default (Settings > Connection > Nickel AI (assistant)). While it is off nothing is created:
//   no client, no worker, no install key, no request, and the sidebar entry is hidden.
// - Read tools run at once; every action tool (settings, modules, scripts) waits for the player's Allow on a card drawn
//   from ToolRunner::Describe (the registry and the script library), never from the model's text. Decline, or no answer
//   within 2 minutes, sends the "declined" error and changes nothing.
// - Tools run only while the panel is visible: nothing changes behind the player's back.
// - A script write shows the diff and says the script is saved held (it does not run until the player starts it).
// - A player message is 1..4000 characters; the quota comes from the service ("3/25 today").

#ifndef NICKEL_AI_UI_LOGIC
#define NICKEL_AI_UI_LOGIC

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "ai_types.hpp"

namespace nickel::ai {

// ---------------------------------------------------------------------------------------------------------------
// Text helpers (UTF-8)

inline bool IsContinuationByte(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

// Unicode characters (code points) of valid UTF-8 text.
inline size_t CountChars(const std::string& text) {
    size_t count = 0;
    for (char c : text) {
        if (!IsContinuationByte(c)) {
            ++count;
        }
    }
    return count;
}

// Byte length of the first `chars` characters (the whole text when it is shorter).
inline size_t BytesOfChars(const std::string& text, size_t chars) {
    size_t seen = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (!IsContinuationByte(text[i])) {
            if (seen == chars) {
                return i;
            }
            ++seen;
        }
    }
    return text.size();
}

inline std::string KeepChars(const std::string& text, size_t chars) {
    return text.substr(0, BytesOfChars(text, chars));
}

inline bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

inline std::string Trim(const std::string& text) {
    size_t from = 0;
    size_t to = text.size();
    while (from < to && IsSpace(text[from])) {
        ++from;
    }
    while (to > from && IsSpace(text[to - 1])) {
        --to;
    }
    return text.substr(from, to - from);
}

// ---------------------------------------------------------------------------------------------------------------
// Line diff of a script write (current file -> new source)

struct DiffLine {
    enum class Kind { Same, Added, Removed };
    Kind kind = Kind::Same;
    std::string text;
};

inline std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t from = 0;
    while (from < text.size()) {
        size_t end = text.find('\n', from);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(from, end - from);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
        from = end + 1;
    }
    return lines;
}

// Common prefix and suffix kept as Same; the middle is an LCS diff when it is small enough (scripts are at most
// 64 KB), otherwise every old middle line removed then every new one added.
inline std::vector<DiffLine> LineDiff(const std::string& before, const std::string& after) {
    const std::vector<std::string> a = SplitLines(before);
    const std::vector<std::string> b = SplitLines(after);
    size_t prefix = 0;
    while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix]) {
        ++prefix;
    }
    size_t suffix = 0;
    while (suffix < a.size() - prefix && suffix < b.size() - prefix
           && a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix]) {
        ++suffix;
    }
    std::vector<DiffLine> out;
    for (size_t i = 0; i < prefix; ++i) {
        out.push_back({DiffLine::Kind::Same, a[i]});
    }
    const size_t n = a.size() - prefix - suffix;
    const size_t m = b.size() - prefix - suffix;
    if (n > 0 && m > 0 && n * m <= 1000000) {
        // lengths of the LCS of a[prefix+i..] and b[prefix+j..]
        std::vector<uint32_t> table((n + 1) * (m + 1), 0);
        auto at = [&](size_t i, size_t j) -> uint32_t& { return table[i * (m + 1) + j]; };
        for (size_t i = n; i-- > 0;) {
            for (size_t j = m; j-- > 0;) {
                at(i, j) = a[prefix + i] == b[prefix + j] ? at(i + 1, j + 1) + 1 : std::max(at(i + 1, j), at(i, j + 1));
            }
        }
        size_t i = 0;
        size_t j = 0;
        while (i < n && j < m) {
            if (a[prefix + i] == b[prefix + j]) {
                out.push_back({DiffLine::Kind::Same, a[prefix + i]});
                ++i;
                ++j;
            } else if (at(i + 1, j) >= at(i, j + 1)) {
                out.push_back({DiffLine::Kind::Removed, a[prefix + i]});
                ++i;
            } else {
                out.push_back({DiffLine::Kind::Added, b[prefix + j]});
                ++j;
            }
        }
        for (; i < n; ++i) {
            out.push_back({DiffLine::Kind::Removed, a[prefix + i]});
        }
        for (; j < m; ++j) {
            out.push_back({DiffLine::Kind::Added, b[prefix + j]});
        }
    } else {
        for (size_t i = 0; i < n; ++i) {
            out.push_back({DiffLine::Kind::Removed, a[prefix + i]});
        }
        for (size_t j = 0; j < m; ++j) {
            out.push_back({DiffLine::Kind::Added, b[prefix + j]});
        }
    }
    for (size_t i = a.size() - suffix; i < a.size(); ++i) {
        out.push_back({DiffLine::Kind::Same, a[i]});
    }
    return out;
}

// Bytes of a tool result's content once written as a JSON string in the POST body, which is what the service compares
// with kMaxToolResultBytes (quotes, backslashes and control characters count as their escapes). Never less than the
// service's count: invalid UTF-8 becomes a 6-byte \ufffd here and 3 bytes there.
inline size_t ResultWireSize(const std::string& content) {
    return json::Serialize(json::Value::MakeString(content)).size();
}

// ---------------------------------------------------------------------------------------------------------------
// Fixed texts (the panel never shows the model's text as a status, only as the assistant's message)

// "2026-11-17T00:00:00Z" -> seconds since 1970 (UTC); 0 when the text is not of that form.
inline int64_t ParseUtc(const std::string& text) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (text.size() < 19 || std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &minute,
                                        &second) != 6) {
        return 0;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) {
        return 0;
    }
    // days from civil (Howard Hinnant)
    const int y = year - (month <= 2 ? 1 : 0);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned shifted = static_cast<unsigned>(month + (month > 2 ? -3 : 9));
    const unsigned doy = (153 * shifted + 2) / 5 + static_cast<unsigned>(day) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
    return days * 86400 + hour * 3600 + minute * 60 + second;
}

// "Resets at 00:00 UTC." from the quota's reset_at; empty when it cannot be read.
inline std::string ResetText(const std::string& resetAt) {
    int hour = 0, minute = 0;
    if (ParseUtc(resetAt) == 0 || std::sscanf(resetAt.c_str() + 11, "%2d:%2d", &hour, &minute) != 2) {
        return "";
    }
    char text[48];
    std::snprintf(text, sizeof(text), "Resets at %02d:%02d UTC.", hour, minute);
    return text;
}

inline std::string ErrorText(const ClientError& error) {
    const std::string& c = error.code;
    if (c == "offline") {
        return "Nickel AI is offline. Check your connection and try again.";
    }
    if (c == "unauthorized") {
        return "Nickel AI could not verify this install. Try again in a moment.";
    }
    if (c == "blocked") {
        return "Nickel AI is not available for this install.";
    }
    if (c == "quota_exceeded") {
        return "You used all your Nickel AI messages for today.";
    }
    if (c == "rate_limited") {
        return "Too many messages at once. Wait a moment before sending another.";
    }
    if (c == "invalid") {
        return "The service refused this message.";
    }
    if (c == "model_unavailable" || c == "unavailable") {
        return "Nickel AI is busy right now. Try again in a moment.";
    }
    if (c == "conversation_full") {
        return "This conversation is full. Start a new chat.";
    }
    if (c == "service_unavailable") {
        return "Nickel AI is offline for now. Try again later.";
    }
    if (c == "update_required") {
        return "Update Nickel to use Nickel AI.";
    }
    return "Something went wrong with Nickel AI" + (c.empty() ? std::string(".") : " (" + c + ").");
}

inline std::string StopText(StopReason stop) {
    switch (stop) {
        case StopReason::MaxTokens:
            return "The answer was cut: it reached its maximum length.";
        case StopReason::Refusal:
            return "Nickel AI can't help with that.";
        case StopReason::Limit:
            return "This request needed too many steps. Try a smaller one.";
        default:
            return "";
    }
}

inline std::string ToolActivity(const std::string& tool) {
    static const char* const kLabels[][2] = {
            {"search_modules", "Searching modules"},
            {"get_module", "Reading a module"},
            {"get_client_status", "Checking Nickel"},
            {"list_scripts", "Listing scripts"},
            {"read_script", "Reading a script"},
            {"validate_script", "Checking a script"},
            {"navigate_ui", "Opening a screen"},
            {"set_setting", "Changing a setting"},
            {"revert_changes", "Undoing changes"},
            {"set_module_enabled", "Switching a module"},
            {"write_script", "Writing a script"},
            {"set_script_running", "Starting or stopping a script"},
    };
    for (const auto& label : kLabels) {
        if (tool == label[0]) {
            return label[1];
        }
    }
    return "Using a tool";
}

// ---------------------------------------------------------------------------------------------------------------
// The page

// A card ignores Allow / Decline for this long after it appears or moves on screen, so the second click of a
// double-click (or a click meant for the card that just went away) cannot answer a card the player has not seen.
constexpr double kCardArmSeconds = 0.4;

// One tool_use of the current step and what became of it.
struct PendingCall {
    ToolUse use;
    bool action = false;  // needs a card
    bool carded = false;  // Describe accepted it: the card is shown
    bool decided = false; // `result` is final
    ToolResult result;
    ConfirmCard card;
    std::vector<DiffLine> diff; // write_script: computed once when the card opens, drawn every frame
    double shownAt = 0;
    double armedAt = 0;   // Allow / Decline count from this time on
    bool placed = false;  // PlaceCard was called at least once
    float placedX = 0;
    float placedY = 0;
};

// A line in the chat about a confirmed or refused change, placed after the message it followed.
struct ChatNote {
    enum class Kind { Applied, Declined, Failed };
    Kind kind = Kind::Applied;
    size_t afterMessages = 0;
    std::string text;
};

enum class Activity {
    Unavailable,      // no client in this build
    Idle,
    Thinking,         // waiting for the first words of the answer
    Writing,          // text is streaming
    UsingTools,       // tool calls are running or their results are on their way
    WaitingForPlayer, // a card waits for Allow / Decline
    Error,
};

class ChatPage {
public:
    ChatPage(ChatClient* client, ToolRunner* tools) : client_(client), tools_(tools) {}

    bool Available() const { return client_ != nullptr && tools_ != nullptr; }

    // Once per frame on the render thread. `now` is a monotonic clock in seconds, `utcNow` the wall clock (seconds
    // since 1970, UTC) for the quota reset. Tool calls only run while `visible`.
    void Tick(double now, int64_t utcNow, bool visible) {
        now_ = now;
        utcNow_ = utcNow;
        if (!Available()) {
            return;
        }
        Refresh();
        if (snap_.state != TurnState::AwaitingToolResults) {
            calls_.clear(); // cancelled or failed: nothing left to answer
            sendFailed_ = false;
            return;
        }
        if (snap_.toolRound != handledRound_) {
            handledRound_ = snap_.toolRound;
            calls_.clear();
            for (const auto& use : snap_.toolUses) {
                PendingCall call;
                call.use = use;
                call.action = IsActionTool(use.name);
                calls_.push_back(std::move(call));
            }
            sent_ = false;
            sendFailed_ = false;
            retrySendAt_ = 0;
        }
        if (!visible || sent_) {
            return;
        }
        for (auto& call : calls_) {
            if (call.decided) {
                continue;
            }
            if (!IsKnownTool(call.use.name)) {
                call.result =
                        MakeToolError(call.use.id, "unknown_tool", "Nickel has no tool named " + call.use.name + ".");
                call.decided = true;
            } else if (!call.action) {
                call.result = Checked(call.use.id, tools_->Run(call.use));
                call.decided = true;
            } else if (!call.carded) {
                ConfirmCard card;
                ToolResult refusal;
                if (tools_->Describe(call.use, &card, &refusal)) {
                    card.toolUseId = call.use.id;
                    card.tool = call.use.name;
                    if (card.title.empty()) {
                        card.title = ToolActivity(call.use.name);
                    }
                    if (card.isScriptWrite) {
                        call.diff = LineDiff(card.currentSource, card.newSource);
                    }
                    call.card = std::move(card);
                    call.carded = true;
                    call.shownAt = now;
                    call.armedAt = now + kCardArmSeconds;
                } else {
                    // checked like any other result: an empty or oversized refusal must not spoil the whole post
                    if (refusal.content.empty()) {
                        refusal = MakeToolError(call.use.id, "refused", "Nickel refused this call.");
                    }
                    refusal.isError = true;
                    call.result = Checked(call.use.id, std::move(refusal));
                    call.decided = true;
                }
            } else if (now - call.shownAt >= kConfirmTimeoutSeconds) {
                call.result = MakeToolError(call.use.id, "declined",
                                            "The player did not answer in time; nothing was changed.");
                call.decided = true;
                Note(ChatNote::Kind::Declined, "Not answered: " + call.card.title);
            }
        }
        if (!sent_ && now >= retrySendAt_) {
            FlushIfDone(); // also retries a post the client refused
        }
    }

    // Why the draft cannot be sent now; empty when it can.
    std::string SendBlockedReason(const std::string& draft) const {
        if (!Available()) {
            return "Nickel AI is not available in this build.";
        }
        if (Busy()) {
            return "Wait for the answer, or stop it.";
        }
        if (QuotaUsedUp()) {
            const std::string reset = ResetText(snap_.quota.resetAt);
            return "You used all " + std::to_string(snap_.quota.limit) + " messages for today."
                   + (reset.empty() ? "" : " " + reset);
        }
        if (now_ < retryUntil_) {
            const int seconds = static_cast<int>(retryUntil_ - now_ + 0.999);
            return "Try again in " + std::to_string(seconds) + (seconds == 1 ? " second." : " seconds.");
        }
        if (Trim(draft).empty()) {
            return "Type a message.";
        }
        if (CountChars(draft) > kMaxMessageChars) {
            return "A message is at most " + std::to_string(kMaxMessageChars) + " characters.";
        }
        return "";
    }

    bool CanSend(const std::string& draft) const { return SendBlockedReason(draft).empty(); }

    bool Send(const std::string& draft) {
        if (!CanSend(draft)) {
            return false;
        }
        if (!client_->StartTurn(Trim(draft))) {
            return false;
        }
        Refresh();
        return true;
    }

    bool Busy() const {
        return snap_.state == TurnState::Sending || snap_.state == TurnState::Streaming
               || snap_.state == TurnState::AwaitingToolResults;
    }

    // Stops the turn; open cards disappear unanswered (the turn they belong to is gone).
    void Cancel() {
        if (!Available()) {
            return;
        }
        client_->Cancel();
        calls_.clear();
        sendFailed_ = false;
        Refresh();
    }

    void NewChat() {
        if (!Available()) {
            return;
        }
        client_->NewConversation();
        calls_.clear();
        notes_.clear();
        handledRound_ = 0;
        sendFailed_ = false;
        Refresh();
    }

    // Where the panel drew a card this frame (screen pixels). A card that moved arms again for kCardArmSeconds.
    void PlaceCard(const std::string& toolUseId, float x, float y) {
        for (auto& call : calls_) {
            if (call.use.id != toolUseId || !call.carded || call.decided) {
                continue;
            }
            const bool moved = std::abs(call.placedX - x) > 0.5f || std::abs(call.placedY - y) > 0.5f;
            if (!call.placed || moved) {
                call.armedAt = std::max(call.armedAt, now_ + kCardArmSeconds);
            }
            call.placed = true;
            call.placedX = x;
            call.placedY = y;
        }
    }

    // False while a card is still arming: its buttons do nothing yet.
    bool Armed(const PendingCall& call) const { return now_ >= call.armedAt; }

    // The player's answer to a card. False when there is no open, armed card with this id.
    bool Allow(const std::string& toolUseId) {
        PendingCall* call = OpenCall(toolUseId);
        if (call == nullptr) {
            return false;
        }
        call->result = Checked(call->use.id, tools_->Run(call->use));
        call->decided = true;
        if (call->result.isError) {
            Note(ChatNote::Kind::Failed, "Could not apply: " + call->card.title);
        } else {
            const char* what = call->card.isScriptWrite ? "Saved (held): " : "Applied: ";
            Note(ChatNote::Kind::Applied, what + call->card.title);
        }
        FlushIfDone();
        return true;
    }

    bool Decline(const std::string& toolUseId) {
        PendingCall* call = OpenCall(toolUseId);
        if (call == nullptr) {
            return false;
        }
        call->result = MakeToolError(call->use.id, "declined", "The player declined; nothing was changed.");
        call->decided = true;
        Note(ChatNote::Kind::Declined, "Declined: " + call->card.title);
        FlushIfDone();
        return true;
    }

    // ---- what the panel draws

    const ConversationSnapshot& Conversation() const { return snap_; }

    const std::vector<PendingCall>& Calls() const { return calls_; }

    std::vector<const PendingCall*> OpenCards() const {
        std::vector<const PendingCall*> open;
        for (const auto& call : calls_) {
            if (call.carded && !call.decided) {
                open.push_back(&call);
            }
        }
        return open;
    }

    const std::vector<ChatNote>& Notes() const { return notes_; }

    // Seconds left before an open card declines itself.
    double SecondsLeft(const PendingCall& call) const {
        return std::max(0.0, kConfirmTimeoutSeconds - (now_ - call.shownAt));
    }

    Activity CurrentActivity() const {
        if (!Available()) {
            return Activity::Unavailable;
        }
        switch (snap_.state) {
            case TurnState::Failed:
                return Activity::Error;
            case TurnState::Sending:
                return Activity::Thinking;
            case TurnState::Streaming:
                return !snap_.messages.empty() && snap_.messages.back().role == ChatMessage::Role::Assistant
                               && !snap_.messages.back().text.empty() && !afterTools_
                           ? Activity::Writing
                           : Activity::Thinking;
            case TurnState::AwaitingToolResults:
                if (sendFailed_) {
                    return Activity::Error;
                }
                return OpenCards().empty() ? Activity::UsingTools : Activity::WaitingForPlayer;
            default:
                return Activity::Idle;
        }
    }

    // "Thinking...", "Searching modules...", "Waiting for your answer"; empty when idle.
    std::string ActivityText() const {
        switch (CurrentActivity()) {
            case Activity::Thinking:
                return "Thinking";
            case Activity::Writing:
                return "Writing";
            case Activity::UsingTools: {
                for (const auto& call : calls_) {
                    if (!call.decided) {
                        return ToolActivity(call.use.name);
                    }
                }
                return calls_.empty() ? "Using tools" : ToolActivity(calls_.front().use.name);
            }
            case Activity::WaitingForPlayer:
                return OpenCards().size() == 1 ? "Waiting for your answer" : "Waiting for your answers";
            default:
                return "";
        }
    }

    // "3/25 today"; before the first answer of the session, the default limit.
    std::string QuotaLine() const {
        if (!snap_.quota.known) {
            return std::to_string(kDefaultDailyLimit) + " messages a day";
        }
        return std::to_string(snap_.quota.used) + "/" + std::to_string(snap_.quota.limit) + " today";
    }

    bool QuotaUsedUp() const {
        if (!snap_.quota.known || snap_.quota.used < snap_.quota.limit) {
            return false;
        }
        const int64_t reset = ParseUtc(snap_.quota.resetAt);
        return reset == 0 || utcNow_ < reset; // past the reset, the service decides again
    }

    // The error box under the conversation, empty when there is none.
    std::string ErrorLine() const {
        if (snap_.state == TurnState::Failed) {
            return ErrorText(snap_.error);
        }
        if (snap_.state == TurnState::Idle) {
            return StopText(snap_.lastStop);
        }
        if (snap_.state == TurnState::AwaitingToolResults && sendFailed_) {
            return "Nickel AI could not send the results of its tools. Retrying; press Stop to give up.";
        }
        return "";
    }

private:
    void Refresh() {
        const uint64_t revision = client_->Revision();
        if (revision == snap_.revision && revision != 0) {
            return;
        }
        snap_ = client_->Snapshot();
        // After a tool round the answer is "Thinking" again until new text arrives (in the same assistant message or
        // in a new one, whichever the client does).
        const size_t count = snap_.messages.size();
        const size_t textSize = count == 0 ? 0 : snap_.messages.back().text.size();
        if (snap_.state == TurnState::AwaitingToolResults) {
            afterTools_ = true;
            markCount_ = count;
            markText_ = textSize;
        } else if (snap_.state == TurnState::Idle || snap_.state == TurnState::Failed) {
            afterTools_ = false;
        } else if (afterTools_ && (count != markCount_ ? textSize > 0 : textSize > markText_)) {
            afterTools_ = false;
        }
        if (snap_.state == TurnState::Failed && snap_.revision != errorRevision_) {
            errorRevision_ = snap_.revision;
            int wait = snap_.error.retryAfterSeconds;
            if (wait <= 0 && (snap_.error.code == "rate_limited" || snap_.error.code == "quota_exceeded")) {
                wait = 60;
            }
            if (wait > 0) {
                retryUntil_ = now_ + wait;
            }
        }
    }

    PendingCall* OpenCall(const std::string& id) {
        if (sent_) {
            return nullptr;
        }
        for (auto& call : calls_) {
            if (call.use.id == id && call.carded && !call.decided && Armed(call)) {
                return &call;
            }
        }
        return nullptr;
    }

    // A result above the protocol's 16 KB is replaced by an error rather than cut into invalid JSON. The size is the
    // one the service measures: the content as a JSON string (quotes and escapes included).
    static ToolResult Checked(const std::string& id, ToolResult result) {
        result.toolUseId = id;
        if (ResultWireSize(result.content) > kMaxToolResultBytes) {
            return MakeToolError(id, "result_too_large", "The result was larger than 16 KB.");
        }
        if (result.content.empty()) {
            result.content = "{}";
        }
        return result;
    }

    void Note(ChatNote::Kind kind, std::string text) {
        notes_.push_back({kind, snap_.messages.size(), std::move(text)});
    }

    // All results of the step go back in one POST, in the order of the tool_use events.
    void FlushIfDone() {
        if (sent_ || calls_.empty()) {
            return;
        }
        for (const auto& call : calls_) {
            if (!call.decided) {
                return;
            }
        }
        std::vector<ToolResult> results;
        results.reserve(calls_.size());
        for (const auto& call : calls_) {
            results.push_back(call.result);
        }
        // A refused post (the client's state moved on, its worker is gone) is kept and tried again each second;
        // the page says so instead of waiting silently.
        sent_ = client_->SendToolResults(results);
        sendFailed_ = !sent_;
        retrySendAt_ = sent_ ? 0 : now_ + 1;
        Refresh();
    }

    ChatClient* client_ = nullptr;
    ToolRunner* tools_ = nullptr;
    ConversationSnapshot snap_;
    std::vector<PendingCall> calls_;
    std::vector<ChatNote> notes_;
    uint64_t handledRound_ = 0;
    uint64_t errorRevision_ = 0;
    bool sent_ = false;
    bool sendFailed_ = false;
    double retrySendAt_ = 0;
    bool afterTools_ = false;
    size_t markCount_ = 0;
    size_t markText_ = 0;
    double now_ = 0;
    int64_t utcNow_ = 0;
    double retryUntil_ = 0;
};

// What the overlay needs to build the panel: set once by the code that owns the real client and tools (the AI
// integration), before the player first opens the panel. Without it the panel says Nickel AI is not available.
struct Services {
    std::unique_ptr<ChatClient> client;
    std::unique_ptr<ToolRunner> tools;
};
using ServicesFactory = Services (*)();

inline ServicesFactory& Factory() {
    static ServicesFactory factory = nullptr;
    return factory;
}

} // namespace nickel::ai

#endif // NICKEL_AI_UI_LOGIC


// =================================================================================================================
// Drawing: included a second time by menu.cpp inside its anonymous namespace, with NICKEL_AI_UI_DRAW defined.
#if defined(NICKEL_AI_UI_DRAW) && !defined(NICKEL_AI_UI_DRAWN)
#define NICKEL_AI_UI_DRAWN

constexpr int kAiTab = 13;

// Settings > Connection > Nickel AI (assistant). Off by default; kept in root\ai\settings.ini, not in the preferences
// ini, so loading or sharing a config cannot turn it on.
MenuOption aiAssistantOption = {"nickel-ai",
                                "Nickel AI (assistant)",
                                "Answers questions about Nickel and can change modules or write scripts after you "
                                "allow each change. Sends your messages to nickelclient.com. While off, nothing is "
                                "sent.",
                                false,
                                false};

MenuOption& AiOption() {
    return aiAssistantOption;
}

// %APPDATA%\Nickel\ai\settings.ini in the game (the folder of the preferences ini).
std::wstring AiSettingPath() {
    return configPath.substr(0, configPath.find_last_of(L"\\/")) + L"\\ai\\settings.ini";
}

struct AiMenu {
    bool settingRead = false;
    bool created = false;
    std::unique_ptr<nickel::ai::ChatClient> client;
    std::unique_ptr<nickel::ai::ToolRunner> tools;
    std::unique_ptr<nickel::ai::ChatPage> page;
    char input[nickel::ai::kMaxMessageChars * 4 + 1] = {};
    bool clearInput = false;
    bool focusInput = false;
    size_t seenItems = 0;
};

// Never destroyed, like PUBLIC's client: the overlay has no unload path, and a destructor at process exit would join the
// AI client's worker thread inside DllMain. Turning the option off (AiShutdown) stops it on the render thread.
AiMenu& Ai() {
    static AiMenu* menu = new AiMenu();
    return *menu;
}

// Drops the page, the tools and the client (its worker stops with it): once off, nothing more is sent.
void AiShutdown() {
    AiMenu& m = Ai();
    if (m.page) {
        m.page->Cancel();
    }
    m.page.reset();
    m.tools.reset();
    m.client.reset();
    m.created = false;
    m.input[0] = 0;
    m.seenItems = 0;
}

// The Settings toggle and Reset all are the only callers. Turning it off never creates the file.
void SetAiEnabled(bool on) {
    AiOption().value = on;
    const std::wstring path = AiSettingPath();
    const bool exists = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (on || exists) {
        CreateDirectoryW(path.substr(0, path.find_last_of(L"\\/")).c_str(), nullptr);
        WritePrivateProfileStringW(L"ai", L"enabled", on ? L"1" : L"0", path.c_str());
    }
    if (!on) {
        AiShutdown();
    }
}

// Called once per frame by DrawMenu: reads the option once, and makes sure nothing stays alive while it is off.
void AiTick() {
    AiMenu& m = Ai();
    if (configPath.empty()) {
        return;
    }
    if (!m.settingRead) {
        m.settingRead = true;
        AiOption().value = GetPrivateProfileIntW(L"ai", L"enabled", 0, AiSettingPath().c_str()) == 1;
    }
    if (!AiOption().value && m.created) {
        AiShutdown();
    }
}

// Sidebar icon: a four-point spark with a small cross beside it (stroked like theme.hpp's icons).
void DrawAiIcon(ImDrawList* d, ImVec2 lo, float size, ImU32 c) {
    const float k = size / 24.f;
    const float th = std::max(1.f, 1.7f * k);
    auto P = [&](float x, float y) {
        return ImVec2(lo.x + x * k, lo.y + y * k);
    };
    const ImVec2 spark[8] = {P(10, 3), P(12, 9), P(18, 11), P(12, 13), P(10, 19), P(8, 13), P(2, 11), P(8, 9)};
    d->AddPolyline(spark, 8, c, ImDrawFlags_Closed, th);
    d->AddLine(P(19, 3), P(19, 9), c, th);
    d->AddLine(P(16, 6), P(22, 6), c, th);
    d->AddCircleFilled(P(19, 19), 1.6f * k, c, 12);
}

// Height in pixels of wrapped text; `size` in pixels (already scaled, as for Label), `width` in pixels.
float AiTextHeight(ImFont* font, float size, const std::string& text, float width) {
    const float px = size * FontEmFactor;
    return Sized(font, px)->CalcTextSizeA(px, FLT_MAX, width, text.c_str(), text.c_str() + text.size()).y;
}

// Wrapped text drawn like Label; returns its height in pixels.
float AiText(ImDrawList* d, ImFont* font, float size, ImVec2 pos, const std::string& text, ImU32 color, float width) {
    const float px = size * FontEmFactor;
    ImFont* baked = Sized(font, px);
    const ImVec2 at(std::round(pos.x), std::round(pos.y - size * .1f));
    d->AddText(baked, px, at, color, text.c_str(), text.c_str() + text.size(), width);
    return AiTextHeight(font, size, text, width);
}

// Keeps the input at 4000 characters while typing, and clears it after a send (an active field keeps its own copy).
int AiInputCallback(ImGuiInputTextCallbackData* data) {
    AiMenu& m = Ai();
    if (m.clearInput) {
        m.clearInput = false;
        data->DeleteChars(0, data->BufTextLen);
        return 0;
    }
    if (data->EventFlag == ImGuiInputTextFlags_CallbackEdit) {
        const std::string text(data->Buf, static_cast<size_t>(data->BufTextLen));
        if (nickel::ai::CountChars(text) > nickel::ai::kMaxMessageChars) {
            const int keep = static_cast<int>(nickel::ai::BytesOfChars(text, nickel::ai::kMaxMessageChars));
            data->DeleteChars(keep, data->BufTextLen - keep);
        }
    }
    return 0;
}

// The arrow between a current and a new value on a card.
void AiArrow(ImDrawList* d, ImVec2 at, float s, ImU32 c) {
    d->AddLine(at, ImVec2(at.x + 10 * s, at.y), c, 1.4f * s);
    d->AddLine(ImVec2(at.x + 6 * s, at.y - 3.5f * s), ImVec2(at.x + 10 * s, at.y), c, 1.4f * s);
    d->AddLine(ImVec2(at.x + 6 * s, at.y + 3.5f * s), ImVec2(at.x + 10 * s, at.y), c, 1.4f * s);
}

// The source diff of a script write in a small scrolling box (computed once by the page when the card opened);
// returns its height in pixels.
float DrawAiDiff(const nickel::ai::PendingCall& call, ImVec2 at, float width, float s, float a) {
    namespace ai = nickel::ai;
    const std::vector<ai::DiffLine>& diff = call.diff;
    const std::string& id = call.use.id;
    const size_t shown = std::min<size_t>(diff.size(), 400);
    const float lineH = 15 * s;
    const float contentH = shown * lineH + 8 * s;
    const float boxH = std::min(contentH, 220 * s);
    ImGui::SetCursorScreenPos(at);
    ImGui::BeginChild(("##ai-diff" + id).c_str(), ImVec2(width, boxH), false, ImGuiWindowFlags_None);
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    d->AddRectFilled(o, ImVec2(o.x + width, o.y + std::max(boxH, contentH)), Rgba(10, 10, 13, a));
    for (size_t i = 0; i < shown; ++i) {
        const auto& line = diff[i];
        const bool added = line.kind == ai::DiffLine::Kind::Added;
        const bool removed = line.kind == ai::DiffLine::Kind::Removed;
        const float y = o.y + 4 * s + i * lineH;
        if (added || removed) {
            const ImU32 band = added ? Rgba(60, 200, 120, .12f * a) : Rgba(240, 86, 106, .12f * a);
            d->AddRectFilled(ImVec2(o.x, y), ImVec2(o.x + width, y + lineH), band);
        }
        const std::string text = std::string(added ? "+ " : removed ? "- " : "  ") + line.text;
        const ImU32 ink = added ? Rgba(120, 230, 160, a) : removed ? Rgba(246, 160, 171, a) : Rgba(148, 148, 160, a);
        Label(d, regular, 11.5f * s, ImVec2(o.x + 4 * s, y), Fit(regular, 11.5f, text, width / s - 12).c_str(), ink);
    }
    ImGui::Dummy(ImVec2(1, contentH));
    EndChildWindow();
    float used = boxH + 6 * s;
    if (diff.size() > shown) {
        const std::string more = std::to_string(diff.size() - shown) + " more lines not shown.";
        ImDrawList* parent = ImGui::GetWindowDrawList();
        used += AiText(parent, regular, 12 * s, ImVec2(at.x, at.y + used), more, Rgba(120, 120, 132, a), width) + 4 * s;
    }
    return used;
}

// One confirmation card inside the chat child; returns its height in pixels. Everything on it comes from the card
// that ToolRunner::Describe built from the registry and the script library.
float DrawAiCard(ImDrawList* d, nickel::ai::ChatPage& page, const nickel::ai::PendingCall& call, ImVec2 p, float w,
                 float s, float a) {
    const float U = 242.f / 210.f;
    const float pad = 14 * s;
    const float inner = w - 2 * pad;
    const float x0 = p.x + pad;
    const nickel::ai::ConfirmCard& card = call.card;
    const ImU32 soft = Rgba(170, 170, 182, a);
    float y = p.y + pad;
    d->ChannelsSplit(2); // the box is drawn behind once its height is known
    d->ChannelsSetCurrent(1);
    const std::string title = Fit(semibold, 14.5f, card.title, inner / s);
    Label(d, semibold, 14.5f * s, ImVec2(x0, y), title.c_str(), Rgba(255, 255, 255, a));
    y += 24 * s;
    // "label        current -> next"
    for (const auto& line : card.lines) {
        const float half = inner * .5f;
        const float labelH = AiText(d, regular, 12.5f * s, ImVec2(x0, y), line.label, soft, half - 8 * s);
        const std::string now = Fit(regular, 12.5f, line.current.empty() ? "-" : line.current, half / s / 2 - 14);
        const float x = x0 + half;
        const float nowW = TextWidth(regular, 12.5f, now.c_str()) * s;
        Label(d, regular, 12.5f * s, ImVec2(x, y), now.c_str(), Rgba(148, 148, 160, a));
        AiArrow(d, ImVec2(x + nowW + 6 * s, y + 8 * s), s, Rgba(148, 148, 160, a));
        const std::string next = Fit(semibold, 12.5f, line.next, (half - nowW) / s - 24);
        Label(d, semibold, 12.5f * s, ImVec2(x + nowW + 22 * s, y), next.c_str(), Rgba(255, 255, 255, a));
        y += std::max(labelH, 18 * s) + 4 * s;
    }
    if (!card.scriptModules.empty()) {
        std::string list;
        for (const auto& name : card.scriptModules) {
            list += (list.empty() ? "" : ", ") + name;
        }
        y += AiText(d, regular, 12.5f * s, ImVec2(x0, y), "Declares: " + list, soft, inner) + 4 * s;
    }
    if (card.isScriptWrite) {
        const std::string what = (card.newScript ? "New script " : "Replaces script ") + card.scriptName
                                 + ". It is saved held: it will not run until you start it.";
        y += AiText(d, regular, 12.5f * s, ImVec2(x0, y), what, soft, inner) + 6 * s;
        y += DrawAiDiff(call, ImVec2(x0, y), inner, s, a);
        if (!card.compileErrors.empty()) {
            std::string errors = "Does not compile yet:";
            for (size_t i = 0; i < card.compileErrors.size() && i < 3; ++i) {
                errors += "\n" + card.compileErrors[i];
            }
            y += AiText(d, regular, 12.5f * s, ImVec2(x0, y), errors, Rgba(246, 160, 171, a), inner) + 4 * s;
        }
    }
    if (!card.warning.empty()) {
        y += AiText(d, regular, 12.5f * s, ImVec2(x0, y), card.warning, Rgba(240, 190, 90, a), inner) + 4 * s;
    }
    y += 6 * s;
    // time left, then Decline and Allow (Save for a script)
    const float bw = 84 * U;
    const float bh = 30 * U;
    const int left = static_cast<int>(page.SecondsLeft(call));
    char timer[48];
    std::snprintf(timer, sizeof(timer), "Declines itself in %d:%02d", left / 60, left % 60);
    Label(d, regular, 12 * s, ImVec2(x0, y + (bh * s - 14 * s) / 2), timer, Rgba(120, 120, 132, a));
    const std::string id = call.use.id;
    // a card that just appeared or moved (the one above it was answered, the chat scrolled) is not clickable yet
    page.PlaceCard(id, p.x, p.y);
    const bool armed = page.Armed(call);
    const ImVec2 allowAt(p.x + w - pad - bw * s, y);
    const ImVec2 declineAt(allowAt.x - bw * s - 8 * s, y);
    const char* yes = card.isScriptWrite ? "Save" : "Allow";
    const bool allow = UiButton(d, "ai-allow" + id, yes, allowAt, bw, bh, 1, armed, a, s);
    const bool decline = UiButton(d, "ai-decline" + id, "Decline", declineAt, bw, bh, 0, armed, a, s);
    y += bh * s + pad;
    d->ChannelsSetCurrent(0);
    d->AddRectFilled(p, ImVec2(p.x + w, y), Rgba(20, 20, 27, a));
    d->AddRect(p, ImVec2(p.x + w, y), Acc(.7f * a), 0, 0, 1.2f * s);
    d->ChannelsMerge();
    if (allow) {
        page.Allow(id); // the card is gone from the next frame
    } else if (decline) {
        page.Decline(id);
    }
    return y - p.y;
}

// The bottom of the page: the message field (Enter sends, Ctrl+Enter adds a line), Send or Stop, the character count
// near the limit, and why a message cannot go now.
void DrawAiInput(AiMenu& m, ImVec2 origin, float s, float opacity, float left, float right, float inputTop,
                 float inputH) {
    namespace ai = nickel::ai;
    ai::ChatPage& page = *m.page;
    const float U = 242.f / 210.f;
    const float sendW = 76 * U;
    const float areaW = right - left;
    auto* d = ImGui::GetWindowDrawList();
    auto P = [&](float x, float y) {
        return ImVec2(origin.x + x * s, origin.y + y * s);
    };
    const std::string draft = m.input;
    const std::string blocked = page.SendBlockedReason(draft);
    const bool busy = page.Busy();
    ImGui::SetCursorScreenPos(P(left, inputTop));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * s, 8 * s));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(.1f, .1f, .125f, .96f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(.12f, .12f, .15f, .96f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(.12f, .12f, .15f, .96f));
    if (m.focusInput && page.Available()) {
        ImGui::SetKeyboardFocusHere();
        m.focusInput = false;
    }
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine
                                | ImGuiInputTextFlags_CallbackEdit | ImGuiInputTextFlags_CallbackAlways;
    if (!page.Available()) {
        flags |= ImGuiInputTextFlags_ReadOnly;
    }
    const ImVec2 fieldSize((areaW - sendW - 8) * s, inputH * s);
    bool send = ImGui::InputTextMultiline("##ai-input", m.input, sizeof(m.input), fieldSize, flags, AiInputCallback);
    const bool editing = ImGui::IsItemActive();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
    if (m.input[0] == 0 && !editing) {
        const char* hint = page.Available() ? "Ask Nickel AI about modules, settings or scripts..."
                                            : "Nickel AI is not available in this build.";
        Label(d, regular, 13 * s, P(left + 10, inputTop + 9), hint, Rgba(110, 110, 122, opacity));
    }
    const size_t chars = ai::CountChars(m.input);
    if (chars + 500 > ai::kMaxMessageChars) {
        const std::string count = std::to_string(chars) + "/" + std::to_string(ai::kMaxMessageChars);
        const float x = right - sendW - 14 - TextWidth(regular, 11.5f, count.c_str());
        Label(d, regular, 11.5f * s, P(x, inputTop + inputH - 17), count.c_str(), Rgba(148, 148, 160, opacity));
    }
    if (busy) {
        if (UiButton(d, "ai-stop", "Stop", P(right - sendW, inputTop), sendW, inputH, 3, true, opacity, s)) {
            page.Cancel();
        }
    } else {
        const ImVec2 at = P(right - sendW, inputTop);
        if (UiButton(d, "ai-send", "Send", at, sendW, inputH, 1, blocked.empty(), opacity, s)) {
            send = true;
        }
    }
    // the reason a typed message cannot go now (an empty field needs none), and always a used-up quota
    if (!blocked.empty() && !busy && (!ai::Trim(draft).empty() || page.QuotaUsedUp())) {
        const std::string why = Fit(regular, 12, blocked, areaW);
        Label(d, regular, 12 * s, P(left + 2, inputTop - 20), why.c_str(), Rgba(240, 190, 90, opacity));
    }
    if (send && !busy && page.Send(draft)) {
        m.input[0] = 0;
        m.clearInput = true;
        m.focusInput = true;
    }
}

// The Nickel AI page (tab 13), in the main panel: quota and activity, New chat, the conversation with its notes,
// cards and error line, then the input.
void DrawAi(ImVec2 origin, float s, float opacity) {
    namespace ai = nickel::ai;
    AiMenu& m = Ai();
    if (!m.created) { // first use: the client (and with it the install key) exists from here
        m.created = true;
        if (ai::Factory() != nullptr) {
            ai::Services services = ai::Factory()();
            m.client = std::move(services.client);
            m.tools = std::move(services.tools);
        }
        m.page = std::make_unique<ai::ChatPage>(m.client.get(), m.tools.get());
        m.focusInput = true;
    }
    ai::ChatPage& page = *m.page;
    page.Tick(ImGui::GetTime(), static_cast<int64_t>(std::time(nullptr)), opened.load() && opacity > .05f);
    const float U = 242.f / 210.f;
    const float left = 257, right = menuW - 22, top = 72, bottom = menuH - 22, areaW = right - left;
    auto* d = ImGui::GetWindowDrawList();
    auto P = [&](float x, float y) {
        return ImVec2(origin.x + x * s, origin.y + y * s);
    };
    const ai::ConversationSnapshot& conv = page.Conversation();

    // ---- top row: quota and activity on the left, New chat on the right
    const std::string quota = page.QuotaLine();
    Label(d, semibold, 13 * s, P(left + 4, top + 6), quota.c_str(), Rgba(170, 170, 182, opacity));
    const std::string activity = page.ActivityText();
    if (!activity.empty()) {
        const float x = left + 4 + TextWidth(semibold, 13, quota.c_str()) + 18;
        const float pulse = .55f + .45f * std::sin(static_cast<float>(ImGui::GetTime()) * 5);
        d->AddCircleFilled(P(x, top + 14), 3 * s, Acc(opacity * pulse));
        const std::string dots(static_cast<size_t>(ImGui::GetTime() * 3) % 4, '.');
        Label(d, regular, 13 * s, P(x + 10, top + 6), (activity + dots).c_str(), Rgba(148, 148, 160, opacity));
    }
    const bool hasChat = !conv.messages.empty() || !page.Notes().empty();
    if (UiButton(d, "ai-new-chat", "New chat", P(right - 84 * U, top), 84 * U, 28 * U, 0, page.Available() && hasChat,
                 opacity, s)) {
        page.NewChat();
        m.seenItems = 0;
    }

    // ---- input (drawn first so a Send of this frame shows in the conversation below)
    const float inputH = 64, inputTop = bottom - inputH;
    DrawAiInput(m, origin, s, opacity, left, right, inputTop, inputH);

    // ---- conversation
    const float chatTop = top + 40, chatBottom = inputTop - 26;
    ImGui::SetCursorScreenPos(P(left, chatTop));
    ImGui::BeginChild("##ai-chat", ImVec2(areaW * s, (chatBottom - chatTop) * s), false,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    d = ImGui::GetWindowDrawList();
    const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 8 * s;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = areaW * s, bubbleMax = width * .78f, pad = 10 * s;
    auto centred = [&](ImFont* font, float size, const char* text, float y, ImU32 color) {
        Label(d, font, size * s, ImVec2(start.x + (width - TextWidth(font, size, text) * s) / 2, start.y + y * s), text,
              color);
    };
    if (!page.Available()) {
        centred(bold, 16, "Nickel AI is not available in this build.", 120, Rgba(255, 255, 255, opacity));
    } else if (!hasChat) {
        centred(bold, 17, "Ask Nickel AI", 110, Rgba(255, 255, 255, opacity));
        const ImU32 grey = Rgba(148, 148, 160, opacity);
        centred(regular, 13, "It finds modules, explains settings and writes scripts.", 140, grey);
        centred(regular, 13, "It asks you before it changes anything.", 160, grey);
    }
    float y = start.y + 4 * s;
    const auto& notes = page.Notes();
    size_t note = 0;
    auto drawNotes = [&](size_t afterMessages) {
        for (; note < notes.size() && notes[note].afterMessages <= afterMessages; ++note) {
            const auto& n = notes[note];
            const ImU32 dot = n.kind == ai::ChatNote::Kind::Applied  ? Acc(opacity)
                              : n.kind == ai::ChatNote::Kind::Failed ? Rgba(246, 160, 171, opacity)
                                                                     : Rgba(148, 148, 160, opacity);
            d->AddCircleFilled(ImVec2(start.x + 8 * s, y + 8 * s), 2.5f * s, dot);
            const ImVec2 at(start.x + 18 * s, y);
            y += AiText(d, regular, 12 * s, at, n.text, Rgba(170, 170, 182, opacity), width - 24 * s) + 8 * s;
        }
    };
    for (size_t i = 0; i < conv.messages.size(); ++i) {
        drawNotes(i);
        const auto& msg = conv.messages[i];
        const bool player = msg.role == ai::ChatMessage::Role::Player;
        if (!player && msg.text.empty()) {
            continue; // a step that only called tools
        }
        const bool streaming = !player && i + 1 == conv.messages.size()
                               && page.CurrentActivity() == ai::Activity::Writing;
        const float px = 13.5f * s * FontEmFactor;
        const ImVec2 box = Sized(regular, px)->CalcTextSizeA(px, FLT_MAX, bubbleMax - 2 * pad, msg.text.c_str(),
                                                             msg.text.c_str() + msg.text.size());
        const float bw = std::min(bubbleMax, box.x + 2 * pad + (streaming ? 8 * s : 0));
        const float bh = box.y + 2 * pad;
        const float x = player ? start.x + width - bw - 4 * s : start.x;
        const ImU32 fill = player ? AccDeep(.9f * opacity) : Rgba(23, 23, 29, opacity);
        d->AddRectFilled(ImVec2(x, y), ImVec2(x + bw, y + bh), fill);
        const ImU32 ink = player ? Rgba(255, 255, 255, opacity) : Rgba(225, 225, 232, opacity);
        AiText(d, regular, 13.5f * s, ImVec2(x + pad, y + pad), msg.text, ink, bubbleMax - 2 * pad);
        if (streaming && std::fmod(ImGui::GetTime(), 1.0) < .6) { // a blinking caret while the answer streams
            const ImVec2 caret(x + bw - pad - 2 * s, y + bh - pad - 14 * s);
            d->AddRectFilled(caret, ImVec2(caret.x + 6 * s, caret.y + 14 * s), Acc(opacity));
        }
        y += bh + 10 * s;
    }
    drawNotes(SIZE_MAX);
    for (const auto* call : page.OpenCards()) {
        y += DrawAiCard(d, page, *call, ImVec2(start.x, y), width - 4 * s, s, opacity) + 10 * s;
    }
    const std::string error = page.ErrorLine();
    if (!error.empty()) {
        const bool failed = conv.state == ai::TurnState::Failed;
        const float textW = width - 2 * pad - 4 * s;
        const float h = AiTextHeight(regular, 13 * s, error, textW) + 2 * pad;
        const ImU32 fill = failed ? Rgba(240, 86, 106, .14f * opacity) : Rgba(255, 255, 255, .06f * opacity);
        d->AddRectFilled(ImVec2(start.x, y), ImVec2(start.x + width - 4 * s, y + h), fill);
        const ImU32 ink = failed ? Rgba(246, 160, 171, opacity) : Rgba(200, 200, 210, opacity);
        AiText(d, regular, 13 * s, ImVec2(start.x + pad, y + pad), error, ink, textW);
        y += h + 10 * s;
    }
    ImGui::SetCursorScreenPos(ImVec2(start.x, y));
    ImGui::Dummy(ImVec2(1, 1));
    // follow the conversation while the player is at the bottom (new messages, streamed text, notes, cards)
    const size_t items = conv.messages.size() + notes.size() + page.OpenCards().size()
                         + (conv.messages.empty() ? 0 : conv.messages.back().text.size());
    if (items != m.seenItems && atBottom) {
        ImGui::SetScrollHereY(1.f);
    }
    m.seenItems = items;
    const float listScroll = ImGui::GetScrollY(), listLimit = ImGui::GetScrollMaxY();
    FadeEdges(s, opacity);
    EndChildWindow();
    // scrollbar to the right of the conversation, only when it overflows (theme accent colour)
    if (listLimit > 0) {
        const float view = chatBottom - chatTop, total = view + listLimit / s;
        const float bar = std::max(30.f, view * view / total);
        const float barTop = chatTop + (view - bar) * (listScroll / listLimit);
        ImGui::GetWindowDrawList()->AddRectFilled(P(right + 3, barTop + 6), P(right + 6, barTop + bar - 6),
                                                  Acc(.85f * opacity), 0);
    }
}

#endif // NICKEL_AI_UI_DRAW
