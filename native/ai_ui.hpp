// Nickel AI in the menu: a round bubble with the Nickel mark beside the menu's left edge, and the chat window it opens
// (the assistant's messages on the left with the Nickel avatar, the player's on the right, the streamed answer, a
// typing indicator, the quota, errors as centred lines, and the confirmation cards of action tools).
//
// Two parts in one file:
// - the logic (nickel::ai::ChatPage, its Timeline, PlaceBubble and BubbleState): no ImGui, no Windows API, checked by
//   tests/ai_ui_checks.cpp on a fake ChatClient and a fake ToolRunner. Included at the top of menu.cpp like any header.
// - the drawing (DrawAiBubble, the chat window, the Settings toggle): compiled only when NICKEL_AI_UI_DRAW is defined,
//   which menu.cpp does when it includes this file a second time inside its anonymous namespace, next to public_ui.hpp
//   (it reuses Label, Hit, Animate and the theme).
//
// Rules the page enforces (docs/design/nickel-ai/permissions-and-safety.md, protocol in ai_types.hpp):
// - Nickel AI is on by default (Settings > Connection > Nickel AI (assistant)). While it is off nothing is created:
//   no client, no worker, no install key, no request, and no bubble. While it is on, the client exists only from the
//   first time the player opens the chat window.
// - Read tools run at once; every action tool (settings, modules, scripts) waits for the player's Allow on a card drawn
//   from ToolRunner::Describe (the registry and the script library), never from the model's text. Decline, or no answer
//   within 2 minutes of the window showing, sends the "declined" error and changes nothing.
// - The work goes on while the player plays with the window closed: reading tools run, nothing that changes something
//   runs without the player's Allow. When the answer is done, fails or waits on a card, a notice is shown
//   (TakeNotice; drawn by the menu, ai_ui draw part).
// - A script write shows the diff and says the script is saved held (it does not run until the player starts it).
// - A player message is 1..4000 characters; the quota comes from the service ("22 left today").
// - The greeting is local: it is never sent and does not count in the quota.

#ifndef NICKEL_AI_UI_LOGIC
#define NICKEL_AI_UI_LOGIC

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "ai_markdown.hpp"
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
// "+1 in 12 min": when the next message comes back (messages come back one by one); empty when unknown or past.
inline std::string NextMessageText(const std::string& resetAt, int64_t utcNow) {
    const int64_t at = ParseUtc(resetAt);
    if (at == 0 || utcNow <= 0) {
        return "";
    }
    const int64_t minutes = std::max<int64_t>(1, (at - utcNow + 59) / 60);
    return at <= utcNow ? "" : "+1 in " + std::to_string(minutes) + " min";
}

