// Nickel AI end to end, without the game: the panel's logic (ChatPage of native/ai_ui.hpp) over the real network client
// and the real tools (native/ai_integration.hpp), against the real AI service running on 127.0.0.1 with a scripted fake
// model and a fake install check (ai/scripts/e2e-server.mjs of the website repository). The modules and scripts are
// fakes in memory. Nothing reaches the internet or the Anthropic API.
//
// NICKEL_AI_E2E_SERVER names that e2e-server.mjs; the test starts it with `node` and stops it at the end. Without it the
// test is skipped (exit code 77).
//
// Checked: streamed text, the quota line, a read tool run at once, an action allowed (the module turns on), an action
// declined (nothing changes, the model receives "declined"), write_script saved held (never started), and the daily
// limit (429 quota_exceeded). The model's answers say "ok:" only when the tool results it received are right.
#include <winsock2.h>
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ai_integration.hpp"
#include "ai_ui.hpp"

namespace {

using namespace overlay;
namespace ai = nickel::ai;

int failed = 0;

void Check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok) {
        ++failed;
    }
}

const char* const kScriptSource = "local m = nickel.module{ name = \"Auto GG\" }\n-- says gg after a kill\n";

// ---- fakes of the game side

class FakeRegistry final : public ModuleRegistry {
public:
    std::vector<Module> list;
    int saves = 0;

    int Count() const override { return static_cast<int>(list.size()); }
    Module& At(int index) override { return list.at(static_cast<size_t>(index)); }
    int Find(const std::string& id) const override {
        for (int i = 0; i < Count(); ++i) {
            if (list[static_cast<size_t>(i)].id == id) {
                return i;
            }
        }
        return -1;
    }
    int Add(const std::string& id) override {
        Module fresh{};
        fresh.id = id;
        list.push_back(fresh);
        return Count() - 1;
    }
    void SaveModule(int, const Setting*) override { ++saves; }
    std::string ReadPreference(const std::string&, const std::string&, size_t) const override { return {}; }
    int ReadPreferenceInt(const std::string&, const std::string&, int fallback) const override { return fallback; }
    int KeyOf(const std::string&) const override { return 0; }
    void BindKey(const std::string&, const std::string&, int) override {}

    Module* ById(const std::string& id) {
        const int index = Find(id);
        return index < 0 ? nullptr : &list[static_cast<size_t>(index)];
    }
};

std::string FakeSha(const std::string& text) {
    uint64_t hash = 1469598103934665603ull;
    std::string hex;
    for (int round = 0; round < 4; ++round) {
        for (char c : text) {
            hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
        }
        hash = (hash ^ static_cast<uint64_t>(round)) * 1099511628211ull;
        char part[17]{};
        std::snprintf(part, sizeof(part), "%016llx", static_cast<unsigned long long>(hash));
        hex += part;
    }
    return hex;
}

class FakeLibrary final : public ScriptLibrary {
public:
    std::vector<ScriptInfo> scripts;
    std::map<std::string, std::string> files;
    std::vector<std::string> started;

    ScriptInfo* Info(const std::string& name) {
        const int index = FindScript(scripts, name);
        return index < 0 ? nullptr : &scripts[static_cast<size_t>(index)];
    }
    std::vector<ScriptInfo> List() const override { return scripts; }
    std::wstring Folder() const override { return L"C:\\fake\\scripts"; }
    bool ReadSource(const std::string& name, ScriptSource& source) const override {
        const auto file = files.find(name);
        if (file == files.end()) {
            return false;
        }
        source.text = file->second;
        source.sha256 = FakeSha(file->second);
        return true;
    }
    std::vector<CompileError> Compile(const std::string&) const override { return {}; }
    bool HasBackup(const std::string&) const override { return false; }
    ScriptFileResult WriteSource(const std::string& name, const std::string& source,
                                 const std::string& expectedSha256) override {
        ScriptInfo* info = Info(name);
        if (info == nullptr && !ValidScriptName(name)) {
            return ScriptFileResult::InvalidName;
        }
        if (expectedSha256.empty() ? info != nullptr : (info == nullptr || FakeSha(files[name]) != expectedSha256)) {
            return ScriptFileResult::Conflict;
        }
        if (info == nullptr) {
            ScriptInfo fresh;
            fresh.name = name;
            fresh.state = ScriptState::Stopped;
            scripts.push_back(fresh);
            info = Info(name);
        }
        files[name] = source;
        info->held = true;
        return ScriptFileResult::Done;
    }
    ScriptFileResult Remove(const std::string&, const std::string&) override { return ScriptFileResult::Failed; }
    ScriptFileResult RestoreBackup(const std::string&, const std::string&) override {
        return ScriptFileResult::NotFound;
    }
    bool Hold(const std::string&) override { return false; }
    bool Start(const std::string& name) override {
        started.push_back(name);
        return false;
    }
    bool Stop(const std::string&) override { return false; }
};

