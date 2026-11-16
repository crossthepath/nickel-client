// Nickel AI panel logic without ImGui (native/ai_ui.hpp, nickel::ai::ChatPage) on a fake client and fake tools:
// sending and its limits (4000 characters, busy, quota, Retry-After), the activity line, read tools run at once,
// action tools wait for Allow / Decline on a card built by the tools (never from model text), the 2-minute timeout,
// nothing runs while the panel is hidden, all results of a step posted once in order, cancel and new chat, error texts,
// and the script diff. Then the chat window: the local greeting (never sent, not counted), the timeline (sides, order,
// avatars, typing indicator, errors as centred lines), the bubble that toggles the window, where both are placed, the
// bubble and window dragged anywhere (click or drag, kept on screen, the place saved as fractions of the screen), the
// quota after a refunded failure, and that the sidebar has no Nickel AI entry any more.
#include "ai_ui.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace nickel::ai;

int failed = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++failed;
    }
}

// The conversation as the real client would report it; the test moves it along.
class FakeClient : public ChatClient {
public:
    bool StartTurn(const std::string& message) override {
        if (snap.state == TurnState::Sending || snap.state == TurnState::Streaming
            || snap.state == TurnState::AwaitingToolResults) {
            return false;
        }
        started.push_back(message);
        snap.messages.push_back({ChatMessage::Role::Player, message});
        snap.state = TurnState::Sending;
        snap.error = {};
        Bump();
        return true;
    }

    bool SendToolResults(const std::vector<ToolResult>& results) override {
        ++attempts;
        if (refusePosts) {
            return false; // the state does not move: the page must not believe it was sent
        }
        posts.push_back(results);
        snap.state = TurnState::Sending;
        snap.toolUses.clear();
        Bump();
        return true;
    }

    void Cancel() override {
        ++cancels;
        snap.state = TurnState::Idle;
        snap.toolUses.clear();
        Bump();
    }

    void NewConversation() override {
        snap = ConversationSnapshot();
        Bump();
    }

    uint64_t Revision() const override { return snap.revision; }

    ConversationSnapshot Snapshot() const override { return snap; }

    // ---- the service's side
    void Turn(int used, int limit, const std::string& resetAt) {
        snap.turnId = "t1";
        snap.conversationId = "c1";
        snap.quota = {true, used, limit, resetAt};
        snap.state = TurnState::Streaming;
        snap.messages.push_back({ChatMessage::Role::Assistant, ""});
        Bump();
    }

    void Text(const std::string& delta) {
        if (snap.messages.empty() || snap.messages.back().role != ChatMessage::Role::Assistant) {
            snap.messages.push_back({ChatMessage::Role::Assistant, ""});
        }
        snap.state = TurnState::Streaming;
        snap.messages.back().text += delta;
        Bump();
    }

    void Tools(const std::vector<ToolUse>& uses) {
        snap.toolUses = uses;
        snap.state = TurnState::AwaitingToolResults;
        snap.lastStop = StopReason::ToolUse;
        ++snap.toolRound;
        Bump();
    }

    void Done(StopReason stop) {
        snap.state = TurnState::Idle;
        snap.lastStop = stop;
        Bump();
    }

    void Fail(const std::string& code, int status, int retryAfter) {
        snap.state = TurnState::Failed;
        snap.error = {code, "", status, retryAfter};
        Bump();
    }

    ConversationSnapshot snap;
    std::vector<std::string> started;
    std::vector<std::vector<ToolResult>> posts;
    int cancels = 0;
    int attempts = 0;
    bool refusePosts = false;

private:
    void Bump() { ++snap.revision; }
};

// A registry with Kill Aura (Range 3.0) and a script library with auto_gg; records what ran.
class FakeTools : public ToolRunner {
public:
    bool Describe(const ToolUse& use, ConfirmCard* card, ToolResult* refusal) override {
        ++describes;
        const std::string module = use.input["module_id"].AsString();
        if ((use.name == "set_setting" || use.name == "set_module_enabled") && module != "kill_aura") {
            if (emptyRefusal) {
                refusal->content.clear(); // a runner that only says no
            } else if (hugeRefusal) {
                const std::string message(kMaxToolResultBytes, 'm');
                refusal->content = "{\"error\":\"not_found\",\"message\":\"" + message + "\"}";
            } else {
                *refusal = MakeToolError(use.id, "not_found", "No module " + module + ".");
            }
            return false;
        }
        if (use.name == "set_setting") {
            card->title = "Change a setting of Kill Aura";
            card->lines.push_back({"Kill Aura > Range", "3.0", "4.5"});
            card->warning = "Servers may detect this module.";
        } else if (use.name == "set_module_enabled") {
            card->title = "Turn on Kill Aura";
            card->lines.push_back({"Kill Aura", "Off", "On"});
        } else if (use.name == "write_script") {
            card->title = "Write script auto_gg";
            card->isScriptWrite = true;
            card->scriptName = "auto_gg";
            card->currentSource = "local a = 1\nprint(a)\n";
            card->newSource = use.input["source"].AsString();
        } else {
            card->title = "Other change";
        }
        return true;
    }

    ToolResult Run(const ToolUse& use) override {
        ran.push_back(use.name);
        ToolResult result;
        if (use.name == "get_module" && big) {
            result.content = "{\"x\":\"" + std::string(kMaxToolResultBytes, 'a') + "\"}";
        } else if (use.name == "read_script" && quoted) {
            // about 10 KB of JSON text, but each \" doubles once it is a JSON string in the POST body
            result.content = "{\"source\":\"";
            for (int i = 0; i < 5000; ++i) {
                result.content += "\\\"";
            }
            result.content += "\"}";
        } else if (use.name == "set_module_enabled" && failRun) {
            return MakeToolError(use.id, "failed", "could not");
        } else {
            result.content = "{\"ok\":true}";
        }
        return result;
    }

    int describes = 0;
    bool big = false;
    bool quoted = false;
    bool failRun = false;
    bool emptyRefusal = false;
    bool hugeRefusal = false;
    std::vector<std::string> ran;
};

ToolUse Use(const std::string& id, const std::string& name, const std::string& inputJson) {
    ToolUse use;
    use.id = id;
    use.name = name;
    nickel::json::Parse(inputJson, &use.input);
    return use;
}

std::string ErrorCode(const ToolResult& result) {
    nickel::json::Value body;
    nickel::json::Parse(result.content, &body);
    return body["error"].AsString();
}

