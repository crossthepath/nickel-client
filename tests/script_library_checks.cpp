// Script snapshots and script commands (native/script_library.hpp), without Minecraft, the menu or Lua: what the
// Scripting tab, and later the installation of community scripts and the assistant, see of a script (state, modules
// and whether they are on, errors), how scripts are named and found, the tab's search, the names a new script file
// may have, and the command behind a row's Start / Stop button.
// Also the pure rules of held scripts and library writes: the folder scan's table, the held list's format, compile
// errors and revisions.
// Not covered here: the engine's ScriptLibrary (EngineScriptLibrary in script_engine.hpp), which still compiles only
// inside menu.cpp (its file half is tests/script_files_checks.cpp); see docs/architecture/script-management.md.
#include <cctype>
#include <cstdint>
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
    uint64_t serial = 0;
    int state = 0;
    bool held = false;
    std::vector<FakeModule> mods;
    std::deque<std::string> errors;
    size_t memory = 0;
    double frameMs = 0;
};

// Records the commands it receives: the fake that callers of the library (the Scripting tab, PUBLIC's installer, the
// assistant's tools) test their logic against.
class RecordingLibrary final : public ScriptLibrary {
public:
    mutable std::vector<std::pair<std::string, std::string>> calls; // command, script name
    bool answer = true;
    ScriptFileResult result = ScriptFileResult::Done;

    std::vector<ScriptInfo> List() const override { return {}; }

    std::wstring Folder() const override { return L"C:\\scripts"; }

    bool ReadSource(const std::string& name, ScriptSource&) const override {
        calls.emplace_back("read", name);
        return answer;
    }

    std::vector<CompileError> Compile(const std::string&) const override {
        calls.emplace_back("compile", "");
        return {};
    }

    bool HasBackup(const std::string& name) const override {
        calls.emplace_back("has backup", name);
        return answer;
    }

    ScriptFileResult WriteSource(const std::string& name, const std::string&, const std::string&) override {
        calls.emplace_back("write", name);
        return result;
    }

    ScriptFileResult Remove(const std::string& name, const std::string&) override {
        calls.emplace_back("remove", name);
        return result;
    }

    ScriptFileResult RestoreBackup(const std::string& name, const std::string&) override {
        calls.emplace_back("restore", name);
        return result;
    }

