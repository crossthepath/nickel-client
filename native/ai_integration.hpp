#pragma once
// Nickel AI: binds the panel (ai_ui.hpp, which sees only ChatClient and ToolRunner of ai_types.hpp) to the network
// client (ai_client.hpp, AssistantClient) and to the client tools (ai_tools.hpp, tools::AssistantTools).
//
// - AssistantChat adapts AssistantClient's immutable Conversation snapshots to the panel's ConversationSnapshot.
// - AssistantToolRunner adapts AssistantTools: the confirmation card is built by AssistantTools::Describe from the
//   registry and the script library (never from the model's text), and an action tool runs only when the panel calls
//   Run after the player pressed Allow. The panel calls both on the render thread, so the tools that touch the
//   registry or start and stop scripts run there.
//
// No ImGui and no game here: tests/ai_e2e_checks.cpp drives the same classes against a local server.
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ai_client.hpp"
#include "ai_tools.hpp"
#include "ai_types.hpp"

namespace nickel::ai {

inline StopReason ParseStopReason(const std::string& text) {
    if (text == "end_turn") {
        return StopReason::EndTurn;
    }
    if (text == "tool_use") {
        return StopReason::ToolUse;
    }
    if (text == "max_tokens") {
        return StopReason::MaxTokens;
    }
    if (text == "refusal") {
        return StopReason::Refusal;
    }
    if (text == "limit") {
        return StopReason::Limit;
    }
    return StopReason::None;
}

// The panel's error code: the server's own code when it sent one, else a code the panel has a text for.
inline std::string PanelErrorCode(const ErrorInfo& error) {
    if (!error.code.empty()) {
        return error.code;
    }
    switch (error.kind) {
        case ErrorKind::None:
        case ErrorKind::Cancelled:
            return "";
        case ErrorKind::Offline:
        case ErrorKind::Timeout:
            return "offline";
        case ErrorKind::Unauthorized:
        case ErrorKind::NoIdentity:
            return "unauthorized";
        case ErrorKind::Blocked:
            return "blocked";
        case ErrorKind::QuotaExceeded:
            return "quota_exceeded";
        case ErrorKind::RateLimited:
            return "rate_limited";
        case ErrorKind::Invalid:
            return "invalid";
        default:
            return ErrorKindName(error.kind);
    }
}

inline TurnState PanelState(Status status) {
    switch (status) {
        case Status::Sending:
            return TurnState::Sending;
        case Status::Streaming:
            return TurnState::Streaming;
        case Status::AwaitingTools:
            return TurnState::AwaitingToolResults;
        case Status::Failed:
            return TurnState::Failed;
        default:
            return TurnState::Idle;
    }
}

// One snapshot of the client as the panel reads it. `toolRound` identifies the current tool step.
inline ConversationSnapshot ToPanelSnapshot(const Conversation& conversation, uint64_t revision, uint64_t toolRound) {
    ConversationSnapshot snap;
    snap.revision = revision;
    snap.conversationId = conversation.conversationId;
    snap.turnId = conversation.turnId;
    for (const auto& message : conversation.messages) {
        if (!message) {
            continue;
        }
        ChatMessage line;
        line.role = message->role == Role::User ? ChatMessage::Role::Player : ChatMessage::Role::Assistant;
        if (line.role == ChatMessage::Role::Assistant && message->text.empty()) {
            continue; // a step that only called tools: nothing to show
        }
        // shared with the client's immutable message (aliasing pointer): no copy of the text per snapshot
        line.text = message->truncated ? SharedText(message->text + "\n[The answer was too long and was cut.]")
                                       : SharedText(std::shared_ptr<const std::string>(message, &message->text));
        snap.messages.push_back(std::move(line));
    }
    snap.state = PanelState(conversation.status);
    snap.lastStop = ParseStopReason(conversation.stopReason);
    if (conversation.quota.used >= 0 && conversation.quota.limit >= 0) {
        snap.quota.known = true;
        // clamped: the panel computes limit - used in int
        constexpr int64_t kMaxQuota = 1000000;
        snap.quota.used = static_cast<int>((std::min)(static_cast<int64_t>(conversation.quota.used), kMaxQuota));
        snap.quota.limit = static_cast<int>((std::min)(static_cast<int64_t>(conversation.quota.limit), kMaxQuota));
        snap.quota.resetAt = conversation.quota.resetAt;
    }
    if (conversation.status == Status::Failed) {
        snap.error.code = PanelErrorCode(conversation.error);
        snap.error.message = conversation.error.message;
        snap.error.httpStatus = conversation.error.httpStatus;
        snap.error.retryAfterSeconds = conversation.error.retryAfter;
    }
    if (conversation.status == Status::AwaitingTools) {
        snap.toolUses = conversation.pendingToolUses;
    }
    snap.toolRound = toolRound;
    return snap;
}

// The panel's ChatClient over AssistantClient. Render thread only (the client's own worker does the network).
class AssistantChat final : public ChatClient {
public:
    explicit AssistantChat(std::unique_ptr<AssistantClient> client) : client_(std::move(client)) {}