const int64_t kNoon = 1794830400; // 2026-11-16T12:00:00Z

void TextHelpers() {
    Check(CountChars("abc") == 3 && CountChars("\xc3\xa9t\xc3\xa9") == 3 && CountChars("\xf0\x9f\x98\x80") == 1,
          "characters are counted as code points");
    Check(KeepChars("\xc3\xa9t\xc3\xa9", 2) == "\xc3\xa9t" && KeepChars("ab", 5) == "ab",
          "cutting keeps whole characters");
    Check(Trim("  hi \n") == "hi" && Trim(" \t\r\n").empty(), "trim removes spaces and newlines");
    Check(ParseUtc("2026-11-17T00:00:00Z") == 1794873600 && ParseUtc("tomorrow") == 0, "reset_at is read as UTC");
    Check(ResetText("2026-11-17T00:00:00Z") == "Resets at 00:00 UTC." && ResetText("").empty(),
          "the reset time is shown in UTC");
    Check(ErrorText({"offline", "", 0, 0}).find("offline") != std::string::npos
              && ErrorText({"quota_exceeded", "", 429, 0}).find("messages") != std::string::npos
              && ErrorText({"blocked", "", 403, 0}).find("not available") != std::string::npos
              && ErrorText({"weird", "model says hi", 500, 0}).find("model says hi") == std::string::npos,
          "error texts are fixed per code, never the server's message");
    Check(StopText(StopReason::Refusal) == "Nickel AI can't help with that." && StopText(StopReason::EndTurn).empty(),
          "a refusal ends with a fixed line");
    Check(IsActionTool("set_setting") && IsActionTool("write_script") && IsActionTool("set_script_running")
              && IsActionTool("revert_changes") && IsActionTool("set_module_enabled") && !IsActionTool("get_module")
              && !IsActionTool("navigate_ui") && IsKnownTool("navigate_ui") && !IsKnownTool("delete_everything"),
          "the five action tools of the contract need a card; the seven read tools do not");
}

void Diff() {
    const auto same = LineDiff("a\nb\n", "a\nb\n");
    Check(same.size() == 2 && same[0].kind == DiffLine::Kind::Same,
          "an unchanged script has no added or removed lines");
    const auto created = LineDiff("", "x\ny");
    Check(created.size() == 2 && created[0].kind == DiffLine::Kind::Added && created[1].text == "y",
          "a new script shows every line added");
    const auto edit = LineDiff("a\nb\nc\nd", "a\nB\nc\nd\ne");
    int added = 0, removed = 0;
    for (const auto& line : edit) {
        added += line.kind == DiffLine::Kind::Added;
        removed += line.kind == DiffLine::Kind::Removed;
    }
    Check(added == 2 && removed == 1 && edit.front().text == "a" && edit.back().text == "e",
          "a changed line is one removed and one added; context lines stay");
    Check(LineDiff("a\r\nb", "a\nb").size() == 2, "Windows line ends do not count as changes");
}

void Unavailable() {
    ChatPage page(nullptr, nullptr);
    page.Tick(1, kNoon, true);
    Check(!page.Available() && page.CurrentActivity() == Activity::Unavailable,
          "without a client the page is unavailable");
    Check(!page.Send("hello") && !page.SendBlockedReason("hello").empty(), "and nothing can be sent");
    page.Cancel();
    page.NewChat();
    Check(!page.Allow("x") && !page.Decline("x"), "and nothing can be allowed");
}

void Sending() {
    FakeClient client;
    FakeTools tools;
    ChatPage page(&client, &tools);
    page.Tick(1, kNoon, true);
    Check(page.QuotaLine() == "25 messages a day" && page.CurrentActivity() == Activity::Idle,
          "before the first answer: the default limit, idle");
    Check(!page.Send("   ") && client.started.empty(), "an empty message is not sent");
    Check(!page.Send(std::string(kMaxMessageChars + 1, 'x')) && client.started.empty(), "4001 characters are refused");
    std::string accents;
    for (size_t i = 0; i < kMaxMessageChars; ++i) {
        accents += "\xc3\xa9";
    }
    Check(page.CanSend(accents), "4000 accented characters (8000 bytes) are allowed");
    Check(page.Send("  make ESP red  ") && client.started.size() == 1 && client.started[0] == "make ESP red",
          "a message is sent trimmed");
    Check(page.Busy() && page.CurrentActivity() == Activity::Thinking && page.ActivityText() == "Thinking",
          "after sending: thinking");
    Check(!page.Send("again") && client.started.size() == 1, "no second message while a turn runs");
    client.Turn(3, 25, "2026-11-17T00:00:00Z");
    page.Tick(2, kNoon, true);
    Check(page.QuotaLine() == "22 left today", "the quota line comes from the turn event");
    Check(page.CurrentActivity() == Activity::Thinking, "no text yet: still thinking");
    client.Text("Sure");
    page.Tick(3, kNoon, true);
    Check(page.CurrentActivity() == Activity::Writing && page.Conversation().messages.back().text == "Sure",
          "streamed text is shown as it arrives");
    client.Done(StopReason::EndTurn);
    page.Tick(4, kNoon, true);
    Check(!page.Busy() && page.CanSend("next") && page.ErrorLine().empty(),
          "the turn ended: the player can write again");
    client.Done(StopReason::Refusal);
    page.Tick(5, kNoon, true);
    Check(page.ErrorLine() == "Nickel AI can't help with that.", "a refusal shows the fixed line");
}