    bool Hold(const std::string& name) override {
        calls.emplace_back("hold", name);
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
};

ScriptInfo Named(const char* name, ScriptState state = ScriptState::Stopped, uint64_t serial = 0) {
    ScriptInfo info;
    info.name = name;
    info.state = state;
    info.serial = serial;
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
    script.serial = 42;
    script.state = 1;
    script.held = true;
    script.mods = {{2, "On Module"}, {3, "Off Module"}, {-1, "Unloaded"}, {9, "Out Of Range"}};
    script.errors = {"first:1: a", "second:2: b"};
    script.memory = 3 * 1048576;
    script.frameMs = 0.25;

    const ScriptInfo info = DescribeScript(script, registry);
    Check(info.name == "Aura Helper" && info.file == script.file && info.serial == 42,
          "name, serial and file are copied");
    Check(info.state == ScriptState::Running && info.held, "the state and the held flag are the engine's");
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
}

// The Scripting tab's selection (ScriptingSelection in script_ui.hpp is FindSelectedScript on the tab's serial).
void Selection() {
    std::vector<ScriptInfo> list = {Named("alpha", ScriptState::Stopped, 1), Named("fly", ScriptState::Running, 2)};
    uint64_t selected = 0;
    Check(FindSelectedScript(list, selected) == -1 && selected == 0, "nothing selected at first");

    selected = 2;
    Check(FindSelectedScript(list, selected) == 1 && selected == 2, "the selected script is found");
    list.insert(list.begin(), Named("aardvark", ScriptState::Stopped, 3));
    Check(FindSelectedScript(list, selected) == 2 && selected == 2,
          "the selection follows the script when a script is added and the list is sorted again");

    list.erase(list.begin() + 2);
    Check(FindSelectedScript(list, selected) == -1 && selected == 0, "the selection is cleared when the script is gone");
    list.push_back(Named("fly", ScriptState::Running, 4));
    Check(FindSelectedScript(list, selected) == -1 && selected == 0,
          "a file of the same name that comes back is a new script and is not selected again");

    selected = 4;
    list.back() = Named("Fly", ScriptState::Running, 5); // a case-only rename: the engine removes and adds the script
    Check(FindSelectedScript(list, selected) == -1 && selected == 0, "a renamed script is not selected any more");

    selected = 4; // gone while the tab was not drawn: cleared the next time the tab looks
    Check(FindSelectedScript(list, selected) == -1 && selected == 0, "a stale selection is cleared when it is read");
}

void Utf8() {
    const char* const good[] = {"", "plain", "\xc2\x80", "\xdf\xbf", "\xe0\xa0\x80", "\xed\x9f\xbf", "\xee\x80\x80",
                                "\xef\xbf\xbf", "\xf0\x90\x80\x80", "\xf4\x8f\xbf\xbf", "\xc3\xa9t\xc3\xa9"};
    bool ok = true;
    for (const char* text : good) {
        ok &= WellFormedUtf8(text);
    }
    Check(ok, "well-formed UTF-8 is accepted, U+0080 to U+10FFFF at every length boundary");

    const char* const bad[] = {"\x80", "a\xbf", "\xc0\x80", "\xc1\xbf", "\xc2", "\xc2\x41", "\xe0\x9f\xbf",
                               "\xed\xa0\x80", "\xed\xbf\xbf", "\xe1\x80", "\xe1\x80\x41", "\xf0\x8f\xbf\xbf",
                               "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xff", "\xf0\x90\x80"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        if (WellFormedUtf8(bad[i])) {
            std::printf("     accepted malformed UTF-8 #%d\n", static_cast<int>(i));
            ok = false;
        }
    }
    Check(ok, "stray continuation bytes, overlong forms, surrogates, values above U+10FFFF and cut sequences refused");

    Check(WithoutUtf8Bom("\xef\xbb\xbfprint(1)") == "print(1)", "a leading byte order mark is removed");
    Check(WithoutUtf8Bom("print(1)") == "print(1)" && WithoutUtf8Bom("").empty()
                  && WithoutUtf8Bom("\xef\xbb") == "\xef\xbb",
          "a source without a byte order mark is unchanged");
    Check(WithoutUtf8Bom("x\xef\xbb\xbf") == "x\xef\xbb\xbf", "a byte order mark that is not first is kept");
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

    ok = true;
    const char* const superscript[] = {"COM\xc2\xb9", "com\xc2\xb2", "LPT\xc2\xb3", "lpt\xc2\xb9.x"};
    for (const char* name : superscript) {
        ok &= !ValidScriptName(name);
    }
    Check(ok && ValidScriptName("COM\xc2\xb2" "x") && ValidScriptName("LPT\xc2\xb4"),
          "COM and LPT with a superscript 1, 2 or 3 are device names too");
    Check(!ValidScriptName("bad\xc3") && !ValidScriptName("\xed\xa0\x80x") && !ValidScriptName("a\x80" "b"),
          "a malformed UTF-8 name is refused (it would not round-trip through the file name)");
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

void CompileErrors() {
    CompileError error = ParseCompileError(":12: Expected 'end' (to close 'function' at line 1), got <eof>");
    Check(error.line == 12 && error.message == "Expected 'end' (to close 'function' at line 1), got <eof>",
          "\":12: msg\" gives line 12 and the message");
    error = ParseCompileError("out of memory");
    Check(error.line == 0 && error.message == "out of memory", "a message without a line gives line 0 and all the text");
    error = ParseCompileError(":7:");
    Check(error.line == 7 && error.message.empty(), "an empty message after the line");
    error = ParseCompileError("");
    Check(error.line == 0 && error.message.empty(), "an empty text");
    error = ParseCompileError(":99999999999999999999: too far");
    Check(error.line == 2147483647 && error.message == "too far", "a very large line number is saturated");
    error = ParseCompileError(":12 no colon");
    Check(error.line == 0 && error.message == ":12 no colon", "a line without its closing colon is part of the message");
    error = ParseCompileError("::x");
    Check(error.line == 0 && error.message == "::x", "a colon without digits is part of the message");
    error = ParseCompileError(":3:  two spaces");
    Check(error.line == 3 && error.message == " two spaces", "only the one space Luau writes is removed");
}

bool Is(ScanStep step, bool unload, bool load, bool compileOnly) {
    return step.unload == unload && step.load == load && step.compileOnly == compileOnly;
}

// Every row of the table in docs/architecture/script-management-next.md, section 1.
// DecideScan(isNew, stampChanged, libChanged, running, held)
void Scan() {
    // not held: today's behaviour
    Check(Is(DecideScan(true, false, false, false, false), false, true, false), "not held, new file: load");
    Check(Is(DecideScan(true, false, true, false, false), false, true, false), "not held, new file while lib changed: load");
    Check(Is(DecideScan(false, true, false, false, false), false, true, false)
                  && Is(DecideScan(false, true, false, true, false), false, true, false),
          "not held, file changed: load (also when it runs, and after the user's Stop)");
    Check(Is(DecideScan(false, false, true, true, false), false, true, false), "not held, lib changed while running: reload");
    Check(Is(DecideScan(false, false, true, false, false), false, false, false),
          "not held, lib changed while stopped: nothing");
    Check(Is(DecideScan(false, false, false, true, false), false, false, false)
                  && Is(DecideScan(false, false, false, false, false), false, false, false),
          "not held, nothing changed: nothing");
    // held
    Check(Is(DecideScan(true, false, false, false, true), false, false, true), "held, new file: compile only");
    Check(Is(DecideScan(false, true, false, false, true), false, false, true), "held, file changed: compile only");
    Check(Is(DecideScan(false, true, false, true, true), true, false, true),
          "held, file changed while running: unload, then compile only");
    Check(Is(DecideScan(false, false, true, false, true), false, false, false), "held, lib changed: nothing");
    Check(Is(DecideScan(false, false, false, false, true), false, false, false), "held, nothing changed: nothing");
    Check(Is(DecideScan(false, false, false, true, true), true, false, false),
          "held and running (Hold from another thread): unload");
    Check(Is(DecideScan(false, false, true, true, true), true, false, false), "held and running while lib changed: unload");
    bool neverLoads = true;
    for (int bits = 0; bits < 16; ++bits) {
        neverLoads &= !DecideScan(bits & 1, bits & 2, bits & 4, bits & 8, true).load;
    }
    Check(neverLoads, "a held script never loads");
}

void HeldList() {
    const std::vector<std::string> names = {"Kill Aura", "\xc3\xa9t\xc3\xa9", "chat logger"};
    Check(FormatHeldList(names) == "Kill Aura\n\xc3\xa9t\xc3\xa9\nchat logger\n", "one name per line, each ended by \\n");
    Check(ParseHeldList(FormatHeldList(names)) == names, "a round trip keeps the names and their order");
    Check(ParseHeldList("Kill Aura\r\nchat logger\r\n") == std::vector<std::string>({"Kill Aura", "chat logger"}),
          "CRLF reads as LF");
    Check(ParseHeldList("\n\na\n\r\n\nb") == std::vector<std::string>({"a", "b"}),
          "blank lines are dropped; the last line needs no line end");
    Check(ParseHeldList("Fly\nfly\nFLY\nother\n") == std::vector<std::string>({"Fly", "other"}),
          "repeats differing only in case are dropped, the first is kept");
    Check(ParseHeldList("ok\na/b\n.hidden\ncon\nbad\xc3\n" + std::string(65, 'x') + "\nlast\n")
                  == std::vector<std::string>({"ok", "last"}),
          "invalid names are dropped");
    Check(ParseHeldList("\xc3\xa9t\xc3\xa9\n") == std::vector<std::string>({"\xc3\xa9t\xc3\xa9"}), "UTF-8 names are kept");
    Check(ParseHeldList("").empty() && FormatHeldList({}).empty(), "an empty list is an empty file");
}

void Revisions() {
    // FIPS 180-2, appendix B.1: SHA-256("abc")
    const uint8_t abc[32] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
                             0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    const std::string hex = Sha256Hex(abc);
    Check(hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "a digest is 64 lowercase hex characters (the FIPS 180-2 \"abc\" digest)");
    Check(ValidSha256Hex(hex), "a revision is valid");
    std::string upper = hex;
    for (char& c : upper) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    Check(!ValidSha256Hex(upper), "upper case is refused");
    Check(!ValidSha256Hex(hex.substr(0, 63)) && !ValidSha256Hex(hex + "0") && !ValidSha256Hex(""),
          "63 or 65 characters, or none, are refused");
    Check(!ValidSha256Hex("sha256:" + hex.substr(7)) && !ValidSha256Hex(std::string(63, 'a') + "g"),
          "a prefix or a non-hex character is refused");
}

void Status() {
    ScriptInfo script = Named("installed", ScriptState::Stopped);
    script.held = true;
    Check(std::string(ScriptStatusText(script)) == "Not started", "a held script that compiles is Not started");
    script.state = ScriptState::FailedToLoad;
    Check(std::string(ScriptStatusText(script)) == "Failed to load", "a held script that does not compile failed to load");
    script.held = false;
    script.state = ScriptState::Stopped;
    Check(std::string(ScriptStatusText(script)) == "Stopped", "a script that is not held keeps its state's text");
}

void FileCommands() {
    RecordingLibrary library;
    library.result = ScriptFileResult::Conflict;
    Check(library.WriteSource("a", "print(1)", "") == ScriptFileResult::Conflict && library.calls.back().first == "write",
          "the fake returns the result it is given, for a caller's conflict path");
    Check(library.Hold("a") && library.calls.back().first == "hold" && library.calls.back().second == "a",
          "Hold is recorded with its name");
}

} // namespace

int main() {
    States();
    Snapshot();
    Names();
    Selection();
    Utf8();
    Search();
    FileNames();
    Commands();
    CompileErrors();
    Scan();
    HeldList();
    Revisions();
    Status();
    FileCommands();
    std::printf(failed ? "%d check(s) FAILED\n" : "all checks passed\n", failed);
    return failed ? 1 : 0;
}
