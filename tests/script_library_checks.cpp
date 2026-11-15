// Script snapshots and script commands (native/script_library.hpp), without Minecraft, the menu or Lua: what the
// Scripting tab, and later the installation of community scripts and the assistant, see of a script (state, modules
// and whether they are on, errors), how scripts are named and found, the tab's search, the names a new script file
// may have, and the command behind a row's Start / Stop button.
// Not covered here: the engine's ScriptLibrary (EngineScriptLibrary in script_engine.hpp), which still compiles only
// inside menu.cpp; see docs/architecture/script-management.md.
#include <cstdio>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "script_library.hpp"

namespace {

using namespace overlay;

int failed = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++failed;
    }
}

// Only the module list matters to a snapshot.
class FakeRegistry final : public ModuleRegistry {
public:
    std::vector<Module> list;

    int Count() const override { return static_cast<int>(list.size()); }

    Module& At(int index) override { return list.at(index); }

    int Find(const std::string& id) const override {
        for (int i = 0; i < Count(); ++i) {
            if (list[i].id == id) {
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

    void SaveModule(int, const Setting*) override {}

    std::string ReadPreference(const std::string&, const std::string&, size_t) const override { return {}; }

    int ReadPreferenceInt(const std::string&, const std::string&, int fallback) const override { return fallback; }

    int KeyOf(const std::string&) const override { return 0; }

    void BindKey(const std::string&, const std::string&, int) override {}
};

// The members of the engine's Script that a snapshot reads, with the same types.
struct FakeModule {
    int index = -1;
    std::string name;
};

struct FakeScript {
    std::wstring file;
    std::string name;
    int state = 0;
    std::vector<FakeModule> mods;
    std::deque<std::string> errors;
    size_t memory = 0;
    double frameMs = 0;
};

// Records the commands it receives.
class RecordingLibrary final : public ScriptLibrary {
public:
    mutable std::vector<std::pair<std::string, std::string>> calls; // command, script name
    bool answer = true;

    std::vector<ScriptInfo> List() const override { return {}; }

    std::wstring Folder() const override { return L"C:\\scripts"; }

    bool ReadSource(const std::string& name, std::string&) const override {
        calls.emplace_back("read", name);
        return answer;
    }

    bool Start(const std::string& name) override {
        calls.emplace_back("start", name);
        return answer;
    }

    bool Stop(const std::string& name) override {
        calls.emplace_back("stop", name);
        return answer;
    }

    bool WriteSource(const std::string& name, const std::string&) override {
        calls.emplace_back("write", name);
        return answer;
    }
};

ScriptInfo Named(const char* name, ScriptState state = ScriptState::Stopped) {
    ScriptInfo info;
    info.name = name;
    info.state = state;
    return info;
}

void States() {
    Check(ScriptStateOf(0) == ScriptState::Stopped && ScriptStateOf(1) == ScriptState::Running
                  && ScriptStateOf(2) == ScriptState::StoppedAfterErrors && ScriptStateOf(3) == ScriptState::FailedToLoad,
          "engine states 0 to 3");
    Check(ScriptStateOf(-1) == ScriptState::Stopped && ScriptStateOf(7) == ScriptState::FailedToLoad,
          "out-of-range engine states are clamped, as the tab showed them");
    Check(std::string(ScriptStateName(ScriptState::Stopped)) == "Stopped"
                  && std::string(ScriptStateName(ScriptState::Running)) == "Running"
                  && std::string(ScriptStateName(ScriptState::StoppedAfterErrors)) == "Stopped after errors"
                  && std::string(ScriptStateName(ScriptState::FailedToLoad)) == "Failed to load",
          "the four texts of the Scripting tab");
    Check(std::string(ScriptStateName(static_cast<ScriptState>(9))) == "Failed to load",
          "a state name is never out of bounds");
}

void Snapshot() {
    FakeRegistry registry;
    registry.list.resize(4);
    registry.list[2].enabled = true;
    registry.list[3].enabled = false;

    FakeScript script;
    script.file = L"C:\\scripts\\Aura Helper.luau";
    script.name = "Aura Helper";
    script.state = 1;
    script.mods = {{2, "On Module"}, {3, "Off Module"}, {-1, "Unloaded"}, {9, "Out Of Range"}};
    script.errors = {"first:1: a", "second:2: b"};
    script.memory = 3 * 1048576;
    script.frameMs = 0.25;

    const ScriptInfo info = DescribeScript(script, registry);
    Check(info.name == "Aura Helper" && info.file == script.file, "name and file are copied");
    Check(info.state == ScriptState::Running, "the state is the engine's");
    Check(info.memory == script.memory && info.frameMs == 0.25, "memory and frame time are copied");
    Check(info.modules.size() == 4 && info.modules[0].name == "On Module" && info.modules[0].index == 2
                  && info.modules[3].name == "Out Of Range",
          "every module of the script, in declaration order, with its index");
    Check(info.modules[0].enabled && !info.modules[1].enabled, "a module is on when its registry module is enabled");
    Check(!info.modules[2].enabled && !info.modules[3].enabled,
          "a module without a registry index, or with an index out of range, is off");
    Check(info.errors.size() == 2 && info.errors[0] == "first:1: a" && info.errors[1] == "second:2: b",
          "errors are copied oldest first");

    script.state = 0;
    script.mods.clear();
    script.errors.clear();
    registry.list[2].enabled = false;
    Check(info.state == ScriptState::Running && info.modules.size() == 4 && info.modules[0].enabled
                  && info.errors.size() == 2,
          "a snapshot does not change when the engine's script or the modules change afterwards");
}

void Names() {
    Check(SameScriptName("Kill Aura", "kill aura") && SameScriptName("ABC", "abc"), "names are compared without case");
    Check(!SameScriptName("abc", "abcd") && !SameScriptName("abc", "abd"), "different names differ");

    std::vector<ScriptInfo> list = {Named("alpha"), Named("Beta"), Named("gamma")};
    Check(FindScript(list, "beta") == 1 && FindScript(list, "GAMMA") == 2, "a script is found by name, any case");
    Check(FindScript(list, "delta") == -1 && FindScript(list, "") == -1, "a missing or empty name is not found");

    // The tab keeps its selection by name: when a script is added the list is sorted again and indices shift.
    const std::string selected = "Beta";
    list.insert(list.begin(), Named("aardvark"));
    Check(FindScript(list, selected) == 2, "the selected script is found again after the list is re-sorted");
    list.erase(list.begin() + 2);
    Check(FindScript(list, selected) == -1, "a selected script that disappeared is not found (the tab clears it)");
}

void Search() {
    const std::vector<ScriptInfo> list = {Named("Auto Totem"), Named("chat logger"), Named("TOTEM counter")};
    Check(MatchingScripts(list, "") == std::vector<int>({0, 1, 2}), "an empty search shows every script");
    Check(MatchingScripts(list, "totem") == std::vector<int>({0, 2}), "search ignores case and keeps the order");
    Check(MatchingScripts(list, "LOG") == std::vector<int>({1}), "search matches inside the name");
    Check(MatchingScripts(list, "none").empty(), "no match shows nothing");
    Check(MatchingScripts({}, "x").empty() && MatchingScripts({}, "").empty(), "an empty list shows nothing");
}

void FileNames() {
    const char* const good[] = {"Kill Aura", "my-script_2", "a.b", "\xc3\xa9t\xc3\xa9", "console", "COM0", "com10",
                                "lpt", "x.dd", "d"};
    bool ok = true;
    for (const char* name : good) {
        ok &= ValidScriptName(name);
    }
    Check(ok, "ordinary names, UTF-8 names and names close to reserved ones are accepted");
    Check(ValidScriptName(std::string(64, 'a')) && !ValidScriptName(std::string(65, 'a')), "at most 64 bytes");

    const char* const bad[] = {"", "a/b", "a\\b", "a:b", "x?", "x*", "a\"b", "<a>", "a|b", "tab\there", ".hidden",
                               " lead", "trail.", "trail ", "types.d", "TYPES.D", "con", "CON.x", "Com1", "lpt9",
                               "Nul", "aux .x", "prn"};
    for (const char* name : bad) {
        if (ValidScriptName(name)) {
            std::printf("     accepted: \"%s\"\n", name);
            ok = false;
        }
    }
    Check(ok, "separators, reserved characters, leading dot or space, trailing dot or space, .d, device names refused");
}

void Commands() {
    RecordingLibrary library;
    Check(ToggleScript(library, Named("a", ScriptState::Running)) && library.calls.back().first == "stop"
                  && library.calls.back().second == "a",
          "Start / Stop on a running script stops it");
    ToggleScript(library, Named("b", ScriptState::Stopped));
    const bool stoppedStarts = library.calls.back().first == "start" && library.calls.back().second == "b";
    ToggleScript(library, Named("c", ScriptState::StoppedAfterErrors));
    const bool erroredStarts = library.calls.back().first == "start" && library.calls.back().second == "c";
    ToggleScript(library, Named("d", ScriptState::FailedToLoad));
    const bool failedStarts = library.calls.back().first == "start" && library.calls.back().second == "d";
    Check(stoppedStarts && erroredStarts && failedStarts,
          "Start / Stop starts a stopped script, one stopped after errors, and one that failed to load");
    Check(library.calls.size() == 4, "one command per press");
    library.answer = false;
    Check(!ToggleScript(library, Named("gone", ScriptState::Running)), "the library's answer is returned");
}

} // namespace

int main() {
    States();
    Snapshot();
    Names();
    Search();
    FileNames();
    Commands();
    std::printf(failed ? "%d check(s) FAILED\n" : "all checks passed\n", failed);
    return failed ? 1 : 0;
}