void QuotaAndErrors() {
    FakeClient client;
    FakeTools tools;
    ChatPage page(&client, &tools);
    page.Tick(1, kNoon, true);
    page.Send("hi");
    client.Turn(25, 25, "2026-11-17T00:00:00Z");
    client.Text("ok");
    client.Done(StopReason::EndTurn);
    page.Tick(2, kNoon, true);
    Check(page.QuotaLine() == "0 left today" && page.QuotaUsedUp() && !page.CanSend("more"),
          "25 of 25 used: input blocked");
    Check(page.SendBlockedReason("more").find("Resets at 00:00 UTC.") != std::string::npos,
          "the block says when the quota resets");
    page.Tick(3, 1794873601, true);
    Check(!page.QuotaUsedUp() && page.CanSend("more"), "after the reset time the service decides again");

    FakeClient c2;
    ChatPage p2(&c2, &tools);
    p2.Tick(1, kNoon, true);
    p2.Send("hi");
    c2.Fail("rate_limited", 429, 30);
    p2.Tick(10, kNoon, true);
    Check(p2.CurrentActivity() == Activity::Error && p2.ErrorLine().find("Too many") != std::string::npos,
          "a 429 shows the rate-limit text");
    Check(!p2.CanSend("again") && p2.SendBlockedReason("again") == "Try again in 30 seconds.",
          "Retry-After blocks sending");
    p2.Tick(40.5, kNoon, true);
    Check(p2.CanSend("again"), "and only for its duration");
    Check(p2.Send("again") && c2.started.size() == 2, "a new turn after an error");
    c2.Fail("offline", 0, 0);
    p2.Tick(41, kNoon, true);
    Check(p2.ErrorLine().find("offline") != std::string::npos && p2.CanSend("x"), "offline: shown, retry allowed");

    // A turn the service gives back: the turn event said 1 used, the failure says 0 (error event or GET /status).
    FakeClient c3;
    ChatPage p3(&c3, &tools);
    p3.Tick(1, kNoon, true);
    p3.Send("hi");
    c3.Turn(1, 25, "2026-11-17T00:00:00Z");
    p3.Tick(2, kNoon, true);
    Check(p3.QuotaLine() == "24 left today", "the turn event counts the message");
    c3.Fail("model_unavailable", 200, 0);
    c3.snap.quota.used = 0;
    p3.Tick(3, kNoon, true);
    Check(p3.QuotaLine() == "25 left today" && p3.ErrorLine().find("busy") != std::string::npos,
          "after a refunded failure the quota line goes back to the count before sending");
    c3.snap.quota.used = 1; // GET /status answering after the error: a later snapshot of the same failure
    ++c3.snap.revision;
    p3.Tick(4, kNoon, true);
    Check(p3.QuotaLine() == "24 left today" && p3.CurrentActivity() == Activity::Error,
          "a quota that arrives after the error is shown too");
}

void ReadToolsRunAtOnce() {
    FakeClient client;
    FakeTools tools;
    ChatPage page(&client, &tools);
    page.Tick(1, kNoon, true);
    page.Send("find esp");
    client.Turn(1, 25, "");
    client.Tools({Use("u1", "search_modules", "{\"query\":\"esp\"}"), Use("u2", "get_client_status", "{}"),
                  Use("u3", "launch_rockets", "{}")});
    page.Tick(2, kNoon, false);
    Check(tools.ran.empty() && client.posts.empty() && page.CurrentActivity() == Activity::UsingTools,
          "nothing runs while the panel is hidden");
    page.Tick(3, kNoon, true);
    Check(tools.ran.size() == 2 && tools.describes == 0, "read tools run without a card when the panel is visible");
    Check(client.posts.size() == 1 && client.posts[0].size() == 3 && client.posts[0][0].toolUseId == "u1"
              && client.posts[0][1].toolUseId == "u2" && client.posts[0][2].toolUseId == "u3",
          "all results go back in one post, in order");
    Check(client.posts[0][2].isError && ErrorCode(client.posts[0][2]) == "unknown_tool", "an unknown tool is an error");
    page.Tick(4, kNoon, true);
    Check(client.posts.size() == 1 && tools.ran.size() == 2, "a step's tools run once");

    tools.big = true;
    client.Tools({Use("u4", "get_module", "{\"id\":\"esp\"}")});
    page.Tick(5, kNoon, true);
    Check(client.posts.size() == 2 && client.posts[1][0].isError && ErrorCode(client.posts[1][0]) == "result_too_large",
          "a result over 16 KB becomes an error");
}

void Confirmations() {
    FakeClient client;
    FakeTools tools;
    ChatPage page(&client, &tools);
    page.Tick(1, kNoon, true);
    page.Send("range 4.5 and turn on kill aura");
    client.Turn(2, 25, "");
    client.Tools({Use("a1", "set_setting", "{\"module_id\":\"kill_aura\",\"setting\":\"range\",\"value\":4.5}"),
                  Use("a2", "set_module_enabled", "{\"module_id\":\"kill_aura\",\"enabled\":true}"),
                  Use("a3", "get_module", "{\"id\":\"kill_aura\"}")});
    page.Tick(2, kNoon, true);
    const auto cards = page.OpenCards();
    Check(cards.size() == 2 && tools.ran.size() == 1 && tools.ran[0] == "get_module",
          "action tools wait on cards; the read tool already ran");
    Check(cards[0]->card.title == "Change a setting of Kill Aura" && cards[0]->card.lines[0].current == "3.0"
              && cards[0]->card.lines[0].next == "4.5" && cards[0]->card.toolUseId == "a1",
          "a card shows the real current and new values from the tools");
    Check(page.CurrentActivity() == Activity::WaitingForPlayer && page.ActivityText() == "Waiting for your answers",
          "the activity says the player must answer");
    Check(client.posts.empty(), "no result is posted while a card is open");
    Check(!page.Allow("a3") && !page.Allow("nope"), "only an open card can be allowed");
    page.Tick(2 + kCardArmSeconds, kNoon, true);
    Check(page.Decline("a1") && client.posts.empty(), "declining one card still waits for the other");
    Check(page.Allow("a2"), "allow the other");
    Check(tools.ran.size() == 2 && tools.ran[1] == "set_module_enabled", "only the allowed action ran");
    Check(client.posts.size() == 1 && client.posts[0].size() == 3, "then every result goes back at once");
    Check(client.posts[0][0].isError && ErrorCode(client.posts[0][0]) == "declined", "the declined call says declined");
    Check(!client.posts[0][1].isError && !client.posts[0][2].isError, "the allowed and read calls succeed");
    Check(page.Notes().size() == 2 && page.Notes()[0].kind == ChatNote::Kind::Declined
              && page.Notes()[1].kind == ChatNote::Kind::Applied
              && page.Notes()[1].text == "Applied: Turn on Kill Aura",
          "the chat records what was declined and applied");
    Check(!page.Allow("a2") && tools.ran.size() == 2, "a card cannot be answered twice");

    // a refused description: no card, the tools' error goes back
    client.Tools({Use("b1", "set_module_enabled", "{\"module_id\":\"ghost\",\"enabled\":true}")});
    page.Tick(3, kNoon, true);
    Check(page.OpenCards().empty() && client.posts.size() == 2 && ErrorCode(client.posts[1][0]) == "not_found"
              && tools.ran.size() == 2,
          "an action the tools cannot describe is refused without a card");

    // failing run
    tools.failRun = true;
    client.Tools({Use("c1", "set_module_enabled", "{\"module_id\":\"kill_aura\",\"enabled\":true}")});
    page.Tick(4, kNoon, true);
    page.Tick(5, kNoon, true);
    page.Allow("c1");
    Check(client.posts.size() == 3 && client.posts[2][0].isError && page.Notes().back().kind == ChatNote::Kind::Failed,
          "a failed action is reported to the model and in the chat");
}

