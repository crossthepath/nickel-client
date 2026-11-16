#pragma once
// Nickel AI: the types the assistant panel (ai_ui.hpp) shares with the network client (ai_client.hpp) and the tool
// layer (ai_tools.hpp). It mirrors the service's protocol (https://nickelclient.com/api/ai/v1):
//
//   POST /turns                          {"conversation_id", "message" (1..4000), "client": {...}} -> SSE
//   POST /turns/{turn_id}/tool-results   {"results": [{"tool_use_id", "content" (JSON text, <= 16 KB), "is_error"}]}
//   SSE events: turn {turn_id, conversation_id, quota {used, limit, reset_at}}, text {delta},
//               tool_use {id, name, input}, done {stop_reason}, error {code, message, retry_after?, quota?}
//   GET /status                          {enabled, quota {used, limit, reset_at}, prompt_version}
//   A turn that fails before any of its output reached the player is not counted: the error's quota (or, from an
//   older service, GET /status asked after the failure) gives the count back.
//
// ToolUse, ToolResult and the protocol limits are shared with the network client (ai_client.hpp includes this file).
// The panel only sees two small interfaces: ChatClient (the conversation, read as a snapshot each frame) and
// ToolRunner (runs a tool call, or describes the change an action tool would make, from real data). The concrete
// classes adapt to them; nothing here does I/O. No ImGui, no game access: tests/ai_ui_checks.cpp uses fakes.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "json_value.hpp"

namespace nickel::ai {

// ---- limits of the protocol

constexpr size_t kMaxMessageChars = 4000;         // one player message, in Unicode characters (code points)
constexpr int kDefaultDailyLimit = 25;            // messages a day per install; the service's quota event is the truth
constexpr size_t kMaxToolResultBytes = 16 * 1024; // the JSON text of one tool result
constexpr double kConfirmTimeoutSeconds = 120;    // a card left unanswered is declined

// ---- tools

// The 12 tools of the contract. Action tools change something and always need the player's confirmation.
inline const std::vector<std::string>& ReadToolNames() {
    static const std::vector<std::string> names = {"search_modules", "get_module",    "get_client_status",
                                                   "list_scripts",   "read_script",   "validate_script",
                                                   "navigate_ui"};
    return names;
}

inline const std::vector<std::string>& ActionToolNames() {
    static const std::vector<std::string> names = {"set_setting",  "revert_changes", "set_module_enabled",
                                                   "write_script", "set_script_running"};
    return names;
}

inline bool IsActionTool(const std::string& name) {
    for (const auto& known : ActionToolNames()) {
        if (known == name) {
            return true;
        }
    }
    return false;
}

inline bool IsKnownTool(const std::string& name) {
    if (IsActionTool(name)) {
        return true;
    }
    for (const auto& known : ReadToolNames()) {
        if (known == name) {
            return true;
        }
    }
    return false;
}

// One tool_use event of the stream. `input` was validated by the service against the tool's schema.
struct ToolUse {
    std::string id;
    std::string name;
    json::Value input;
    std::string inputJson; // the same input as compact JSON text (filled by the network client)
};

// One entry of POST /tool-results. `content` is JSON text; errors are {"error": code, "message": text}.
struct ToolResult {
    std::string toolUseId;
    std::string content;
    bool isError = false;
};

inline ToolResult MakeToolError(const std::string& toolUseId, const std::string& code, const std::string& message) {
    json::Value body = json::Value::MakeObject();
    body.Set("error", json::Value::MakeString(code));
    body.Set("message", json::Value::MakeString(message));
    ToolResult result;
    result.toolUseId = toolUseId;
    result.content = json::Serialize(body);
    result.isError = true;
    return result;
}

// One change shown on a confirmation card: "Range: 3.0 -> 4.5". Built from the registry, never from model text.
struct ConfirmLine {
    std::string label;   // "Kill Aura > Range", "Kill Aura", "Script auto_gg.luau"
    std::string current; // the value now ("Off", "3.0", "" when there is none)
    std::string next;    // the value after the change
};

// What the panel shows before an action tool runs. ToolRunner::Describe fills it.
struct ConfirmCard {
    std::string toolUseId;
    std::string tool;    // the tool name
    std::string title;   // "Turn on Kill Aura", "Change 2 settings of Kill Aura", "Write script auto_gg"
    std::vector<ConfirmLine> lines;
    std::string warning; // optional: "Servers may detect this module."