class FakeUi final : public ai::tools::UiNavigator {
public:
    bool ShowModule(int, const std::string&) override { return true; }
    bool ShowTab(const std::string&) override { return false; }
};

class FakeStatus final : public ai::tools::ClientStatusSource {
public:
    ai::tools::ClientStatus Status() const override { return {"1.1.0-beta", "1.26.62.1", true, ""}; }
};

// A fixed, well-formed install key (32 bytes in base64url); the fake install check registers it on the first turn.
class TestIdentity final : public ai::IdentitySource {
public:
    bool Authorization(std::string* header) override {
        *header = "Install " + std::string(43, 'Q');
        return true;
    }
};

Module MakeModule(const std::string& id, const std::string& name, const std::string& category, bool enabled) {
    Module module{};
    module.id = id;
    module.name = name;
    module.category = category;
    module.description = name + " description";
    module.enabled = enabled;
    return module;
}

// ---- the service process

struct Server {
    PROCESS_INFORMATION process{};
    HANDLE input = nullptr; // closing it stops the server
    std::string origin;

    bool Start(const std::string& script) {
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE outRead = nullptr, outWrite = nullptr, inRead = nullptr;
        if (!CreatePipe(&outRead, &outWrite, &inherit, 0) || !CreatePipe(&inRead, &input, &inherit, 0)) {
            return false;
        }
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(input, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = inRead;
        startup.hStdOutput = outWrite;
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        std::string command = "node \"" + script + "\"";
        const BOOL started = CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                            nullptr, &startup, &process);
        CloseHandle(outWrite);
        CloseHandle(inRead);
        if (!started) {
            CloseHandle(outRead);
            return false;
        }
        std::string line;
        char c = 0;
        DWORD read = 0;
        while (line.size() < 512 && ReadFile(outRead, &c, 1, &read, nullptr) && read == 1 && c != '\n') {
            line.push_back(c);
        }
        CloseHandle(outRead);
        nickel::json::Value parsed;
        if (!nickel::json::Parse(line, &parsed) || !parsed["origin"].IsString()) {
            std::printf("server said: %s\n", line.c_str());
            return false;
        }
        origin = parsed["origin"].AsString();
        return true;
    }

    ~Server() {
        if (input != nullptr) {
            CloseHandle(input);
        }
        if (process.hProcess != nullptr) {
            if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0) {
                TerminateProcess(process.hProcess, 1);
            }
            CloseHandle(process.hProcess);
            CloseHandle(process.hThread);
        }
    }
};

// ---- driving the panel

double Now() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return duration<double>(steady_clock::now() - start).count();
}

// The service's fixed clock (test/helpers.mjs NOW): the quota resets at the next midnight UTC after it.
const int64_t kServiceNow = ai::ParseUtc("2026-11-15T18:00:00Z");

// Ticks the page (visible, on this thread like the render thread) until `done` or `seconds` pass. `each` sees every
// snapshot.
bool Pump(ai::ChatPage& page, const std::function<bool()>& done, double seconds = 15,
          const std::function<void()>& each = nullptr) {
    const double until = Now() + seconds;
    while (Now() < until) {
        page.Tick(Now(), kServiceNow, true);
        if (each) {
            each();
        }
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

std::string LastAssistant(const ai::ChatPage& page) {
    const auto& messages = page.Conversation().messages;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role == ai::ChatMessage::Role::Assistant) {
            return it->text;
        }
    }
    return "";
}

bool Idle(const ai::ChatPage& page) {
    return page.Conversation().state == ai::TurnState::Idle;
}

const ai::PendingCall* OnlyCard(const ai::ChatPage& page) {
    const auto open = page.OpenCards();
    return open.size() == 1 ? open.front() : nullptr;
}