void Timeout() {
    FakeClient client;
    FakeTools tools;
    ChatPage page(&client, &tools);
    page.Tick(0, kNoon, true);
    page.Send("x");
    client.Tools({Use("t1", "set_module_enabled", "{\"module_id\":\"kill_aura\",\"enabled\":true}")});
    page.Tick(10, kNoon, true);
    Check(page.OpenCards().size() == 1 && page.SecondsLeft(*page.OpenCards()[0]) == kConfirmTimeoutSeconds,
          "a card starts its two minutes when shown");
    page.Tick(10 + kConfirmTimeoutSeconds - 1, kNoon, true);
    Check(client.posts.empty(), "still open just before the timeout");
    page.Tick(10 + kConfirmTimeoutSeconds, kNoon, true);
    Check(client.posts.size() == 1 && ErrorCode(client.posts[0][0]) == "declined" && tools.ran.empty(),
          "an unanswered card is declined after two minutes and nothing runs");
}

void ScriptCard() {
    FakeClient client;
    FakeTools tools;
    ChatPage page(&client, &tools);
    page.Tick(0, kNoon, true);
    page.Send("write a script");
    client.Tools({Use("w1", "write_script", "{\"name\":\"auto_gg\",\"source\":\"local a = 2\\nprint(a)\\n\"}")});
    page.Tick(1, kNoon, true);
    const auto cards = page.OpenCards();
    Check(cards.size() == 1 && cards[0]->card.isScriptWrite && cards[0]->card.tool == "write_script",
          "a script write shows a script card");
    const auto diff = LineDiff(cards[0]->card.currentSource, cards[0]->card.newSource);
    Check(diff.size() == 3 && diff[0].kind == DiffLine::Kind::Removed && diff[1].kind == DiffLine::Kind::Added,
          "its diff compares the file with the new source");
    Check(cards[0]->diff.size() == diff.size() && cards[0]->diff[0].kind == DiffLine::Kind::Removed
              && cards[0]->diff[1].text == "local a = 2",
          "the diff is computed once when the card opens, not by the drawing");
    page.Tick(2, kNoon, true);
    page.Allow("w1");
    Check(page.Notes().back().text == "Saved (held): Write script auto_gg", "a saved script is noted as held");
}

void CancelAndNewChat() {
    FakeClient client;
    FakeTools tools;
    ChatPage page(&client, &tools);
    page.Tick(0, kNoon, true);
    page.Send("x");
    client.Tools({Use("k1", "set_module_enabled", "{\"module_id\":\"kill_aura\",\"enabled\":true}")});
    page.Tick(1, kNoon, true);
    page.Cancel();
    Check(client.cancels == 1 && page.OpenCards().empty() && !page.Busy(), "Stop cancels the turn and its cards");
    page.Tick(2, kNoon, true);
    Check(client.posts.empty() && tools.ran.empty(), "and nothing of the cancelled step runs or is posted");
    Check(page.Send("y"), "a new message after a stop");
    client.Turn(2, 25, "");
    client.Text("hello");
    client.Done(StopReason::EndTurn);
    page.Tick(3, kNoon, true);
    page.NewChat();
    Check(page.Conversation().messages.empty() && page.Notes().empty() && page.Conversation().conversationId.empty(),
          "New chat forgets the conversation and its notes");

    // a step that ends in tools, then new text: "Writing" only once text arrives again
    page.Send("z");
    client.Turn(3, 25, "");
    client.Text("Let me look.");
    client.Tools({Use("r1", "list_scripts", "{}")});
    page.Tick(4, kNoon, true);
    Check(client.posts.size() == 1, "the read tool of the step is answered");
    client.snap.state = TurnState::Streaming;
    ++client.snap.revision;
    page.Tick(5, kNoon, true);
    Check(page.CurrentActivity() == Activity::Thinking, "after a tool round, thinking until new text");
    client.Text(" Found 2 scripts.");
    page.Tick(6, kNoon, true);
    Check(page.CurrentActivity() == Activity::Writing, "then writing");
}

// The second click of a double-click lands on the card that moved under the cursor: it must not answer it.
void CardArming() {
    FakeClient client;
    FakeTools tools;
    ChatPage page(&client, &tools);
    page.Tick(0, kNoon, true);
    page.Send("x");
    client.Tools({Use("d1", "set_setting", "{\"module_id\":\"kill_aura\",\"setting\":\"range\",\"value\":4.5}"),
                  Use("d2", "set_module_enabled", "{\"module_id\":\"kill_aura\",\"enabled\":true}")});
    page.Tick(1, kNoon, true);
    page.PlaceCard("d1", 0, 100);
    page.PlaceCard("d2", 0, 300);
    Check(!page.Armed(*page.OpenCards()[0]) && !page.Allow("d1") && !page.Decline("d1") && tools.ran.empty(),
          "a card that just appeared ignores clicks");
    page.Tick(1 + kCardArmSeconds - 0.05, kNoon, true);
    Check(!page.Allow("d1") && tools.ran.empty(), "still arming just before the delay");
    page.Tick(1 + kCardArmSeconds, kNoon, true);
    page.PlaceCard("d1", 0, 100);
    page.PlaceCard("d2", 0, 300);
    Check(page.Armed(*page.OpenCards()[0]), "a card that stayed in place arms after the delay");
    Check(page.Allow("d1") && tools.ran.size() == 1, "then Allow works");
    // next frame: card 2 moved up to card 1's place; the double-click's second click arrives 0.2 s later
    page.Tick(1.6, kNoon, true);
    page.PlaceCard("d2", 0, 100);
    page.Tick(1.8, kNoon, true);
    page.PlaceCard("d2", 0, 100);
    Check(!page.Allow("d2") && !page.Decline("d2") && tools.ran.size() == 1 && client.posts.empty(),
          "the card that moved under the cursor ignores the second click");
    page.Tick(1.6 + kCardArmSeconds, kNoon, true);
    page.PlaceCard("d2", 0, 100);
    Check(page.Allow("d2") && tools.ran.size() == 2 && client.posts.size() == 1,
          "it answers once it has stayed in place for the delay");
}