    ~AssistantChat() override {
        if (client_) {
            client_->Stop();
        }
    }

    AssistantClient& Client() { return *client_; }

    bool StartTurn(const std::string& message) override { return client_->Send(message); }

    bool SendToolResults(const std::vector<ToolResult>& results) override {
        const auto conversation = client_->Snap();
        lastSubmit_ = client_->SubmitToolResults(conversation->turnId, results);
        return lastSubmit_ == SubmitError::None;
    }

    void Cancel() override { client_->Cancel(); }

    void NewConversation() override { client_->NewConversation(); }

    uint64_t Revision() const override { return client_->Version(); }

    ConversationSnapshot Snapshot() const override {
        const auto conversation = client_->Snap();
        if (conversation->status == Status::AwaitingTools) {
            // A new step is a new turn id or a new set of tool_use ids (the service never reuses an id).
            std::string key = conversation->turnId;
            for (const auto& use : conversation->pendingToolUses) {
                key += "|" + use.id;
            }
            if (key != roundKey_) {
                roundKey_ = std::move(key);
                ++round_;
            }
        }
        return ToPanelSnapshot(*conversation, conversation->version, round_);
    }

    SubmitError LastSubmit() const { return lastSubmit_; }

private:
    std::unique_ptr<AssistantClient> client_;
    mutable std::string roundKey_;
    mutable uint64_t round_ = 0;
    SubmitError lastSubmit_ = SubmitError::None;
};

// The panel's ToolRunner over AssistantTools.
class AssistantToolRunner final : public ToolRunner {
public:
    explicit AssistantToolRunner(tools::Deps deps) : scripts_(deps.scripts), tools_(deps) {}

    tools::AssistantTools& Tools() { return tools_; }

    bool Describe(const ToolUse& use, ConfirmCard* card, ToolResult* refusal) override {
        const tools::ConfirmCard described = tools_.Describe(use.name, use.input);
        if (!described.ok) {
            *refusal = MakeToolError(use.id, described.error.empty() ? "refused" : described.error, described.message);
            return false;
        }
        card->toolUseId = use.id;
        card->tool = use.name;
        card->title = described.title;
        for (const auto& line : described.lines) {
            card->lines.push_back({line.what, line.before, line.after});
        }
        if (use.name == "write_script") {
            card->isScriptWrite = true;
            card->newScript = described.newScript;
            card->scriptName = described.script;
            if (!described.newScript) {
                overlay::ScriptSource current;
                if (scripts_.ReadSource(described.script, current)) {
                    card->currentSource = current.text;
                }
            }
            const json::Value* source = use.input.IsObject() ? use.input.Find("source") : nullptr;
            if (source != nullptr && source->IsString()) {
                card->newSource = source->AsString();
            }
            for (const auto& error : described.compileErrors) {
                card->compileErrors.push_back("line " + std::to_string(error.line) + ": " + error.message);
            }
            card->warning = described.wasRunning
                                    ? "This script is running: saving stops it until you start it again."
                                    : "Saved held: it does not run until you start it.";
        }
        if (use.name == "set_script_running") {
            card->scriptModules = described.declaredModules;
        }
        return true;
    }

    // The panel calls Run for an action tool only after Allow, so such a call is confirmed here.
    ToolResult Run(const ToolUse& use) override {
        const bool confirmed = tools::AssistantTools::NeedsConfirmation(use.name, use.input);
        const tools::ToolResult result = tools_.Run(use.name, use.input, confirmed);
        ToolResult out;
        out.toolUseId = use.id;
        out.content = result.content;
        out.isError = result.isError;
        return out;
    }

private:
    overlay::ScriptLibrary& scripts_;
    tools::AssistantTools tools_;
};

} // namespace nickel::ai