// Waits for the card to arm (kCardArmSeconds), as a player would take time to read it.
void LetCardArm(ai::ChatPage& page) {
    Pump(page, [] { return false; }, ai::kCardArmSeconds + 0.2);
}

} // namespace

int main() {
    char* script = nullptr;
    size_t length = 0;
    if (_dupenv_s(&script, &length, "NICKEL_AI_E2E_SERVER") != 0 || script == nullptr || !*script) {
        std::printf("skipped: NICKEL_AI_E2E_SERVER (the website's ai/scripts/e2e-server.mjs) is not set\n");
        std::free(script);
        return 77;
    }
    Server server;
    const bool serverUp = server.Start(script);
    std::free(script);
    Check(serverUp, "the AI service started on 127.0.0.1 with the fake model");
    if (!serverUp) {
        return 1;
    }

    FakeRegistry registry;
    registry.list.push_back(MakeModule("kill_aura", "Kill Aura", "Combat", false));
    registry.list.push_back(MakeModule("flight", "Flight", "Movement", true));
    FakeLibrary library;
    ai::tools::RegistrySettingsWriter writer(registry);
    FakeUi ui;
    FakeStatus status;
    TestIdentity identity;

    ai::ClientConfig config;
    config.timeoutMs = 20000;
    config.client = {"1.1.0-beta", "1.26.62.1", "fr-FR"};
    Check(ai::UseLocalTestServer(config, server.origin), "the client accepts the local origin " + server.origin);
    auto chatOwner = std::make_unique<ai::AssistantChat>(std::make_unique<ai::AssistantClient>(config, &identity));
    ai::AssistantChat& chat = *chatOwner;
    ai::tools::Deps deps{registry, library, writer, ui, status};
    ai::AssistantToolRunner tools(deps);
    ai::ChatPage page(&chat, &tools);
    Check(page.Available(), "the panel has a client and tools");

    // ---- 1. text streaming and the quota line
    Check(page.QuotaLine() == "25 messages a day", "before any answer the panel shows the default limit");
    Check(page.Send("hello"), "the first message is sent");
    const std::string hello = "Hello! I am Nickel AI. How can I help?";
    bool sawPartial = false;
    Check(Pump(page, [&] { return Idle(page) && LastAssistant(page) == hello; }, 15,
               [&] {
                   const std::string text = LastAssistant(page);
                   sawPartial = sawPartial || (!text.empty() && text.size() < hello.size());
               }),
          "the answer streams in and the turn ends");
    Check(LastAssistant(page) == hello, "the whole answer: " + LastAssistant(page));
    Check(sawPartial, "part of the answer was shown before the whole of it");
    const std::string conversation = page.Conversation().conversationId;
    Check(!conversation.empty() && !page.Conversation().turnId.empty(), "the turn event gave the ids");
    Check(page.QuotaLine() == "1/5 today", "quota line after the first turn: " + page.QuotaLine());
    Check(page.Conversation().lastStop == ai::StopReason::EndTurn, "the step ended with end_turn");

    // ---- 2. a read tool, then an action the player allows
    Check(page.Send("turn on kill aura"), "the second message is sent");
    Check(Pump(page, [&] { return OnlyCard(page) != nullptr; }), "get_module ran at once, then a card asks to allow");
    if (const ai::PendingCall* call = OnlyCard(page)) {
        Check(call->card.tool == "set_module_enabled" && call->card.title == "Turn on Kill Aura",
              "the card is Turn on Kill Aura: " + call->card.title);
        Check(call->card.lines.size() == 1 && call->card.lines[0].label == "Kill Aura" &&
                      call->card.lines[0].current == "Off" && call->card.lines[0].next == "On",
              "the card shows Kill Aura Off -> On, from the registry");
        Check(!registry.ById("kill_aura")->enabled, "nothing changed before the player answers");
        const std::string id = call->use.id;
        Check(!page.Allow(id), "a click right after the card appears does nothing (it arms first)");
        LetCardArm(page);
        Check(page.Allow(id), "the player allows");
    }
    Check(Pump(page, [&] { return Idle(page); }), "the turn ends after the results");
    Check(registry.ById("kill_aura")->enabled && registry.saves == 1, "Kill Aura is on and saved once");
    Check(LastAssistant(page) == "ok: Kill Aura is on.", "the model got both results right: " + LastAssistant(page));
    Check(page.Conversation().conversationId == conversation, "the same conversation continues");
    Check(page.QuotaLine() == "2/5 today", "a turn with tools counts once: " + page.QuotaLine());

    // ---- 3. an action the player declines
    Check(page.Send("turn off flight"), "the third message is sent");
    Check(Pump(page, [&] { return OnlyCard(page) != nullptr; }), "a card asks to turn off Flight");
    if (const ai::PendingCall* call = OnlyCard(page)) {
        Check(call->card.title == "Turn off Flight", "the card is Turn off Flight: " + call->card.title);
        LetCardArm(page);
        Check(page.Decline(call->use.id), "the player declines");
    }
    Check(Pump(page, [&] { return Idle(page); }), "the turn ends after the refusal");
    Check(registry.ById("flight")->enabled && registry.saves == 1, "Flight is still on and nothing was saved");
    Check(LastAssistant(page) == "ok: Understood, I left Flight as it was.",
          "the model received the declined error: " + LastAssistant(page));

    // ---- 4. write_script, saved held
    Check(page.Send("write a script"), "the fourth message is sent");
    Check(Pump(page, [&] { return OnlyCard(page) != nullptr; }), "a card shows the new script");
    if (const ai::PendingCall* call = OnlyCard(page)) {
        Check(call->card.isScriptWrite && call->card.newScript && call->card.scriptName == "auto_gg",
              "the card is a new script auto_gg");
        Check(call->card.newSource == kScriptSource && call->card.currentSource.empty() && !call->diff.empty(),
              "the card shows the source as a diff");
        Check(library.files.count("auto_gg") == 0, "nothing is written before the player answers");
        LetCardArm(page);
        Check(page.Allow(call->use.id), "the player allows the write");
    }
    Check(Pump(page, [&] { return Idle(page); }), "the turn ends after the write");
    const ScriptInfo* written = library.Info("auto_gg");
    Check(written != nullptr && written->held && library.files["auto_gg"] == kScriptSource,
          "auto_gg is saved with the model's source and held");
    Check(library.started.empty(), "the written script was never started");
    Check(LastAssistant(page).rfind("ok: Saved auto_gg.", 0) == 0, "the model got held:true: " + LastAssistant(page));
    bool heldNote = false;
    for (const auto& note : page.Notes()) {
        heldNote = heldNote || note.text == "Saved (held): " + std::string("Save the new script auto_gg");
    }
    Check(heldNote, "the chat notes the script as saved held");

    // ---- 5. the last message of the day, then the limit
    Check(page.Send("thanks"), "the fifth message is sent");
    Check(Pump(page, [&] { return Idle(page) && LastAssistant(page) == "You're welcome!"; }), "the fifth answer");
    Check(page.QuotaLine() == "5/5 today", "quota line at the limit: " + page.QuotaLine());
    Check(page.QuotaUsedUp() && !page.CanSend("more"), "the panel stops sending at the limit");
    Check(page.SendBlockedReason("more").find("You used all 5 messages for today.") == 0,
          "and says why: " + page.SendBlockedReason("more"));
    // the service enforces it too (a client that ignores the quota line)
    Check(chat.Client().Send("one more"), "the client posts a sixth message anyway");
    Check(Pump(page, [&] { return page.Conversation().state == ai::TurnState::Failed; }), "the service refuses it");
    const ai::ConversationSnapshot& refused = page.Conversation();
    Check(refused.error.code == "quota_exceeded" && refused.error.httpStatus == 429,
          "429 quota_exceeded: " + refused.error.code + " " + std::to_string(refused.error.httpStatus));
    Check(page.ErrorLine() == "You used all your Nickel AI messages for today.", "error line: " + page.ErrorLine());

    // ---- 6. a new conversation forgets the id
    page.NewChat();
    page.Tick(Now(), kServiceNow, true);
    Check(page.Conversation().conversationId.empty() && page.Conversation().messages.empty(),
          "New chat clears the conversation");

    chatOwner.reset(); // stops the client's worker before the server goes away
    std::printf("%s (%d failed)\n", failed == 0 ? "all ai e2e checks passed" : "AI E2E CHECKS FAILED", failed);
    return failed == 0 ? 0 : 1;
}