// The service measures a result as a JSON string: quotes and escapes count twice or more.
void ResultSizeAsTheServiceCounts() {
    Check(ResultWireSize("a\"b") == 6 && ResultWireSize("{}") == 4 && ResultWireSize("x\ny") == 6,
          "the wire size counts the quotes and escapes");
    Check(ResultWireSize(std::string(kMaxToolResultBytes - 2, 'a')) == kMaxToolResultBytes,
          "16 KB of plain text including its two quotes is the limit");
    FakeClient client;
    FakeTools tools;
    tools.quoted = true;
    ChatPage page(&client, &tools);
    page.Tick(0, kNoon, true);
    page.Send("read it");
    client.Tools({Use("q1", "read_script", "{\"name\":\"auto_gg\"}"), Use("q2", "list_scripts", "{}")});
    page.Tick(1, kNoon, true);
    Check(client.posts.size() == 1 && client.posts[0][0].isError
              && ErrorCode(client.posts[0][0]) == "result_too_large" && !client.posts[0][1].isError,
          "10 KB of raw text that escapes past 16 KB becomes result_too_large, the other result still goes");
    for (const auto& result : client.posts[0]) {
        Check(ResultWireSize(result.content) <= kMaxToolResultBytes, "every posted result fits the service's limit");
    }
}

void RefusedPost() {
    FakeClient client;
    FakeTools tools;
    ChatPage page(&client, &tools);
    page.Tick(0, kNoon, true);
    page.Send("x");
    client.refusePosts = true;
    client.Tools({Use("p1", "list_scripts", "{}")});
    page.Tick(1, kNoon, true);
    Check(client.attempts == 1 && client.posts.empty() && page.CurrentActivity() == Activity::Error
              && !page.ErrorLine().empty(),
          "a post the client refused is shown as an error, not as tools still running");
    page.Tick(1.5, kNoon, true);
    Check(client.attempts == 1 && tools.ran.size() == 1,
          "it is not retried every frame, and the tool does not run again");
    client.refusePosts = false;
    page.Tick(2, kNoon, true);
    Check(client.attempts == 2 && client.posts.size() == 1 && client.posts[0][0].toolUseId == "p1"
              && page.ErrorLine().empty(),
          "it is retried after a second with the same results");
    page.Tick(3, kNoon, true);
    Check(client.attempts == 2, "and sent only once");
}

void RefusalIsChecked() {
    FakeClient client;
    FakeTools tools;
    tools.emptyRefusal = true;
    ChatPage page(&client, &tools);
    page.Tick(0, kNoon, true);
    page.Send("x");
    client.Tools({Use("e1", "set_module_enabled", "{\"module_id\":\"ghost\",\"enabled\":true}"),
                  Use("e2", "list_scripts", "{}")});
    page.Tick(1, kNoon, true);
    Check(client.posts.size() == 1 && client.posts[0][0].isError && ErrorCode(client.posts[0][0]) == "refused"
              && client.posts[0][0].toolUseId == "e1",
          "an empty refusal becomes a JSON \"refused\" error");
    tools.emptyRefusal = false;
    tools.hugeRefusal = true;
    client.Tools({Use("h1", "set_setting", "{\"module_id\":\"ghost\",\"setting\":\"x\",\"value\":1}"),
                  Use("h2", "list_scripts", "{}")});
    page.Tick(2, kNoon, true);
    Check(client.posts.size() == 2 && client.posts[1][0].isError && ErrorCode(client.posts[1][0]) == "result_too_large"
              && !client.posts[1][1].isError,
          "an oversized refusal is capped like any result and does not spoil the step");
}

// ---- the chat window

using Kind = ChatItem::Kind;

std::string Kinds(const std::vector<ChatItem>& items) {
    std::string out;
    for (const auto& item : items) {
        switch (item.kind) {
            case Kind::Greeting: out += "G"; break;
            case Kind::Message: out += item.side == ChatSide::Player ? "P" : "A"; break;
            case Kind::Typing: out += "T"; break;
            case Kind::Card: out += "C"; break;
            case Kind::Note: out += "N"; break;
            case Kind::System: out += "S"; break;
        }
    }
    return out;
}

// "1" where an avatar starts a run, "0" elsewhere.
std::string Avatars(const std::vector<ChatItem>& items) {
    std::string out;
    for (const auto& item : items) {
        out += item.groupStart ? "1" : "0";
    }
    return out;
}

void Greeting() {
    FakeClient client;
    FakeTools tools;
    ChatPage page(&client, &tools);
    page.Tick(1, kNoon, true);
    auto items = page.Timeline();
    Check(Kinds(items) == "G" && items[0].side == ChatSide::Assistant && items[0].groupStart
              && items[0].text == kGreeting,
          "a new chat opens with the assistant's greeting, on the left with its avatar");
    Check(std::string(kGreeting)
                  == "Hi! I'm Nickel AI. Ask me anything about Nickel: modules, settings or scripts. I can also turn "
                     "modules on or write a script for you, always after you confirm.",
          "the greeting says what Nickel AI does and that it asks first");
    Check(client.started.empty() && client.snap.messages.empty() && client.snap.revision == 0,
          "the greeting is local: nothing is sent and the conversation stays empty");
    Check(page.QuotaLine() == "25 messages a day" && page.CanSend("hi"), "the greeting is not counted in the quota");
    Check(page.Send("hi") && client.started.size() == 1 && client.started[0] == "hi",
          "the first message sent is the player's own, without the greeting");
    client.Turn(1, 25, "2026-11-17T00:00:00Z");
    client.Text("Hello!");
    client.Done(StopReason::EndTurn);
    page.Tick(2, kNoon, true);
    Check(page.QuotaLine() == "24 left today", "one message sent, one counted: 24 left");
    Check(Kinds(page.Timeline()) == "GPA", "the greeting stays first, before the conversation");
    page.NewChat();
    Check(Kinds(page.Timeline()) == "G" && client.started.size() == 1, "New chat shows the greeting again, sends nothing");

    ChatPage none(nullptr, nullptr);
    none.Tick(1, kNoon, true);
    items = none.Timeline();
    Check(Kinds(items) == "S" && items[0].side == ChatSide::Centre && !items[0].groupStart,
          "without a client: no greeting, one centred line");
}