    // write_script: the source diff. The script is saved held: it never runs until the player starts it.
    bool isScriptWrite = false;
    bool newScript = false;
    std::string scriptName;
    std::string currentSource; // empty for a new script
    std::string newSource;
    std::vector<std::string> compileErrors; // "line 4: expected 'end'"; the script can still be saved

    // set_script_running (start): the modules the script declares.
    std::vector<std::string> scriptModules;
};

// ---- the conversation, as the client reports it

enum class StopReason { None, EndTurn, ToolUse, MaxTokens, Refusal, Limit };

enum class TurnState {
    Idle,                // no turn running (none yet, or the last one ended)
    Sending,             // a POST is out, no event yet
    Streaming,           // events are arriving
    AwaitingToolResults, // "done" with stop_reason tool_use: the client waits for SendToolResults
    Failed,              // an error before or during the stream; `error` says which
};

// Server codes: unauthorized (401), blocked (403), quota_exceeded / rate_limited (429, Retry-After), invalid (400),
// and the codes of the stream's "error" event. Client codes: offline (no connection or the service is unreachable).
struct ClientError {
    std::string code;
    std::string message;
    int httpStatus = 0;
    int retryAfterSeconds = 0;
};

struct QuotaView {
    bool known = false; // false until the first "turn" event
    int used = 0;
    int limit = kDefaultDailyLimit;
    std::string resetAt; // ISO 8601, UTC
};

struct ChatMessage {
    enum class Role { Player, Assistant };
    Role role = Role::Player;
    std::string text; // an assistant message grows with each "text" delta
};

struct ConversationSnapshot {
    uint64_t revision = 0;
    std::string conversationId; // empty before the first turn
    std::string turnId;
    std::vector<ChatMessage> messages;
    TurnState state = TurnState::Idle;
    StopReason lastStop = StopReason::None;
    QuotaView quota;
    ClientError error;          // when state == Failed
    std::vector<ToolUse> toolUses; // the tool_use events of the step, when state == AwaitingToolResults
    uint64_t toolRound = 0;     // increases each time a step ends with tool_use
};

// The network side, as the panel uses it. Every call returns at once (the work is on the client's worker thread);
// the panel calls them on the render thread and reads Snapshot() when Revision() changed.
class ChatClient {
public:
    virtual ~ChatClient() = default;
    // Starts a turn with the player's message (already checked: 1..kMaxMessageChars characters). False when a turn
    // is already running.
    virtual bool StartTurn(const std::string& message) = 0;
    // Posts the results of every tool_use of the step, in the order of snapshot.toolUses.
    virtual bool SendToolResults(const std::vector<ToolResult>& results) = 0;
    // Stops the running turn; the conversation stays.
    virtual void Cancel() = 0;
    // Forgets the conversation (the next turn starts a new one).
    virtual void NewConversation() = 0;
    virtual uint64_t Revision() const = 0;
    virtual ConversationSnapshot Snapshot() const = 0;
};

// The tool side, as the panel uses it. Called on the render thread only, while the panel is visible.
class ToolRunner {
public:
    virtual ~ToolRunner() = default;
    // For an action tool: fills `card` from the registry and the script library. False when the call cannot run
    // (unknown module, invalid value...): `refusal` is then the result sent back, nothing is shown.
    virtual bool Describe(const ToolUse& use, ConfirmCard* card, ToolResult* refusal) = 0;
    // Runs the call: a read tool at once, an action tool only after the player allowed it.
    virtual ToolResult Run(const ToolUse& use) = 0;
};

} // namespace nickel::ai