inline std::string ResetText(const std::string& resetAt, int64_t utcNow = 0) {
    const std::string next = NextMessageText(resetAt, utcNow); // "+1 in 12 min"
    return next.empty() ? "" : "One message comes back " + next.substr(3) + ".";
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
        return "No Nickel AI messages left for now: one comes back every 30 minutes.";
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
            {"capture_packets", "Capturing packets"},
            {"read_packets", "Reading packets"},
            {"get_server_info", "Looking at the server"},
            {"get_world_state", "Looking around"},
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

// What the page tells the player while the chat window is hidden (ChatPage::TakeNotice).
struct PageNotice {
    enum class Kind { None, Answered, NeedsYou, Failed };
    Kind kind = Kind::None;
    std::string text;
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

// The assistant's first words in every chat. Local: never sent to the service and not counted in the quota.
constexpr const char* kGreeting =
        "Hi! I'm Nickel AI. Ask me anything about Nickel: modules, settings or scripts. I can also turn modules on or "
        "write a script for you, always after you confirm.";

// Where an item of the chat sits: the assistant on the left, the player on the right, system lines centred.
enum class ChatSide { Assistant, Player, Centre };

// One thing the chat window draws, in order (ChatPage::Timeline).
struct ChatItem {
    enum class Kind {
        Greeting, // the local greeting (assistant side)
        Message,  // a message of the conversation (`message` is its index)
        Typing,   // the assistant is thinking or using tools: three animated dots (`text`: the tool at work, or empty)
        Card,     // a confirmation card (assistant side, `call`)
        Note,     // "Applied: ...", "Declined: ..." (centred, `note` is its index)
        System,   // an error or a stop line (centred, `text`; `failure` for an error)
    };
    Kind kind = Kind::System;
    ChatSide side = ChatSide::Centre;
    bool groupStart = false; // first item of a run on its side: the avatar is drawn next to it
    size_t message = 0;
    size_t note = 0;
    const PendingCall* call = nullptr;
    bool streaming = false; // the assistant's answer is still arriving
    bool failure = false;
    std::string text;
};

class ChatPage {
public:
    ChatPage(ChatClient* client, ToolRunner* tools) : client_(client), tools_(tools) {}

    bool Available() const { return client_ != nullptr && tools_ != nullptr; }

    // Once per frame on the render thread, whether the chat window shows or not (`visible`). `now` is a monotonic
    // clock in seconds, `utcNow` the wall clock (seconds since 1970, UTC) for the quota reset.
    // Work goes on while the player plays: reading tools run and their results go back at once. A call that changes
    // something waits on its card until the player opens the window and answers it; the card's time to answer only
    // runs while the window shows, and a card is armed again when the window comes back.
    void Tick(double now, int64_t utcNow, bool visible) {
        const double hiddenFor = !visible && lastTick_ > 0 && now > lastTick_ ? now - lastTick_ : 0;
        const bool cameBack = visible && !lastVisible_;
        lastTick_ = now;
        lastVisible_ = visible;
        now_ = now;
        utcNow_ = utcNow;
        if (!Available()) {
            return;
        }
        Refresh();
        WatchForNotice(visible);
        for (auto& call : calls_) {
            if (!call.carded || call.decided) {
                continue;
            }
            if (hiddenFor > 0) {
                call.shownAt += hiddenFor; // the countdown waits for the player
            }
            if (cameBack) {
                call.armedAt = std::max(call.armedAt, now + kCardArmSeconds);
            }
        }
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
        if (sent_) {
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
            } else if (visible && now - call.shownAt >= kConfirmTimeoutSeconds) {
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
            const std::string reset = ResetText(snap_.quota.resetAt, utcNow_);
            return "No message left for now."
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

    // The notice raised while the window was hidden (answer done, failed, a card waiting), once; empty otherwise.
    PageNotice TakeNotice() {
        PageNotice out = notice_;
        notice_ = {};
        return out;
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
            RedescribeOpenCards();
        }
        FlushIfDone();
        return true;
    }

    // After a change, the other open cards of the step may describe a state that is gone (a revert card built before
    // a module was turned off, a script card listing the modules of the source just replaced). Run works the change out
    // again when Allow is pressed, so each card is described again from the current state and armed again, and the
    // player always confirms what will happen. A card the tools now refuse is answered with that refusal.
    void RedescribeOpenCards() {
        for (auto& other : calls_) {
            if (!other.carded || other.decided) {
                continue;
            }
            ConfirmCard card;
            ToolResult refusal;
            if (tools_->Describe(other.use, &card, &refusal)) {
                card.toolUseId = other.use.id;
                card.tool = other.use.name;
                if (card.title.empty()) {
                    card.title = ToolActivity(other.use.name);
                }
                other.diff = card.isScriptWrite ? LineDiff(card.currentSource, card.newSource) : std::vector<DiffLine>{};
                other.card = std::move(card);
                other.armedAt = now_ + kCardArmSeconds;
                other.shownAt = now_; // new content: the full time to answer it
            } else {
                if (refusal.content.empty()) {
                    refusal = MakeToolError(other.use.id, "refused", "Nickel refused this call.");
                }
                refusal.isError = true;
                other.result = Checked(other.use.id, std::move(refusal));
                other.decided = true;
                Note(ChatNote::Kind::Declined, "No longer possible: " + other.card.title);
            }
        }
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

    // "22 left · +1 in 12 min" (one message comes back every 30 minutes, up to the limit), "Unlimited"; before the
    // first answer of the session, the limit.
    std::string QuotaLine() const {
        if (!snap_.quota.known) {
            return "Up to " + std::to_string(kDefaultDailyLimit) + " messages";
        }
        if (snap_.quota.unlimited) {
            return "Unlimited messages";
        }
        const int left = std::max(0, snap_.quota.limit - snap_.quota.used);
        const std::string next = NextMessageText(snap_.quota.resetAt, utcNow_);
        return std::to_string(left) + " left" + (next.empty() ? "" : " Â· " + next);
    }

    bool QuotaUsedUp() const {
        if (!snap_.quota.known || snap_.quota.unlimited || snap_.quota.used < snap_.quota.limit) {
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

    // Everything the chat window shows, top to bottom (see ChatItem). Rebuilt each frame from the conversation, the
    // notes, the open cards and the activity; the greeting is local and always first.
    std::vector<ChatItem> Timeline() const {
        std::vector<ChatItem> items;
        if (!Available()) {
            ChatItem line;
            line.kind = ChatItem::Kind::System;
            line.text = "Nickel AI is not available in this build.";
            items.push_back(std::move(line));
            return items;
        }
        ChatItem greeting;
        greeting.kind = ChatItem::Kind::Greeting;
        greeting.side = ChatSide::Assistant;
        greeting.text = kGreeting;
        items.push_back(std::move(greeting));
        size_t note = 0;
        auto notesUpTo = [&](size_t afterMessages) {
            for (; note < notes_.size() && notes_[note].afterMessages <= afterMessages; ++note) {
                ChatItem line;
                line.kind = ChatItem::Kind::Note;
                line.note = note;
                line.text = notes_[note].text;
                items.push_back(std::move(line));
            }
        };
        const Activity activity = CurrentActivity();
        for (size_t i = 0; i < snap_.messages.size(); ++i) {
            notesUpTo(i);
            const ChatMessage& message = snap_.messages[i];
            const bool player = message.role == ChatMessage::Role::Player;
            if (!player && message.text.empty()) {
                continue; // a step that only called tools
            }
            ChatItem item;
            item.kind = ChatItem::Kind::Message;
            item.side = player ? ChatSide::Player : ChatSide::Assistant;
            item.message = i;
            item.streaming = !player && i + 1 == snap_.messages.size() && activity == Activity::Writing;
            items.push_back(std::move(item));
        }
        notesUpTo(SIZE_MAX);
        for (const PendingCall* call : OpenCards()) {
            ChatItem card;
            card.kind = ChatItem::Kind::Card;
            card.side = ChatSide::Assistant;
            card.call = call;
            items.push_back(std::move(card));
        }
        if (activity == Activity::Thinking || activity == Activity::UsingTools) {
            ChatItem typing;
            typing.kind = ChatItem::Kind::Typing;
            typing.side = ChatSide::Assistant;
            if (activity == Activity::UsingTools) {
                typing.text = ActivityText(); // "Searching modules": the tool at work, from the fixed labels
            }
            items.push_back(std::move(typing));
        }
        const std::string error = ErrorLine();
        if (!error.empty()) {
            ChatItem line;
            line.kind = ChatItem::Kind::System;
            line.failure = snap_.state != TurnState::Idle;
            line.text = error;
            items.push_back(std::move(line));
        }
        // a run of items on one side starts with its avatar; a centred line ends the run
        for (size_t i = 0; i < items.size(); ++i) {
            items[i].groupStart = items[i].side != ChatSide::Centre && (i == 0 || items[i - 1].side != items[i].side);
        }
        return items;
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
    double lastTick_ = 0;
    bool lastVisible_ = false;
    bool wasWorking_ = false;   // a turn was running at the last tick (for the notice when it ends)
    bool cardsNoticed_ = false; // the open cards were already announced
    PageNotice notice_;

    // While the window is hidden: a notice when the answer is done, when it failed, or when a card waits.
    void WatchForNotice(bool visible) {
        bool openCards = false;
        for (const auto& call : calls_) {
            openCards = openCards || (call.carded && !call.decided);
        }
        const bool working = Busy();
        if (visible) {
            notice_ = {};
            wasWorking_ = working;
            cardsNoticed_ = openCards;
            return;
        }
        if (openCards && !cardsNoticed_) {
            notice_ = {PageNotice::Kind::NeedsYou, "Nickel AI is waiting for your OK on a change."};
            cardsNoticed_ = true;
        } else if (wasWorking_ && !working) {
            notice_ = snap_.state == TurnState::Failed
                              ? PageNotice{PageNotice::Kind::Failed, "Nickel AI stopped before the end of its answer."}
                              : PageNotice{PageNotice::Kind::Answered, "Nickel AI finished its answer."};
        }
        wasWorking_ = working;
        if (!openCards) {
            cardsNoticed_ = false;
        }
    }
    int64_t utcNow_ = 0;
    double retryUntil_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------
// The floating bubble and its chat window

// Sizes at menu scale 1, in pixels.
constexpr float kBubbleSize = 48;       // the round button with the Nickel mark
constexpr float kBubbleGap = 12;        // between the bubble and the menu, and between the bubble and its window
constexpr float kBubbleBottom = 20;     // from the bubble's bottom to the menu's bottom
constexpr float kBubbleInset = 16;      // inside the sidebar, from its left and bottom edges
constexpr float kChatWidth = 380;
constexpr float kChatHeight = 520;
constexpr float kChatMinHeight = 300;
constexpr float kScreenMargin = 8;      // screen pixels, like the menu's own margin

struct UiRect {
    float x = 0, y = 0, w = 0, h = 0;
    float Right() const { return x + w; }
    float Bottom() const { return y + h; }
    bool Contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    bool Overlaps(const UiRect& o) const { return x < o.Right() && o.x < Right() && y < o.Bottom() && o.y < Bottom(); }
};

// The sides of the bubble its window takes.
struct ChatSides {
    bool above = true;    // above the bubble (else below)
    bool leftward = true; // its right edge on the bubble's: it extends to the left (else from the bubble's left edge)
};

struct BubbleLayout {
    UiRect bubble;
    UiRect window;
    ChatSides sides;
    bool moved = false;           // the player's place (PlaceBubbleAt), not the default one beside the menu
    bool bubbleInSidebar = false; // default place, no room left of the menu: the bubble sits in the sidebar's corner
    bool windowOverMenu = false;  // the window covers part of the menu
};

// `menu` is the menu window on screen (pixels), `scale` the menu's scale. The default place, while the player never
// moved the bubble: just outside the menu's left edge near its bottom, or inside the sidebar's bottom-left corner when
// the screen has no room for it. The window opens above the bubble, on the left: its right edge on the bubble's (left
// of the menu) when it fits, otherwise from the screen's left margin (over the menu's left part); in the sidebar case
// its left edge is the bubble's. It never covers the bubble unless the screen is shorter than its minimum height.
inline BubbleLayout PlaceBubble(const UiRect& menu, float scale, float screenW, float screenH) {
    BubbleLayout out;
    const float size = kBubbleSize * scale, gap = kBubbleGap * scale, margin = kScreenMargin;
    const float outsideX = menu.x - gap - size;
    if (outsideX >= margin) {
        out.bubble = {outsideX, menu.Bottom() - kBubbleBottom * scale - size, size, size};
    } else {
        out.bubbleInSidebar = true;
        out.bubble = {menu.x + kBubbleInset * scale, menu.Bottom() - kBubbleInset * scale - size, size, size};
    }
    out.bubble.y = std::max(margin, std::min(out.bubble.y, screenH - margin - size));
    const float w = std::min(kChatWidth * scale, std::max(0.f, screenW - 2 * margin));
    float h = std::min(kChatHeight * scale, std::max(0.f, screenH - 2 * margin));
    const float above = out.bubble.y - gap - margin; // room between the screen's top margin and the bubble
    float y;
    if (above >= std::min(h, kChatMinHeight * scale)) {
        h = std::min(h, above);
        y = out.bubble.y - gap - h;
    } else {
        y = margin; // a very short screen: the window may cover the bubble; its close button still closes it
    }
    float x = out.bubbleInSidebar ? out.bubble.x : out.bubble.Right() - w;
    x = std::max(margin, std::min(x, screenW - margin - w));
    out.window = {x, y, w, h};
    out.sides = {true, !out.bubbleInSidebar};
    out.windowOverMenu = out.window.Overlaps(menu);
    return out;
}

// ---- the player's place: the bubble dragged anywhere, the window following it

// A press that travels further than this (screen pixels) moves the bubble or the window; a shorter one is a click.
constexpr float kDragThreshold = 4;

// The window beside a bubble placed by the player: above or below it, extending left or right of it, on the sides
// `prefer` (last frame's) while they have room, else on the sides with room (the larger room when neither has all of
// it), always inside the screen's margin. Its height shrinks to the room down to its minimum; below that it covers the
// bubble rather than leave the screen (its close button still closes it).
inline UiRect PlaceChatWindow(const UiRect& bubble, float scale, float screenW, float screenH, ChatSides prefer,
                              ChatSides* chosen) {
    const float gap = kBubbleGap * scale, margin = kScreenMargin;
    const float w = std::min(kChatWidth * scale, std::max(0.f, screenW - 2 * margin));
    const float full = std::min(kChatHeight * scale, std::max(0.f, screenH - 2 * margin));
    const float shortest = std::min(full, kChatMinHeight * scale);
    const float roomAbove = bubble.y - gap - margin, roomBelow = screenH - margin - bubble.Bottom() - gap;
    const float roomLeft = bubble.Right() - margin, roomRight = screenW - margin - bubble.x;
    ChatSides sides;
    const float preferredV = prefer.above ? roomAbove : roomBelow, otherV = prefer.above ? roomBelow : roomAbove;
    sides.above = preferredV >= full || (otherV < full && preferredV >= otherV) ? prefer.above : !prefer.above;
    const float preferredH = prefer.leftward ? roomLeft : roomRight, otherH = prefer.leftward ? roomRight : roomLeft;
    sides.leftward = preferredH >= w || (otherH < w && preferredH >= otherH) ? prefer.leftward : !prefer.leftward;
    const float room = sides.above ? roomAbove : roomBelow;
    float h = full, y = margin;
    if (room >= shortest) {
        h = std::min(full, room);
        y = sides.above ? bubble.y - gap - h : bubble.Bottom() + gap;
    }
    float x = sides.leftward ? bubble.Right() - w : bubble.x;
    x = std::max(margin, std::min(x, screenW - margin - w));
    y = std::max(margin, std::min(y, screenH - margin - h));
    if (chosen != nullptr) {
        *chosen = sides;
    }
    return {x, y, w, h};
}

// The bubble whose centre is at (fx, fy), fractions of the screen's width and height (how its place is kept, so that it
// survives other window sizes), kept inside the screen's margin, and its window.
inline BubbleLayout PlaceBubbleAt(float fx, float fy, float scale, float screenW, float screenH,
                                  ChatSides prefer = {}) {
    BubbleLayout out;
    out.moved = true;
    const float size = kBubbleSize * scale, margin = kScreenMargin;
    const float x = std::clamp(fx, 0.f, 1.f) * screenW - size / 2, y = std::clamp(fy, 0.f, 1.f) * screenH - size / 2;
    out.bubble = {std::max(margin, std::min(x, screenW - margin - size)),
                  std::max(margin, std::min(y, screenH - margin - size)), size, size};
    out.window = PlaceChatWindow(out.bubble, scale, screenW, screenH, prefer, &out.sides);
    return out;
}

// The fractions PlaceBubbleAt takes for a bubble at `bubble` on this screen.
inline void BubbleFraction(const UiRect& bubble, float screenW, float screenH, float* fx, float* fy) {
    *fx = screenW > 0 ? std::clamp((bubble.x + bubble.w / 2) / screenW, 0.f, 1.f) : .5f;
    *fy = screenH > 0 ? std::clamp((bubble.y + bubble.h / 2) / screenH, 0.f, 1.f) : .5f;
}

// The bubble dragged by (dx, dy) from where the press began; its window follows (and may change sides).
inline BubbleLayout DragBubble(const BubbleLayout& from, float dx, float dy, float scale, float screenW,
                               float screenH) {
    UiRect moved = from.bubble;
    moved.x += dx;
    moved.y += dy;
    float fx = 0, fy = 0;
    BubbleFraction(moved, screenW, screenH, &fx, &fy);
    return PlaceBubbleAt(fx, fy, scale, screenW, screenH, from.sides);
}

// The window dragged by its header by (dx, dy): the bubble and the window move together, as one, and stop at the
// screen's margin (the window does not change sides under the mouse).
inline BubbleLayout DragTogether(const BubbleLayout& from, float dx, float dy, float screenW, float screenH) {
    const float margin = kScreenMargin;
    const float left = std::min(from.bubble.x, from.window.x), top = std::min(from.bubble.y, from.window.y);
    const float right = std::max(from.bubble.Right(), from.window.Right());
    const float bottom = std::max(from.bubble.Bottom(), from.window.Bottom());
    dx = std::max(margin - left, std::min(dx, screenW - margin - right));
    dy = std::max(margin - top, std::min(dy, screenH - margin - bottom));
    BubbleLayout out = from;
    out.moved = true;
    out.bubbleInSidebar = false;
    out.bubble.x += dx;
    out.bubble.y += dy;
    out.window.x += dx;
    out.window.y += dy;
    return out;
}

// The place as kept in <Nickel data>\ai\settings.ini ([ai] bubble=0.123400,0.567800); false for anything else, which
// leaves the default place.
inline bool ReadBubblePlace(const std::string& text, float* fx, float* fy) {
    float x = -1, y = -1;
    char tail = 0;
    if (std::sscanf(text.c_str(), "%f,%f%c", &x, &y, &tail) != 2 || !(x >= 0 && x <= 1) || !(y >= 0 && y <= 1)) {
        return false;
    }
    *fx = x;
    *fy = y;
    return true;
}

inline std::string WriteBubblePlace(float fx, float fy) {
    char text[64];
    std::snprintf(text, sizeof(text), "%.6f,%.6f", std::clamp(fx, 0.f, 1.f), std::clamp(fy, 0.f, 1.f));
    return text;
}

// A press on the bubble or on the window's header: a click when it ends before travelling kDragThreshold, otherwise a
// drag from then on (even if the mouse comes back).
class DragGesture {
public:
    void Press(float x, float y) {
        pressed_ = true;
        dragging_ = false;
        startX_ = x;
        startY_ = y;
        dx_ = dy_ = 0;
    }

    // The mouse while the button is held; true once the press is a drag.
    bool Move(float x, float y) {
        if (!pressed_) {
            return false;
        }
        dx_ = x - startX_;
        dy_ = y - startY_;
        if (!dragging_ && dx_ * dx_ + dy_ * dy_ > kDragThreshold * kDragThreshold) {
            dragging_ = true;
        }
        return dragging_;
    }

    // The button is up: true when the press was a click.
    bool Release() {
        const bool click = pressed_ && !dragging_;
        pressed_ = dragging_ = false;
        return click;
    }

    void Cancel() { pressed_ = dragging_ = false; }

    bool Pressed() const { return pressed_; }
    bool Dragging() const { return dragging_; }
    float DX() const { return dx_; }
    float DY() const { return dy_; }

private:
    bool pressed_ = false;
    bool dragging_ = false;
    float startX_ = 0, startY_ = 0, dx_ = 0, dy_ = 0;
};

// Whether the bubble and its window show. The page (and with it the client and the install key) is created the first
// time the window opens, never before.
class BubbleState {
public:
    // The bubble shows while Nickel AI is on and the menu is drawn; the window only with it.
    static bool BubbleShown(bool optionOn, bool menuShown) { return optionOn && menuShown; }

    bool WindowOpen() const { return open_; }

    // A click on the bubble opens the window, a second one closes it.
    void Click() {
        open_ = !open_;
        if (open_) {
            opened_ = true;
            focus_ = true;
        }
    }

    // The window's x button, or Nickel AI turned off.
    void Close() { open_ = false; }

    // navigate_ui to Nickel AI: opens it (an open window stays open).
    void Open() {
        if (!open_) {
            Click();
        }
    }

    // True once since the window last opened: the input takes the keyboard and the chat scrolls to its end.
    bool TakeOpening() {
        const bool was = focus_;
        focus_ = false;
        return was;
    }

    // The window was opened at least once since Nickel AI was turned on (the page may exist).
    bool EverOpened() const { return opened_; }

    void Reset() { *this = BubbleState(); }

private:
    bool open_ = false;
    bool opened_ = false;
    bool focus_ = false;
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

// Settings > Connection > Nickel AI (assistant). On by default (a player who turns it off keeps enabled=0); kept in
// root\ai\settings.ini, not in the preferences
// ini, so loading or sharing a config cannot turn it on.
MenuOption aiAssistantOption = {"nickel-ai",
                                "Nickel AI (assistant)",
                                "Answers questions about Nickel and can change modules or write scripts after you "
                                "allow each change. Sends your messages to nickelclient.com. While off, nothing is "
                                "sent.",
                                true,
                                true};

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
    nickel::ai::BubbleState bubble;
    std::unique_ptr<nickel::ai::ChatClient> client;
    std::unique_ptr<nickel::ai::ToolRunner> tools;
    std::unique_ptr<nickel::ai::ChatPage> page;
    char input[nickel::ai::kMaxMessageChars * 4 + 1] = {};
    bool clearInput = false;
    bool focusInput = false;
    size_t seenItems = 0;
    // Where the player put the bubble ([ai] bubble= in settings.ini, fractions of the screen); default place otherwise.
    bool placed = false;
    float placeX = 0, placeY = 0;
    nickel::ai::ChatSides sides;               // the window's sides last frame, kept while they have room
    nickel::ai::DragGesture bubbleDrag;        // a press on the bubble: a click toggles the window, a drag moves it
    nickel::ai::DragGesture headerDrag;        // a press on the window's header: a drag moves both
    nickel::ai::BubbleLayout dragFrom;         // the layout when that press began
    std::string reviewSend;                    // offline review (probe only): a message sent once the chat can send
    bool windowVisible = false;                // the chat window showed last frame (DrawAiBubble)
    nickel::ai::PageNotice toast;              // shown at the top of the screen while the window is hidden
    double toastUntil = 0;
    bool unread = false;                       // a notice came while hidden: a dot on the bubble until it opens
    // Soft line breaks of the input (AiInputCallback): spaces shown as line breaks so a long message wraps.
    std::string inputBefore;                   // the input at the last edit, to move the breaks with the edit
    std::vector<int> softBreaks;               // byte offsets of those breaks (a space when the message is sent)
    float inputWrap = 0;                       // the field's text width in pixels
};

// Never destroyed, like PUBLIC's client: the overlay has no unload path, and a destructor at process exit would join the
// AI client's worker thread inside DllMain. Turning the option off (AiShutdown) stops it on the render thread.
AiMenu& Ai() {
    static AiMenu* menu = new AiMenu();
    return *menu;
}

// The input as typed: soft breaks back to spaces.
std::string AiInputText() {
    AiMenu& m = Ai();
    std::string text = m.input;
    for (int at : m.softBreaks) {
        if (at >= 0 && at < static_cast<int>(text.size()) && text[static_cast<size_t>(at)] == '\n') {
            text[static_cast<size_t>(at)] = ' ';
        }
    }
    return text;
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
    m.bubble.Reset();
    m.input[0] = 0;
    m.seenItems = 0;
    m.bubbleDrag.Cancel(); // the place itself is kept
    m.headerDrag.Cancel();
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
        AiOption().value = GetPrivateProfileIntW(L"ai", L"enabled", 1, AiSettingPath().c_str()) == 1;
        wchar_t place[64] = {};
        GetPrivateProfileStringW(L"ai", L"bubble", L"", place, 64, AiSettingPath().c_str());
        m.placed = nickel::ai::ReadBubblePlace(Utf8(place), &m.placeX, &m.placeY);
        // offline review (probe only): Nickel AI on in memory, its window open
        char value[8] = {};
        if (OwnProbe() && GetEnvironmentVariableA("NICKEL_REVIEW_AI", value, sizeof(value))) {
            AiOption().value = true;
            m.bubble.Open();
            // NICKEL_REVIEW_AI_SEND: a player message sent once, as if typed (NICKEL_AI_LOCAL_SERVER: a test server)
            char message[512] = {};
            if (GetEnvironmentVariableA("NICKEL_REVIEW_AI_SEND", message, sizeof(message))) {
                m.reviewSend = message;
            }
        }
    }
    if (!AiOption().value && m.created) {
        AiShutdown();
    }
    // The page ticks every frame, menu open or closed: the answer goes on while the player plays (ChatPage::Tick).
    if (m.page) {
        m.page->Tick(ImGui::GetTime(), static_cast<int64_t>(std::time(nullptr)), m.windowVisible);
        nickel::ai::PageNotice notice = m.page->TakeNotice();
        if (notice.kind != nickel::ai::PageNotice::Kind::None) {
            m.toast = std::move(notice);
            m.toastUntil = ImGui::GetTime() + (m.toast.kind == nickel::ai::PageNotice::Kind::NeedsYou ? 12.0 : 7.0);
            m.unread = true;
        }
    }
    if (m.windowVisible) {
        m.unread = false;
        m.toastUntil = 0;
    }
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

// An answer laid out from its Markdown (ai_markdown.hpp) for `size` (pixels, as for Label) and `width` (pixels). Layouts
// are kept, least recently used dropped first, with room for the whole history (ai_client.hpp keeps 200 messages), so
// a long chat is never measured again every frame. The answer that is still streaming has its own entry, laid out
// again at most every 100 ms (its last pieces of text show a few frames later), so it neither fills the cache with
// every partial text nor costs a layout per frame. The reference stays valid until the next call.
const nickel::ai::md::Layout& AiMarkdownLayout(const std::string& text, float size, float width, float s,
                                               bool streaming = false) {
    namespace md = nickel::ai::md;
    struct Entry {
        std::string text;
        float size, width;
        md::Layout layout;
        const void* source = nullptr; // where the caller's text lived (the shared message text), for the index
    };
    // Index: the caller's text buffer (a message's text is shared and stays at one address), its size and the metrics.
    // A hit is checked against the kept text, so a buffer reused by another message is never mistaken for it.
    using Key = std::tuple<const void*, size_t, float, float>;
    constexpr size_t kKept = 256;
    static std::list<Entry> kept; // most recently used first
    static std::map<Key, std::list<Entry>::iterator> index;
    static Entry live;            // the streaming answer
    static double liveAt = -1;
    const double now = ImGui::GetTime();
    if (streaming && live.size == size && live.width == width && live.text.size() <= text.size()
        && (now - liveAt < 0.1 || live.text.size() == text.size()) && text.compare(0, live.text.size(), live.text) == 0) {
        return live.layout; // also while a stalled stream sends nothing new
    }
    const Key key{text.data(), text.size(), size, width};
    if (!streaming) {
        if (auto found = index.find(key); found != index.end()) {
            if (found->second->text == text) {
                kept.splice(kept.begin(), kept, found->second);
                return kept.front().layout;
            }
            kept.erase(found->second); // that buffer now holds another text
            index.erase(found);
        }
    }
    const float px = size * FontEmFactor;
    ImFont* fonts[2] = {Sized(regular, px), Sized(semibold, px)};
    const md::Measure measure = [&](const std::string& t, bool bold) {
        return fonts[bold ? 1 : 0]->CalcTextSizeA(px, FLT_MAX, 0, t.c_str(), t.c_str() + t.size()).x;
    };
    md::Metrics m;
    m.maxWidth = width;
    m.lineH = px + 1.5f * s;
    m.gap = 8 * s;
    m.itemGap = 2 * s;
    m.indentStep = 14 * s;
    m.markerW = 16 * s;
    if (streaming) {
        live = {text, size, width, md::LayOut(md::Parse(md::FontSafe(text)), m, measure)};
        liveAt = now;
        return live.layout;
    }
    kept.push_front({text, size, width, md::LayOut(md::Parse(md::FontSafe(text)), m, measure), text.data()});
    index[key] = kept.begin();
    if (kept.size() > kKept) {
        const Entry& oldest = kept.back();
        index.erase(Key{oldest.source, oldest.text.size(), oldest.size, oldest.width});
        kept.pop_back();
    }
    return kept.front().layout;
}

// Draws a laid-out answer from `at` (its top-left): bold runs in semibold, `code` tinted on a light background, bullets
// as dots, numbers, quotes with a bar on the left, code blocks on a band, rules as a thin line.
void DrawAiMarkdown(ImDrawList* d, const nickel::ai::md::Layout& laid, ImVec2 at, float size, float width, ImU32 ink,
                    float s, float a) {
    namespace md = nickel::ai::md;
    const float px = size * FontEmFactor, lineH = px + 1.5f * s;
    ImFont* fonts[2] = {Sized(regular, px), Sized(semibold, px)};
    const ImU32 soft = Rgba(160, 160, 172, a), codeInk = Rgba(240, 190, 140, a), codeBg = Rgba(255, 255, 255, .07f * a);
    const float right = at.x + std::min(width, laid.width);
    for (const md::Line& line : laid.lines) {
        const float top = at.y + line.y, left = at.x + line.indent;
        if (line.kind == md::BlockKind::Rule) {
            const float mid = std::round(top + 4 * s);
            d->AddLine(ImVec2(at.x, mid), ImVec2(right, mid), Rgba(60, 60, 72, a), 1 * s);
            continue;
        }
        if (line.kind == md::BlockKind::CodeLine) {
            d->AddRectFilled(ImVec2(at.x - 4 * s, top), ImVec2(right + 4 * s, top + lineH), codeBg);
        }
        if (line.kind == md::BlockKind::Quote) {
            d->AddRectFilled(ImVec2(at.x + 1 * s, top), ImVec2(at.x + 3 * s, top + lineH), Rgba(80, 80, 96, a));
        }
        if (line.first && line.kind == md::BlockKind::Bullet) {
            const ImVec2 dot(left - 9 * s, top + lineH * .5f);
            if (line.level % 2 == 0) {
                d->AddCircleFilled(dot, 2.2f * s, soft, 12);
            } else {
                d->AddCircle(dot, 2.2f * s, soft, 12, 1 * s);
            }
        }
        if (line.first && line.kind == md::BlockKind::Numbered) {
            const float w = fonts[0]->CalcTextSizeA(px, FLT_MAX, 0, line.marker.c_str()).x;
            d->AddText(fonts[0], px, ImVec2(std::round(left - 5 * s - w), std::round(top - size * .1f)), soft,
                       line.marker.c_str());
        }
        for (const md::Run& run : line.runs) {
            const float x = left + run.x;
            if (run.code && line.kind != md::BlockKind::CodeLine) {
                d->AddRectFilled(ImVec2(x - 2 * s, top), ImVec2(x + run.w + 2 * s, top + lineH), codeBg, 3 * s);
            }
            const ImU32 color = run.code ? codeInk : line.kind == md::BlockKind::Quote ? soft : ink;
            d->AddText(fonts[run.bold ? 1 : 0], px, ImVec2(std::round(x), std::round(top - size * .1f)), color,
                       run.text.c_str(), run.text.c_str() + run.text.size());
        }
    }
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
        // Wrap: move the soft breaks with the edit (common start and end of the old and new text), turn them back
        // into spaces, then break again at the last space before each line overflows. A break replaces a space by
        // "\n" (same length), so the cursor does not move.
        const std::string now(data->Buf, static_cast<size_t>(data->BufTextLen));
        const std::string& old = m.inputBefore;
        size_t head = 0;
        while (head < old.size() && head < now.size() && old[head] == now[head]) ++head;
        size_t tail = 0;
        while (tail < old.size() - head && tail < now.size() - head
               && old[old.size() - 1 - tail] == now[now.size() - 1 - tail]) ++tail;
        const int shift = static_cast<int>(now.size()) - static_cast<int>(old.size());
        std::vector<int> kept;
        for (int at : m.softBreaks) {
            if (at < static_cast<int>(head)) kept.push_back(at);
            else if (at >= static_cast<int>(old.size() - tail)) kept.push_back(at + shift);
        }
        for (int at : kept) {
            if (at >= 0 && at < data->BufTextLen && data->Buf[at] == '\n') data->Buf[at] = ' ';
        }
        m.softBreaks.clear();
        ImFont* font = ImGui::GetFont();
        const float size = ImGui::GetFontSize();
        int lineStart = 0, lastSpace = -1;
        for (int i = 0; i < data->BufTextLen; ++i) {
            const char c = data->Buf[i];
            if (c == '\n') {
                lineStart = i + 1;
                lastSpace = -1;
                continue;
            }
            if (c == ' ') lastSpace = i;
            const float w = font->CalcTextSizeA(size, FLT_MAX, 0, data->Buf + lineStart, data->Buf + i + 1).x;
            if (m.inputWrap > 0 && w > m.inputWrap && lastSpace > lineStart) {
                data->Buf[lastSpace] = '\n';
                m.softBreaks.push_back(lastSpace);
                lineStart = lastSpace + 1;
                lastSpace = -1;
            }
        }
        data->BufDirty = true;
        m.inputBefore.assign(data->Buf, static_cast<size_t>(data->BufTextLen));
    }
    return 0;
}

// The arrow between a current and a new value on a card.
void AiArrow(ImDrawList* d, ImVec2 at, float s, ImU32 c) {
    d->AddLine(at, ImVec2(at.x + 10 * s, at.y), c, 1.4f * s);
    d->AddLine(ImVec2(at.x + 6 * s, at.y - 3.5f * s), ImVec2(at.x + 10 * s, at.y), c, 1.4f * s);
    d->AddLine(ImVec2(at.x + 6 * s, at.y + 3.5f * s), ImVec2(at.x + 10 * s, at.y), c, 1.4f * s);
}

// A small rounded button of the chat window: style 0 neutral, 1 accent, 3 red; disabled = 40 %. True on a click.
bool AiButton(ImDrawList* d, const std::string& id, const char* label, ImVec2 lo, float width, float height, int style,
              bool enabled, float a, float s) {
    const ImVec2 hi(lo.x + width * s, lo.y + height * s);
    const bool clicked = Hit(("##" + id).c_str(), lo, ImVec2(width * s, height * s)) && enabled;
    const bool over = enabled && ImGui::IsItemHovered();
    const float glow = Animate("ai-button" + id, over ? 1.f : 0.f, ImGui::GetIO().DeltaTime);
    const float fade = a * (enabled ? 1.f : .4f);
    const ImU32 fill = style == 3   ? Rgba(240, 86, 106, (.15f + .09f * glow) * fade)
                       : style == 1 ? ToU32(MixColor(themeLive.accentDeep, ImVec4(1, 1, 1, 1), .14f * glow), fade)
                                    : Rgba(255, 255, 255, (.07f + .05f * glow) * fade);
    d->AddRectFilled(lo, hi, fill, 8 * s);
    const float size = 13;
    const ImU32 ink = style == 3   ? Rgba(246, 160, 171, fade)
                      : style == 1 ? Rgba(255, 255, 255, fade)
                                   : Rgba(230, 230, 235, fade);
    Label(d, semibold, size * s, ImVec2(lo.x + (width - TextWidth(semibold, size, label)) / 2 * s,
                                        lo.y + (height / 2 - .7f * size) * s),
          label, ink);
    return clicked;
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
    const float bw = 72;
    const float bh = 28;
    const int left = static_cast<int>(page.SecondsLeft(call));
    char timer[48];
    std::snprintf(timer, sizeof(timer), "Declines in %d:%02d", left / 60, left % 60);
    Label(d, regular, 11.5f * s, ImVec2(x0, y + (bh * s - 13 * s) / 2), timer, Rgba(120, 120, 132, a));
    const std::string id = call.use.id;
    // a card that just appeared or moved (the one above it was answered, the chat scrolled) is not clickable yet
    page.PlaceCard(id, p.x, p.y);
    const bool armed = page.Armed(call);
    const ImVec2 allowAt(p.x + w - pad - bw * s, y);
    const ImVec2 declineAt(allowAt.x - bw * s - 6 * s, y);
    const char* yes = card.isScriptWrite ? "Save" : "Allow";
    const bool allow = AiButton(d, "ai-allow" + id, yes, allowAt, bw, bh, 1, armed, a, s);
    const bool decline = AiButton(d, "ai-decline" + id, "Decline", declineAt, bw, bh, 0, armed, a, s);
    y += bh * s + pad;
    d->ChannelsSetCurrent(0);
    d->AddRectFilled(p, ImVec2(p.x + w, y), Rgba(23, 23, 29, a), 14 * s);
    d->AddRect(p, ImVec2(p.x + w, y), Acc(.6f * a), 14 * s, 0, 1.2f * s);
    d->ChannelsMerge();
    if (allow) {
        page.Allow(id); // the card is gone from the next frame
    } else if (decline) {
        page.Decline(id);
    }
    return y - p.y;
}

// ---- the bubble and the chat window

// Keeps where the player put the bubble ([ai] bubble= in settings.ini, fractions of the screen), or forgets it
// (`placed` false: the default place beside the menu again).
void AiKeepPlace(bool placed, float fx, float fy) {
    AiMenu& m = Ai();
    m.placed = placed;
    m.placeX = fx;
    m.placeY = fy;
    const std::wstring path = AiSettingPath();
    CreateDirectoryW(path.substr(0, path.find_last_of(L"\\/")).c_str(), nullptr);
    WritePrivateProfileStringW(L"ai", L"bubble", placed ? Wide(nickel::ai::WriteBubblePlace(fx, fy)).c_str() : nullptr,
                               path.c_str());
}

// The bubble and window of this frame: dragged, at the player's place, or beside the menu (from the menu's place on
// screen) while never moved; always inside the screen, whatever its size this frame. DrawMenu asks before it draws
// anything, so the sidebar can keep room for a bubble placed inside it and the menu ignores the mouse over the bubble
// or the window.
nickel::ai::BubbleLayout AiLayout(ImVec2 origin, float s) {
    namespace ai = nickel::ai;
    AiMenu& m = Ai();
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const ai::UiRect menu{origin.x, origin.y, menuW * s, menuH * s};
    ai::BubbleLayout place;
    if (m.headerDrag.Dragging()) {
        place = ai::DragTogether(m.dragFrom, m.headerDrag.DX(), m.headerDrag.DY(), screen.x, screen.y);
    } else if (m.bubbleDrag.Dragging()) {
        place = ai::DragBubble(m.dragFrom, m.bubbleDrag.DX(), m.bubbleDrag.DY(), s, screen.x, screen.y);
    } else if (m.placed) {
        place = ai::PlaceBubbleAt(m.placeX, m.placeY, s, screen.x, screen.y, m.sides);
    } else {
        place = ai::PlaceBubble(menu, s, screen.x, screen.y);
    }
    place.windowOverMenu = place.window.Overlaps(menu);
    m.sides = place.sides;
    return place;
}

// The end of a press on the bubble or the header (the mouse button is up): a drag keeps the new place, a click on the
// bubble toggles the window. While the button is held, the drag follows the mouse.
void AiDragTick(const nickel::ai::BubbleLayout& place, float s) {
    namespace ai = nickel::ai;
    AiMenu& m = Ai();
    const ImGuiIO& io = ImGui::GetIO();
    for (ai::DragGesture* gesture : {&m.bubbleDrag, &m.headerDrag}) {
        if (!gesture->Pressed()) {
            continue;
        }
        if (io.MouseDown[0]) {
            if (ImGui::IsMousePosValid()) {
                gesture->Move(io.MousePos.x, io.MousePos.y);
            }
            continue;
        }
        if (gesture->Dragging()) {
            const float w = io.DisplaySize.x, h = io.DisplaySize.y;
            const ai::BubbleLayout last = gesture == &m.headerDrag
                                                  ? ai::DragTogether(m.dragFrom, gesture->DX(), gesture->DY(), w, h)
                                                  : ai::DragBubble(m.dragFrom, gesture->DX(), gesture->DY(), s, w, h);
            float fx = 0, fy = 0;
            ai::BubbleFraction(last.bubble, io.DisplaySize.x, io.DisplaySize.y, &fx, &fy);
            AiKeepPlace(true, fx, fy);
            m.sides = last.sides;
        }
        const bool click = gesture->Release();
        if (click && gesture == &m.bubbleDrag && place.bubble.Contains(io.MousePos.x, io.MousePos.y)) {
            m.bubble.Click();
        }
    }
}

// True while the mouse is over the bubble or the open chat window: the menu under them must not start a drag or resize.
bool AiCovers(const nickel::ai::BubbleLayout& place, ImVec2 mouse) {
    if (!AiOption().value) {
        return false;
    }
    return place.bubble.Contains(mouse.x, mouse.y)
           || (Ai().bubble.WindowOpen() && place.window.Contains(mouse.x, mouse.y));
}

// Units the sidebar keeps free at its bottom when the bubble sits inside it (0 otherwise).
float AiSidebarReserve(const nickel::ai::BubbleLayout& place) {
    return AiOption().value && place.bubbleInSidebar ? nickel::ai::kBubbleSize + nickel::ai::kBubbleInset + 6 : 0.f;
}

// The Nickel mark (the logo texture of the menu header, branding/nickel/logo/nickel-mark-256.png) in a filled circle:
// the bubble and the assistant's avatar. Without the texture, an "N".
void DrawAiMark(ImDrawList* d, ImTextureID logo, ImVec2 c, float r, ImU32 fill, float a) {
    d->AddCircleFilled(c, r, fill, 40);
    if (logo) {
        const float half = r * .66f;
        d->AddImage(logo, ImVec2(c.x - half, c.y - half), ImVec2(c.x + half, c.y + half), ImVec2(0, 0), ImVec2(1, 1),
                    Rgba(255, 255, 255, a));
    } else {
        const float size = r * 1.1f;
        Label(d, bold, size, ImVec2(c.x - TextWidth(bold, size, "N") / 2, c.y - size * .55f), "N",
              Rgba(255, 255, 255, a));
    }
}

// The player's default avatar: a neutral grey circle with a head and shoulders.
void DrawAiPlayerAvatar(ImDrawList* d, ImVec2 c, float r, float a) {
    d->AddCircleFilled(c, r, Rgba(52, 52, 62, a), 40);
    const ImU32 ink = Rgba(150, 150, 162, a);
    d->AddCircleFilled(ImVec2(c.x, c.y - r * .22f), r * .3f, ink, 24);
    d->PathArcTo(ImVec2(c.x, c.y + r * .62f), r * .52f, 3.14159265f, 6.28318531f, 20);
    d->PathFillConvex(ink);
}

// Three dots rising one after the other: the assistant is writing.
void DrawAiTypingDots(ImDrawList* d, ImVec2 first, float s, float a) {
    const float t = static_cast<float>(ImGui::GetTime());
    for (int i = 0; i < 3; ++i) {
        const float wave = std::max(0.f, std::sin(t * 6.f - i * .9f));
        d->AddCircleFilled(ImVec2(first.x + i * 10 * s, first.y - wave * 3 * s), 3 * s,
                           Rgba(200, 200, 210, (.35f + .5f * wave) * a), 12);
    }
}

// The input at the bottom of the window: the field (Enter sends, Shift+Enter adds a line), the round send button (Stop
// while an answer runs), the character count near the limit and why a message cannot go now.
void DrawAiInput(AiMenu& m, ImVec2 origin, float W, float H, float s, float a) {
    namespace ai = nickel::ai;
    ai::ChatPage& page = *m.page;
    auto* d = ImGui::GetWindowDrawList();
    auto P = [&](float x, float y) {
        return ImVec2(origin.x + x * s, origin.y + y * s);
    };
    const float boxH = 64, left = 12, boxW = W - 24, top = H - 12 - boxH, button = 32;
    const std::string draft = AiInputText();
    const std::string blocked = page.SendBlockedReason(draft);
    const bool busy = page.Busy();
    d->AddRectFilled(P(left, top), P(left + boxW, top + boxH), Rgba(23, 23, 29, a), 14 * s);
    if (m.focusInput && page.Available()) {
        ImGui::SetKeyboardFocusHere(); // applies to the field submitted next
        m.focusInput = false;
    }
    ImGui::SetCursorScreenPos(P(left + 2, top + 2));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * s, 8 * s));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
    // Enter validates, Ctrl+Enter or Shift+Enter (no newline flag while Shift is down) adds a line
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackEdit
                                | ImGuiInputTextFlags_CallbackAlways;
    if (!ImGui::GetIO().KeyShift) {
        flags |= ImGuiInputTextFlags_CtrlEnterForNewLine;
    }
    if (!page.Available()) {
        flags |= ImGuiInputTextFlags_ReadOnly;
    }
    const ImVec2 fieldSize((boxW - 4 - button - 10) * s, (boxH - 4) * s);
    m.inputWrap = fieldSize.x - 2 * 10 * s - 6 * s; // inside the frame padding, room for the cursor
    bool send = ImGui::InputTextMultiline("##ai-input", m.input, sizeof(m.input), fieldSize, flags, AiInputCallback);
    const bool editing = ImGui::IsItemActive();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
    if (send) {
        m.focusInput = true; // Enter took the focus away; keep typing in the field
    }
    d->AddRect(P(left, top), P(left + boxW, top + boxH), editing ? Acc(.55f * a) : Rgba(40, 40, 50, a), 14 * s, 0,
               1 * s);
    if (m.input[0] == 0) {
        const char* hint = page.Available() ? "Message Nickel AI..." : "Nickel AI is not available in this build.";
        Label(d, regular, 13.5f * s, P(left + 12, top + 11), hint, Rgba(110, 110, 122, a));
    }
    // send (or stop): a round button in the field's bottom-right corner
    const ImVec2 c = P(left + boxW - 8 - button / 2, top + boxH - 8 - button / 2);
    const float r = button / 2 * s;
    const bool pressed = Hit("##ai-send", ImVec2(c.x - r, c.y - r), ImVec2(2 * r, 2 * r));
    const bool over = ImGui::IsItemHovered();
    if (busy) {
        d->AddCircleFilled(c, r, Rgba(240, 86, 106, (over ? .3f : .2f) * a), 32);
        d->AddRectFilled(ImVec2(c.x - 5 * s, c.y - 5 * s), ImVec2(c.x + 5 * s, c.y + 5 * s), Rgba(246, 160, 171, a),
                         2 * s);
        if (pressed) {
            page.Cancel();
        }
    } else {
        const bool ready = blocked.empty();
        const ImU32 fill = ready ? (over ? AccHover(a) : AccDeep(a)) : Rgba(255, 255, 255, .08f * a);
        d->AddCircleFilled(c, r, fill, 32);
        const ImU32 ink = ready ? Rgba(255, 255, 255, a) : Rgba(120, 120, 132, a);
        const float th = 2 * s;
        d->AddLine(ImVec2(c.x, c.y + 6 * s), ImVec2(c.x, c.y - 6 * s), ink, th);
        d->AddLine(ImVec2(c.x - 5 * s, c.y - 1 * s), ImVec2(c.x, c.y - 6 * s), ink, th);
        d->AddLine(ImVec2(c.x + 5 * s, c.y - 1 * s), ImVec2(c.x, c.y - 6 * s), ink, th);
        if (pressed && ready) {
            send = true;
        }
    }
    // above the field: the character count near the limit (right), why the message cannot go now (left)
    const size_t chars = ai::CountChars(m.input);
    float countW = 0;
    if (chars + 500 > ai::kMaxMessageChars) {
        const std::string count = std::to_string(chars) + "/" + std::to_string(ai::kMaxMessageChars);
        countW = TextWidth(regular, 11.5f, count.c_str());
        Label(d, regular, 11.5f * s, P(left + boxW - 2 - countW, top - 17), count.c_str(), Rgba(148, 148, 160, a));
    }
    if (!blocked.empty() && !busy && (!ai::Trim(draft).empty() || page.QuotaUsedUp())) {
        const std::string why = Fit(regular, 11.5f, blocked, boxW - countW - 12);
        Label(d, regular, 11.5f * s, P(left + 2, top - 17), why.c_str(), Rgba(240, 190, 90, a));
    }
    if (send && !busy && page.Send(draft)) {
        m.input[0] = 0;
        m.softBreaks.clear();
        m.inputBefore.clear();
        m.clearInput = true;
        m.focusInput = true;
    }
    if (!m.reviewSend.empty() && page.CanSend(m.reviewSend) && page.Send(m.reviewSend)) {
        m.reviewSend.clear();
    }
}

// The chat window: header (mark, "Nickel AI", quota, New chat, close), the conversation, the input. Dragging the
// header's free part moves the window and the bubble together; a double-click on it puts both back beside the menu.
void DrawAiWindow(const nickel::ai::BubbleLayout& place, float s, float opacity, ImTextureID logo) {
    namespace ai = nickel::ai;
    AiMenu& m = Ai();
    ai::ChatPage& page = *m.page;
    const ai::UiRect& rect = place.window;
    const float W = rect.w / s, H = rect.h / s, headH = 58;
    const ImVec2 origin(rect.x, rect.y);
    auto P = [&](float x, float y) {
        return ImVec2(origin.x + x * s, origin.y + y * s);
    };
    ImGui::SetNextWindowPos(origin);
    ImGui::SetNextWindowSize(ImVec2(rect.w, rect.h));
    ImGui::Begin("##NickelAiChat", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
                         | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse
                         | (opened.load() ? 0 : ImGuiWindowFlags_NoInputs));
    WindowFont(s * 13.5f * FontEmFactor);
    auto* d = ImGui::GetWindowDrawList();
    const float radius = 16 * s;
    // opaque: at 98 % the HUD and the game showed through (a Keystrokes box drawn under the window looked like an
    // empty grey rectangle in the conversation)
    d->AddRectFilled(origin, P(W, H), Rgba(15, 15, 20, opacity), radius);
    d->AddRect(origin, P(W, H), Rgba(26, 26, 33, opacity), radius, 0, 1 * s);
    const bool opening = m.bubble.TakeOpening();
    if (opening) {
        m.focusInput = true;
    }
    const ai::ConversationSnapshot& conv = page.Conversation();

    // ---- header (its free part, left of New chat, is the handle that moves the window and the bubble)
    Hit("##ai-header", origin, ImVec2((W - 130) * s, headH * s));
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        AiKeepPlace(false, 0, 0); // back beside the menu
        m.sides = {};
        m.headerDrag.Cancel();
    } else if (ImGui::IsItemActivated()) {
        m.headerDrag.Press(io.MousePos.x, io.MousePos.y);
        m.dragFrom = place;
    }
    if (ImGui::IsItemHovered() || m.headerDrag.Dragging()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }
    DrawAiMark(d, logo, P(32, headH / 2), 17 * s, Rgba(28, 28, 36, opacity), opacity);
    Label(d, semibold, 15 * s, P(58, 12), "Nickel AI", Rgba(255, 255, 255, opacity));
    Label(d, regular, 12 * s, P(58, 32), page.QuotaLine().c_str(), Rgba(148, 148, 160, opacity));
    const float closeX = W - 12 - 28, rowY = (headH - 28) / 2;
    const bool closing = Hit("##ai-close", P(closeX, rowY), ImVec2(28 * s, 28 * s));
    const float closeGlow = Animate("ai-close", ImGui::IsItemHovered() ? 1.f : 0.f, ImGui::GetIO().DeltaTime);
    if (closeGlow > .001f) {
        d->AddCircleFilled(P(closeX + 14, rowY + 14), 14 * s, Rgba(255, 255, 255, .08f * closeGlow * opacity), 24);
    }
    {
        const ImVec2 c = P(closeX + 14, rowY + 14);
        const float k = 5 * s;
        const ImU32 ink = Rgba(200, 200, 210, opacity);
        d->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), ink, 1.6f * s);
        d->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), ink, 1.6f * s);
    }
    const bool hasChat = !conv.messages.empty() || !page.Notes().empty();
    const float newW = 78;
    if (AiButton(d, "ai-new-chat", "New chat", P(closeX - 6 - newW, rowY), newW, 28, 0, page.Available() && hasChat,
                 opacity, s)) {
        page.NewChat();
        m.seenItems = 0;
    }
    d->AddLine(P(0, headH), P(W, headH), Rgba(26, 26, 33, opacity), 1 * s);

    // ---- input (drawn first so a message sent this frame shows in the conversation below)
    DrawAiInput(m, origin, W, H, s, opacity);

    // ---- conversation
    const float chatTop = headH + 1, chatBottom = H - 12 - 64 - 22;
    ImGui::SetCursorScreenPos(P(0, chatTop));
    ImGui::BeginChild("##ai-chat", ImVec2(W * s, (chatBottom - chatTop) * s), false,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    d = ImGui::GetWindowDrawList();
    const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 8 * s;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float x0 = start.x + 12 * s, cw = (W - 24) * s;
    const float avatarR = 14 * s, lane = 2 * avatarR + 8 * s, bubbleMax = cw - lane - 28 * s;
    const float padX = 12 * s, padY = 9 * s, round = 14 * s, textSize = 13.5f * s;
    const std::vector<ai::ChatItem> items = page.Timeline();
    float y = start.y + 14 * s;
    for (size_t k = 0; k < items.size(); ++k) {
        const ai::ChatItem& item = items[k];
        if (item.groupStart && k > 0) {
            y += 8 * s; // a little more room between turns
        }
        const bool player = item.side == ai::ChatSide::Player;
        float h = 0;
        switch (item.kind) {
            case ai::ChatItem::Kind::Greeting:
            case ai::ChatItem::Kind::Message: {
                const std::string& text =
                        item.kind == ai::ChatItem::Kind::Greeting ? item.text : conv.messages[item.message].text.str();
                const float px = textSize * FontEmFactor;
                // the assistant's answers are Markdown (ai_markdown.hpp); the player's text and the greeting are plain
                const bool markdown = !player && item.kind == ai::ChatItem::Kind::Message;
                const ai::md::Layout* laid = markdown ? &AiMarkdownLayout(text, textSize, bubbleMax - 2 * padX, s, item.streaming) : nullptr;
                const ImVec2 box = laid ? ImVec2(laid->width, laid->height)
                                        : Sized(regular, px)->CalcTextSizeA(px, FLT_MAX, bubbleMax - 2 * padX, text.c_str(),
                                                                            text.c_str() + text.size());
                const float bw = std::min(bubbleMax, box.x + 2 * padX + (item.streaming ? 10 * s : 0));
                h = std::max(box.y + 2 * padY, 2 * avatarR);
                const float x = player ? x0 + cw - lane - bw : x0 + lane;
                const ImU32 fill = player ? AccDeep(opacity) : Rgba(28, 28, 36, opacity);
                d->AddRectFilled(ImVec2(x, y), ImVec2(x + bw, y + h), fill, round);
                const ImU32 ink = player ? Rgba(255, 255, 255, opacity) : Rgba(228, 228, 235, opacity);
                if (laid) {
                    DrawAiMarkdown(d, *laid, ImVec2(x + padX, y + (h - box.y) / 2), textSize, bubbleMax - 2 * padX, ink,
                                   s, opacity);
                } else {
                    AiText(d, regular, textSize, ImVec2(x + padX, y + (h - box.y) / 2), text, ink, bubbleMax - 2 * padX);
                }
                if (item.streaming && std::fmod(ImGui::GetTime(), 1.0) < .6) { // a blinking caret while it streams
                    const ImVec2 caret(x + bw - padX - 3 * s, y + h - padY - 13 * s);
                    d->AddRectFilled(caret, ImVec2(caret.x + 2 * s, caret.y + 13 * s), Acc(opacity));
                }
                break;
            }
            case ai::ChatItem::Kind::Typing: {
                const std::string caption = Fit(regular, 12, item.text, (bubbleMax - 70 * s) / s);
                const float captionW = caption.empty() ? 0 : TextWidth(regular, 12, caption.c_str()) * s + 8 * s;
                const float bw = 56 * s + captionW;
                h = 2 * avatarR + 6 * s;
                const float x = x0 + lane;
                d->AddRectFilled(ImVec2(x, y), ImVec2(x + bw, y + h), Rgba(28, 28, 36, opacity), round);
                DrawAiTypingDots(d, ImVec2(x + 18 * s, y + h / 2 + 1 * s), s, opacity);
                if (!caption.empty()) {
                    Label(d, regular, 12 * s, ImVec2(x + 50 * s, y + h / 2 - 7 * s), caption.c_str(),
                          Rgba(148, 148, 160, opacity));
                }
                break;
            }
            case ai::ChatItem::Kind::Card:
                h = DrawAiCard(d, page, *item.call, ImVec2(x0 + lane, y), cw - lane, s, opacity);
                break;
            case ai::ChatItem::Kind::Note: {
                const ai::ChatNote& note = page.Notes()[item.note];
                const ImU32 dot = note.kind == ai::ChatNote::Kind::Applied  ? Acc(opacity)
                                  : note.kind == ai::ChatNote::Kind::Failed ? Rgba(246, 160, 171, opacity)
                                                                            : Rgba(148, 148, 160, opacity);
                const std::string text = Fit(regular, 11.5f, note.text, cw / s - 24);
                const float tw = TextWidth(regular, 11.5f, text.c_str()) * s, x = x0 + (cw - tw - 10 * s) / 2;
                d->AddCircleFilled(ImVec2(x + 2.5f * s, y + 8 * s), 2.5f * s, dot, 12);
                Label(d, regular, 11.5f * s, ImVec2(x + 10 * s, y + 1 * s), text.c_str(), Rgba(160, 160, 172, opacity));
                h = 17 * s;
                break;
            }
            case ai::ChatItem::Kind::System: {
                const float size = 12 * s, wrap = cw * .86f, px = size * FontEmFactor;
                const ImVec2 box = Sized(regular, px)->CalcTextSizeA(px, FLT_MAX, wrap, item.text.c_str(),
                                                                     item.text.c_str() + item.text.size());
                const ImU32 ink = item.failure ? Rgba(246, 160, 171, opacity) : Rgba(148, 148, 160, opacity);
                AiText(d, regular, size, ImVec2(x0 + (cw - box.x) / 2, y + 2 * s), item.text, ink, wrap);
                h = box.y + 4 * s;
                break;
            }
        }
        if (item.groupStart && item.side != ai::ChatSide::Centre) {
            const ImVec2 c(player ? x0 + cw - avatarR : x0 + avatarR, y + avatarR);
            if (player) {
                DrawAiPlayerAvatar(d, c, avatarR, opacity);
            } else {
                DrawAiMark(d, logo, c, avatarR, Rgba(28, 28, 36, opacity), opacity);
            }
        }
        y += h + 6 * s;
    }
    ImGui::SetCursorScreenPos(ImVec2(start.x, y + 4 * s));
    ImGui::Dummy(ImVec2(1, 1));
    // follow the conversation while the player is at the bottom (new items, streamed text); jump there on opening
    const size_t lastText = conv.messages.empty() ? 0 : conv.messages.back().text.size();
    const size_t signature = items.size() * 1000003u + lastText;
    if (opening || (signature != m.seenItems && atBottom)) {
        ImGui::SetScrollHereY(1.f);
    }
    m.seenItems = signature;
    const float listScroll = ImGui::GetScrollY(), listLimit = ImGui::GetScrollMaxY();
    FadeEdges(s, opacity);
    EndChildWindow();
    // thin accent scrollbar at the window's right edge, only when the conversation overflows
    if (listLimit > 0) {
        const float view = chatBottom - chatTop, total = view + listLimit / s;
        const float bar = std::max(30.f, view * view / total);
        const float barTop = chatTop + (view - bar) * (listScroll / listLimit);
        ImGui::GetWindowDrawList()->AddRectFilled(P(W - 6, barTop + 6), P(W - 3, barTop + bar - 6),
                                                  Acc(.85f * opacity), 1.5f * s);
    }
    EndWindow();
    if (closing) {
        m.bubble.Close();
    }
}

// The notice of AiTick (answer done, failed, a card waiting) while the chat window is hidden: a dark pill at the top of
// the screen for a few seconds, over the game whether the menu is open or not, fading in and out.
void AiDrawToast(float menuOpacity) {
    AiMenu& m = Ai();
    const double now = ImGui::GetTime();
    if (m.toastUntil <= now || m.toast.text.empty() || !AiOption().value) {
        return;
    }
    const double left = m.toastUntil - now, shown = (m.toast.kind == nickel::ai::PageNotice::Kind::NeedsYou ? 12.0 : 7.0) - left;
    const float a = static_cast<float>(std::clamp(std::min(shown / .25, left / .4), 0.0, 1.0));
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float s = std::clamp(screen.y / 1080.f, .7f, 1.6f);
    const char* hint = menuOpacity > .05f ? "Open the Nickel AI bubble to read it." : "Right Shift, then the Nickel AI bubble.";
    const float titleW = TextWidth(semibold, 14.f, m.toast.text.c_str()), hintW = TextWidth(regular, 12.f, hint);
    const float w = (std::max(titleW, hintW) + 52) * s, h = 50 * s;
    const ImVec2 lo((screen.x - w) / 2, 18 * s - (1 - a) * 8 * s), hi(lo.x + w, lo.y + h);
    ImDrawList* d = ImGui::GetForegroundDrawList();
    d->AddRectFilled(ImVec2(lo.x, lo.y + 2 * s), ImVec2(hi.x, hi.y + 2 * s), Rgba(0, 0, 0, .35f * a), 14 * s);
    d->AddRectFilled(lo, hi, Rgba(15, 15, 20, .96f * a), 14 * s);
    d->AddRect(lo, hi, Rgba(38, 38, 48, a), 14 * s, 0, 1 * s);
    const ImU32 dot = m.toast.kind == nickel::ai::PageNotice::Kind::Failed ? Rgba(246, 160, 171, a) : Acc(a);
    d->AddCircleFilled(ImVec2(lo.x + 20 * s, lo.y + h / 2), 4.5f * s, dot, 16);
    Label(d, semibold, 14.f * s, ImVec2(lo.x + 34 * s, lo.y + 8 * s), m.toast.text.c_str(), Rgba(255, 255, 255, a));
    Label(d, regular, 12.f * s, ImVec2(lo.x + 34 * s, lo.y + 27 * s), hint, Rgba(148, 148, 160, a));
}

// Called by DrawMenu after the menu and its side panels while Nickel AI is on: the page's tick, the chat window when it
// is open, then the bubble. The page (client, worker, install key) is created the first time the window opens.
void DrawAiBubble(const nickel::ai::BubbleLayout& place, float s, float opacity, ImTextureID logo) {
    namespace ai = nickel::ai;
    AiMenu& m = Ai();
    m.windowVisible = false; // set again below when the window shows; AiTick reads it next frame
    AiDrawToast(opacity);
    if (!ai::BubbleState::BubbleShown(AiOption().value, opacity > 0)) {
        return;
    }
    if (m.bubble.WindowOpen() && !m.created) {
        m.created = true;
        if (ai::Factory() != nullptr) {
            ai::Services services = ai::Factory()();
            m.client = std::move(services.client);
            m.tools = std::move(services.tools);
        }
        m.page = std::make_unique<ai::ChatPage>(m.client.get(), m.tools.get());
    }
    // the page ticks in AiTick every frame; here only whether the window shows (cards answerable, notices off)
    m.windowVisible = m.bubble.WindowOpen() && opened.load() && opacity > .05f;
    if (m.bubble.WindowOpen() && m.page) {
        DrawAiWindow(place, s, opacity, logo);
    }
    // the bubble in its own small window (a few pixels larger for the ring), above the menu
    const ai::UiRect& b = place.bubble;
    const float margin = 4 * s;
    ImGui::SetNextWindowPos(ImVec2(b.x - margin, b.y - margin));
    ImGui::SetNextWindowSize(ImVec2(b.w + 2 * margin, b.h + 2 * margin));
    ImGui::Begin("##NickelAiBubble", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
                         | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse
                         | (opened.load() ? 0 : ImGuiWindowFlags_NoInputs));
    auto* d = ImGui::GetWindowDrawList();
    // a press that moves past kDragThreshold drags the bubble (the window follows); a shorter one is a click
    // (AiDragTick)
    Hit("##ai-bubble", ImVec2(b.x, b.y), ImVec2(b.w, b.h));
    const bool over = ImGui::IsItemHovered();
    if (ImGui::IsItemActivated()) {
        m.bubbleDrag.Press(ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y);
        m.dragFrom = place;
    }
    if (m.bubbleDrag.Dragging()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    } else if (over) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    const float glow = Animate("ai-bubble", over ? 1.f : 0.f, ImGui::GetIO().DeltaTime);
    const bool open = m.bubble.WindowOpen();
    const ImVec2 c(b.x + b.w / 2, b.y + b.h / 2);
    const float r = b.w / 2;
    d->AddCircleFilled(ImVec2(c.x, c.y + 1.5f * s), r + 1 * s, Rgba(0, 0, 0, .35f * opacity), 48); // soft shadow
    DrawAiMark(d, logo, c, r, Rgba(15, 15, 20, opacity), opacity);
    d->AddCircle(c, r, Rgba(26, 26, 33, opacity), 48, 1 * s);
    const float ring = std::max(glow, open ? .55f : 0.f);
    if (ring > .001f) {
        d->AddCircle(c, r + 1.5f * s, Acc(ring * opacity), 48, 2 * s);
    }
    // a closed window with an answer on its way, a card waiting or an answer not read yet: an accent dot
    if (!open && m.page && (m.page->Busy() || !m.page->OpenCards().empty() || m.unread)) {
        const ImVec2 dot(c.x + r * .72f, c.y - r * .72f);
        d->AddCircleFilled(dot, 5 * s, Rgba(15, 15, 20, opacity), 16);
        d->AddCircleFilled(dot, 3.5f * s, Acc(opacity), 16);
    }
    EndWindow();
    AiDragTick(place, s);
}

#endif // NICKEL_AI_UI_DRAW