void TimelineSidesAndOrder() {
    FakeClient client;
    FakeTools tools;
    ChatPage page(&client, &tools);
    page.Tick(1, kNoon, true);
    page.Send("turn on kill aura");
    page.Tick(2, kNoon, true);
    auto items = page.Timeline();
    Check(Kinds(items) == "GPT" && items[1].side == ChatSide::Player && items[1].message == 0,
          "the player's message is on the right, after the greeting");
    Check(Avatars(items) == "111", "each side change starts with an avatar");
    client.Turn(1, 25, "");
    client.Text("Sure, ");
    page.Tick(3, kNoon, true);
    items = page.Timeline();
    Check(Kinds(items) == "GPA" && items[2].side == ChatSide::Assistant && items[2].streaming,
          "the streamed answer is on the left, in its bubble, marked as still arriving");
    client.Text("let me check.");
    client.Tools({Use("s1", "set_module_enabled", "{\"module_id\":\"kill_aura\",\"enabled\":true}")});
    page.Tick(4, kNoon, true);
    items = page.Timeline();
    Check(Kinds(items) == "GPAC" && items[3].side == ChatSide::Assistant && items[3].call != nullptr
              && items[3].call->card.title == "Turn on Kill Aura" && !items[2].streaming,
          "a confirmation card is an assistant-side item after the answer, from the tools' card");
    Check(Avatars(items) == "1110", "the card continues the assistant's run: no second avatar");
    page.Tick(4 + kCardArmSeconds, kNoon, true);
    page.Allow("s1");
    client.Text("Done.");
    client.Done(StopReason::EndTurn);
    page.Tick(5, kNoon, true);
    page.Send("thanks");
    client.Turn(2, 25, "");
    client.Text("You're welcome.");
    client.Done(StopReason::EndTurn);
    page.Tick(6, kNoon, true);
    items = page.Timeline();
    Check(Kinds(items) == "GPANPA", "messages keep their order; the applied change is a centred note");
    std::vector<size_t> order;
    for (const auto& item : items) {
        if (item.kind == Kind::Message) {
            order.push_back(item.message);
        }
    }
    Check(order.size() == 4 && order[0] == 0 && order[1] == 1 && order[2] == 2 && order[3] == 3,
          "message items follow the conversation's order");
    Check(items[3].side == ChatSide::Centre && Avatars(items) == "111011",
          "the note is centred and ends the run; the next message starts with an avatar");
    Check(page.Conversation().messages[items[2].message].text == "Sure, let me check.Done.",
          "the assistant's bubble holds its streamed text");
}

void TypingIndicator() {
    FakeClient client;
    FakeTools tools;
    ChatPage page(&client, &tools);
    page.Tick(1, kNoon, true);
    page.Send("find esp");
    page.Tick(2, kNoon, true);
    auto items = page.Timeline();
    Check(items.back().kind == Kind::Typing && items.back().side == ChatSide::Assistant && items.back().text.empty(),
          "while waiting for the first words: a typing indicator in an assistant bubble");
    client.Turn(1, 25, "");
    page.Tick(3, kNoon, true);
    Check(page.Timeline().back().kind == Kind::Typing, "still typing until text arrives");
    client.Text("Looking");
    page.Tick(4, kNoon, true);
    items = page.Timeline();
    Check(Kinds(items) == "GPA", "the indicator goes away once the answer streams");
    client.Tools({Use("r1", "search_modules", "{\"query\":\"esp\"}")});
    page.Tick(5, kNoon, false);
    items = page.Timeline();
    Check(Kinds(items) == "GPAT" && items.back().text == "Searching modules" && !items.back().groupStart,
          "using tools: the indicator names the tool and continues the assistant's run");
    page.Tick(6, kNoon, true);
    client.snap.state = TurnState::Streaming;
    ++client.snap.revision;
    page.Tick(7, kNoon, true);
    Check(page.Timeline().back().kind == Kind::Typing, "after a tool round: typing again until new text");
    client.Text(" found it.");
    page.Tick(8, kNoon, true);
    Check(page.Timeline().back().kind == Kind::Message, "new text replaces the indicator");
    client.Tools({Use("w1", "set_module_enabled", "{\"module_id\":\"kill_aura\",\"enabled\":true}")});
    page.Tick(9, kNoon, true);
    Check(Kinds(page.Timeline()) == "GPAC", "a card waiting for the player: no typing indicator");
}

void ErrorsAsSystemLines() {
    FakeClient client;
    FakeTools tools;
    ChatPage page(&client, &tools);
    page.Tick(1, kNoon, true);
    page.Send("hi");
    client.Fail("offline", 0, 0);
    page.Tick(2, kNoon, true);
    auto items = page.Timeline();
    Check(Kinds(items) == "GPS" && items.back().side == ChatSide::Centre && items.back().failure
              && !items.back().groupStart && items.back().text.find("offline") != std::string::npos,
          "offline is a centred system line, not a bubble");
    Check(page.Send("again"), "the player can write again after an error");
    client.Fail("model_unavailable", 503, 0);
    page.Tick(3, kNoon, true);
    Check(page.Timeline().back().kind == Kind::System && page.Timeline().back().text.find("busy") != std::string::npos,
          "busy is a system line");
    page.Send("more");
    client.Fail("quota_exceeded", 429, 0);
    page.Tick(4, kNoon, true);
    items = page.Timeline();
    Check(items.back().kind == Kind::System && items.back().text.find("messages") != std::string::npos,
          "the used-up quota is a system line");
    int systems = 0;
    for (const auto& item : items) {
        systems += item.kind == Kind::System;
        Check(item.kind == Kind::System || item.text.find("offline") == std::string::npos,
              "no bubble carries an error text");
    }
    Check(systems == 1, "only the current error is shown");
    page.Tick(70, kNoon, true);
    page.Send("x");
    client.Turn(3, 25, "");
    client.Text("No.");
    client.Done(StopReason::Refusal);
    page.Tick(71, kNoon, true);
    items = page.Timeline();
    Check(items.back().kind == Kind::System && !items.back().failure && items.back().text == StopText(StopReason::Refusal),
          "a refusal ends with a grey system line under the answer");
}

void BubbleToggles() {
    Check(!BubbleState::BubbleShown(false, true) && !BubbleState::BubbleShown(true, false)
              && BubbleState::BubbleShown(true, true),
          "the bubble shows only while Nickel AI is on and the menu is open");
    BubbleState bubble;
    Check(!bubble.WindowOpen() && !bubble.EverOpened() && !bubble.TakeOpening(),
          "the window starts closed: nothing created before the first click");
    bubble.Click();
    Check(bubble.WindowOpen() && bubble.EverOpened(), "a click on the bubble opens the window");
    Check(bubble.TakeOpening() && !bubble.TakeOpening(), "opening focuses the input and scrolls once");
    bubble.Click();
    Check(!bubble.WindowOpen(), "a second click closes it");
    bubble.Click();
    bubble.Close();
    Check(!bubble.WindowOpen(), "the x closes it");
    bubble.Open();
    bubble.TakeOpening();
    bubble.Open();
    Check(bubble.WindowOpen() && !bubble.TakeOpening(), "navigate_ui opens it, and an open window stays open");
    bubble.Reset();
    Check(!bubble.WindowOpen() && !bubble.EverOpened(), "turning Nickel AI off closes and forgets it");
}

bool Near(float a, float b) {
    return a > b - 0.01f && a < b + 0.01f;
}

void BubblePlacement() {
    // 1920 x 1080, default menu centred
    UiRect menu{454, 251, 1012, 578};
    BubbleLayout place = PlaceBubble(menu, 1, 1920, 1080);
    Check(!place.bubbleInSidebar && Near(place.bubble.x, 394) && Near(place.bubble.w, 48)
              && Near(place.bubble.Right(), menu.x - 12) && Near(place.bubble.Bottom(), menu.Bottom() - 20),
          "the bubble is just outside the menu's left edge, near its bottom");
    Check(Near(place.window.w, 380) && Near(place.window.h, 520) && Near(place.window.Right(), place.bubble.Right())
              && Near(place.window.Bottom(), place.bubble.y - 12),
          "the window opens on the left, above the bubble, 380 x 520");
    Check(!place.windowOverMenu && !place.window.Overlaps(menu) && !place.window.Overlaps(place.bubble),
          "with room it covers neither the menu nor the bubble");

    // 1280 x 720: room for the bubble, not for the window
    menu = {134, 71, 1012, 578};
    place = PlaceBubble(menu, 1, 1280, 720);
    Check(!place.bubbleInSidebar && Near(place.window.x, 8) && place.windowOverMenu
              && !place.window.Overlaps(place.bubble),
          "without room the window overlaps the menu's left part, the bubble stays clear");

    // the menu dragged against the left edge: the bubble goes into the sidebar's bottom-left corner
    menu = {20, 100, 1012, 578};
    place = PlaceBubble(menu, 1, 1920, 1080);
    Check(place.bubbleInSidebar && Near(place.bubble.x, menu.x + 16) && Near(place.bubble.Bottom(), menu.Bottom() - 16)
              && place.bubble.x >= menu.x && place.bubble.Right() <= menu.x + 244,
          "no room on the left: the bubble sits inside the sidebar's bottom-left corner");
    Check(Near(place.window.x, place.bubble.x) && place.windowOverMenu && !place.window.Overlaps(place.bubble)
              && place.window.y >= 8,
          "its window opens above it, over the menu's left part");

    // a smaller scale shrinks both
    menu = {600, 300, 1012 * .8f, 578 * .8f};
    place = PlaceBubble(menu, .8f, 1920, 1080);
    Check(Near(place.bubble.w, 48 * .8f) && Near(place.window.w, 380 * .8f) && place.window.h <= 520 * .8f + .01f,
          "the bubble and the window follow the menu's scale");

    // a short screen: the window shrinks to the room above the bubble, and stays on screen
    menu = {500, 8, 1012, 430};
    place = PlaceBubble(menu, 1, 1600, 446);
    Check(place.window.y >= 8 && place.window.Bottom() <= place.bubble.y - 12 + .01f && place.window.h >= 300,
          "on a short screen the window fits above the bubble");
}

bool OnScreen(const UiRect& r, float w, float h) {
    return r.x >= kScreenMargin - .01f && r.y >= kScreenMargin - .01f && r.Right() <= w - kScreenMargin + .01f
           && r.Bottom() <= h - kScreenMargin + .01f;
}

void BubbleDragAndPlace() {
    // a press is a click until it travels more than kDragThreshold (4 px), then a drag for good
    DragGesture press;
    press.Press(100, 100);
    Check(!press.Move(102, 103) && press.Pressed() && !press.Dragging(), "3.6 px of jitter is still a click");
    Check(press.Release() && !press.Pressed(), "released there: a click (the window toggles)");
    press.Press(100, 100);
    Check(press.Move(104, 103) && press.Dragging() && Near(press.DX(), 4) && Near(press.DY(), 3),
          "5 px: a drag, which follows the mouse");
    Check(press.Move(100, 100) && press.Dragging(), "coming back to the start does not make it a click again");
    Check(!press.Release(), "a drag released is not a click");
    press.Press(0, 0);
    press.Cancel();
    Check(!press.Pressed() && !press.Release(), "a cancelled press is neither");

    // the place kept as fractions of the screen: the same spot on another window size, always on screen
    const UiRect menu{454, 251, 1012, 578};
    const BubbleLayout start = PlaceBubble(menu, 1, 1920, 1080);
    BubbleLayout dragged = DragBubble(start, 1200, -500, 1, 1920, 1080);
    Check(dragged.moved && !dragged.bubbleInSidebar && Near(dragged.bubble.x, start.bubble.x + 1200)
              && Near(dragged.bubble.y, start.bubble.y - 500),
          "a dragged bubble follows the mouse");
    float fx = 0, fy = 0;
    BubbleFraction(dragged.bubble, 1920, 1080, &fx, &fy);
    Check(Near(fx, (dragged.bubble.x + 24) / 1920) && Near(fy, (dragged.bubble.y + 24) / 1080),
          "its place is the centre as a fraction of the screen");
    float rx = 0, ry = 0;
    Check(ReadBubblePlace(WriteBubblePlace(fx, fy), &rx, &ry) && Near(rx, fx) && Near(ry, fy),
          "settings.ini keeps the fractions ([ai] bubble=x,y)");
    Check(!ReadBubblePlace("", &rx, &ry) && !ReadBubblePlace("1.5,0.2", &rx, &ry) && !ReadBubblePlace("0.2", &rx, &ry)
              && !ReadBubblePlace("0.2,0.3x", &rx, &ry) && !ReadBubblePlace("nan,0.5", &rx, &ry),
          "a missing or damaged value is the default place");
    const BubbleLayout same = PlaceBubbleAt(fx, fy, 1, 1920, 1080);
    Check(Near(same.bubble.x, dragged.bubble.x) && Near(same.bubble.y, dragged.bubble.y),
          "read back, the bubble is where it was left");
    const BubbleLayout small = PlaceBubbleAt(fx, fy, .7f, 1280, 720);
    Check(Near(small.bubble.x + small.bubble.w / 2, fx * 1280) && Near(small.bubble.y + small.bubble.h / 2, fy * 720)
              && Near(small.bubble.w, 48 * .7f),
          "on a 1280 x 720 window: the same spot, at the menu's scale");

    // clamping: dragged off the screen, or a window made smaller, keeps both on screen
    BubbleLayout far = DragBubble(start, -3000, 3000, 1, 1920, 1080);
    Check(OnScreen(far.bubble, 1920, 1080) && OnScreen(far.window, 1920, 1080) && !far.window.Overlaps(far.bubble),
          "dragged past the bottom-left corner: the bubble stops at the margin, the window stays on screen");
    Check(far.sides.above && !far.sides.leftward, "bottom-left: the window opens above it, to its right");
    BubbleLayout corner = PlaceBubbleAt(0, 0, 1, 1920, 1080);
    Check(OnScreen(corner.bubble, 1920, 1080) && OnScreen(corner.window, 1920, 1080) && !corner.sides.above
              && !corner.sides.leftward && !corner.window.Overlaps(corner.bubble),
          "top-left: the window opens below it, to its right");
    corner = PlaceBubbleAt(1, 0, 1, 1920, 1080);
    Check(!corner.sides.above && corner.sides.leftward && Near(corner.window.Right(), corner.bubble.Right())
              && OnScreen(corner.window, 1920, 1080),
          "top-right: below it, to its left");
    corner = PlaceBubbleAt(1, 1, 1, 1920, 1080);
    Check(corner.sides.above && corner.sides.leftward && Near(corner.window.Bottom(), corner.bubble.y - 12),
          "bottom-right: above it, to its left");
    const float sizes[][2] = {{1920, 1080}, {1600, 900}, {1280, 720}, {1024, 576}, {800, 450}};
    bool kept = true;
    for (const auto& screen : sizes) {
        for (float x : {0.f, .3f, .5f, 1.f}) {
            for (float y : {0.f, .5f, 1.f}) {
                const BubbleLayout p = PlaceBubbleAt(x, y, .7f, screen[0], screen[1]);
                kept &= OnScreen(p.bubble, screen[0], screen[1]) && OnScreen(p.window, screen[0], screen[1]);
            }
        }
    }
    Check(kept, "every saved place, on every window size, keeps the bubble and the window inside the screen");

    // the window keeps its sides while they have room (no flip from one frame to the next)
    BubbleLayout middle = PlaceBubbleAt(.5f, .5f, 1, 1920, 1080, ChatSides{false, false});
    Check(!middle.sides.above && !middle.sides.leftward && Near(middle.window.y, middle.bubble.Bottom() + 12)
              && Near(middle.window.x, middle.bubble.x),
          "in the middle of the screen the window stays on the sides it had");

    // dragging the window by its header moves both, as one, and stops at the margin
    const BubbleLayout together = DragTogether(start, 300, 200, 1920, 1080);
    Check(Near(together.bubble.x - start.bubble.x, 300) && Near(together.window.x - start.window.x, 300)
              && Near(together.window.y - start.window.y, 200) && together.moved,
          "a header drag moves the bubble and the window by the same amount");
    const BubbleLayout stopped = DragTogether(start, -5000, -5000, 1920, 1080);
    Check(OnScreen(stopped.window, 1920, 1080) && OnScreen(stopped.bubble, 1920, 1080)
              && Near(std::min(stopped.window.x, stopped.bubble.x), kScreenMargin)
              && Near(std::min(stopped.window.y, stopped.bubble.y), kScreenMargin)
              && Near(stopped.window.x - stopped.bubble.x, start.window.x - start.bubble.x),
          "dragged past the top-left corner: both stop at the margin, still together");
}

std::string SourceFile(const char* relative) {
    std::ifstream file(std::string(NICKEL_SOURCE_DIR) + "/" + relative, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

void NoSidebarEntry() {
    const std::string menu = SourceFile("native/menu.cpp");
    const std::string ui = SourceFile("native/ai_ui.hpp");
    Check(!menu.empty() && !ui.empty(), "the menu's sources are readable");
    Check(menu.find("static const int order[12]={0,1,2,3,4,5,11,12,6,7,8,9};") != std::string::npos,
          "the sidebar lists FEATURES, PUBLIC and OTHER without Nickel AI");
    const size_t names = menu.find("const char* names[]={");
    Check(names != std::string::npos && menu.find("\"Nickel AI\"", names) > menu.find('\n', names),
          "no tab is named Nickel AI");
    Check(menu.find("kAiTab") == std::string::npos && ui.find("kAiTab") == std::string::npos
              && menu.find("DrawAiIcon") == std::string::npos && ui.find("DrawAiIcon") == std::string::npos,
          "no Nickel AI tab or sidebar icon is left");
    Check(menu.find("DrawAiBubble(aiPlace,s,opacity,logo);") != std::string::npos
              && menu.find("toggle(AiOption());") != std::string::npos,
          "the menu draws the bubble, and Settings keeps the Nickel AI toggle");
}

} // namespace

int main() {
    TextHelpers();
    Diff();
    Unavailable();
    Sending();
    QuotaAndErrors();
    ReadToolsRunAtOnce();
    Confirmations();
    Timeout();
    ScriptCard();
    CancelAndNewChat();
    CardArming();
    ResultSizeAsTheServiceCounts();
    RefusedPost();
    RefusalIsChecked();
    Greeting();
    TimelineSidesAndOrder();
    TypingIndicator();
    ErrorsAsSystemLines();
    BubbleToggles();
    BubblePlacement();
    BubbleDragAndPlace();
    NoSidebarEntry();
    std::printf("%s (%d failed)\n", failed == 0 ? "all ai ui checks passed" : "ai ui checks failed", failed);
    return failed == 0 ? 0 : 1;
}
