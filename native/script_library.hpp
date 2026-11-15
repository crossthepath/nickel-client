#pragma once
// The scripts of Nickel as components other than the Luau engine see them: a read-only snapshot and a few commands.
//
// The engine (script_engine.hpp) implements ScriptLibrary and hands it out through script_engine_api.hpp
// (ScriptsLibrary()). The Scripting tab (script_ui.hpp) uses only this interface, never the engine's script list or
// its lock; later components that manage scripts without the menu use it too: installing community scripts (PUBLIC)
// and an assistant that creates and fixes scripts (Nickel AI). No Lua, no Windows, no menu here:
// tests/script_library_checks.cpp. Design: docs/architecture/script-management.md.
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "module_registry.hpp"

namespace overlay {

// The state of a script. The values are the engine's (Script::state).
enum class ScriptState : int {
    Stopped = 0,            // never started, or stopped by the user
    Running = 1,            // loaded, its modules and handlers run
    StoppedAfterErrors = 2, // five errors within ten seconds; runs again when the file is saved or it is started
    FailedToLoad = 3,       // the file does not compile, or its main chunk failed
};

// The state for an engine value; values outside 0..3 are clamped, as the Scripting tab always showed them.
inline ScriptState ScriptStateOf(int state) {
    return static_cast<ScriptState>(state < 0 ? 0 : state > 3 ? 3 : state);
}

// The text shown in the Scripting tab.
inline const char* ScriptStateName(ScriptState state) {
    static const char* const kNames[] = {"Stopped", "Running", "Stopped after errors", "Failed to load"};
    return kNames[static_cast<int>(ScriptStateOf(static_cast<int>(state)))];
}

// A module declared by a script.
struct ScriptModuleInfo {
    std::string name;   // as declared by the script (module("Name", ...))
    int index = -1;     // index in the ModuleRegistry, or -1
    bool enabled = false;
};

// One script, copied from the engine: it stays valid after the engine reloads or removes the script.
struct ScriptInfo {
    std::string name;   // the file name without ".luau", UTF-8; identifies the script (compared without case)
    uint64_t serial = 0; // identity of this entry: a file that disappears and comes back (or is renamed) gets a new one
    std::wstring file;  // full path of the .luau file
    ScriptState state = ScriptState::Stopped;
    bool held = false; // installed or written but not started: it does not run until Start (scripts\.nickel\held.txt)
    std::vector<ScriptModuleInfo> modules;
    std::vector<std::string> errors; // latest errors with their line, oldest first (the engine keeps 8)
    size_t memory = 0;               // bytes used by the script's Luau VM
    double frameMs = 0;              // time spent in the script per frame, averaged by the engine
};

// The snapshot of one engine script. `EngineScript` is the engine's Script (or a test double) with the members name,
// serial, file, state (int), held, mods (each with index and name), errors (any sequence of strings), memory and frameMs; a module
// is enabled when its index is valid in `registry` and that module is enabled.
template <class EngineScript>
ScriptInfo DescribeScript(const EngineScript& script, ModuleRegistry& registry) {
    ScriptInfo info;
    info.name = script.name;
    info.serial = script.serial;
    info.file = script.file;
    info.state = ScriptStateOf(script.state);
    info.held = script.held;
    for (const auto& module : script.mods) {
        ScriptModuleInfo entry;
        entry.name = module.name;
        entry.index = module.index;
        entry.enabled = module.index >= 0 && module.index < registry.Count() && registry.At(module.index).enabled;
        info.modules.push_back(entry);
    }
    info.errors.assign(script.errors.begin(), script.errors.end());
    info.memory = script.memory;
    info.frameMs = script.frameMs;
    return info;
}

// The state of a script as the Scripting tab shows it: a held script that compiles is "Not started" (installed or
// written by a component, waiting for Start), any other state is ScriptStateName's.
inline const char* ScriptStatusText(const ScriptInfo& script) {
    return script.held && script.state == ScriptState::Stopped ? "Not started" : ScriptStateName(script.state);
}

// True when `a` and `b` name the same script: equal ignoring the case of ASCII letters, as the engine compares
// file names (_stricmp).
inline bool SameScriptName(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// Index of the script called `name` in `scripts`, or -1 (also for an empty name).
inline int FindScript(const std::vector<ScriptInfo>& scripts, const std::string& name) {
    if (name.empty()) {
        return -1;
    }
    for (size_t i = 0; i < scripts.size(); ++i) {
        if (SameScriptName(scripts[i].name, name)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Indices of the scripts whose name contains `query`, ignoring the case of ASCII letters; every script when `query`
// is empty. The order of `scripts` is kept.
inline std::vector<int> MatchingScripts(const std::vector<ScriptInfo>& scripts, const std::string& query) {
    auto lower = [](std::string text) {
        for (char& c : text) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return text;
    };
    const std::string wanted = lower(query);
    std::vector<int> shown;
    for (size_t i = 0; i < scripts.size(); ++i) {
        if (wanted.empty() || lower(scripts[i].name).find(wanted) != std::string::npos) {
            shown.push_back(static_cast<int>(i));
        }
    }
    return shown;
}

// The Scripting tab's selection: the serial of the selected script, 0 for none. Returns its index in `scripts`, or -1
// and clears `selected` when that entry is gone. As the engine kept it before the tab used snapshots: a script that
// disappears stays unselected even when a file of the same name comes back, and the index follows the script when
// the list is sorted again.
inline int FindSelectedScript(const std::vector<ScriptInfo>& scripts, uint64_t& selected) {
    if (selected != 0) {
        for (size_t i = 0; i < scripts.size(); ++i) {
            if (scripts[i].serial == selected) {
                return static_cast<int>(i);
            }
        }
    }
    selected = 0;
    return -1;
}

// True when `text` is well-formed UTF-8 (RFC 3629): no stray continuation byte, no overlong form, no surrogate
// (U+D800 to U+DFFF), nothing above U+10FFFF, no truncated sequence.
inline bool WellFormedUtf8(const std::string& text) {
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        size_t extra = 0;
        unsigned char low = 0x80, high = 0xbf; // allowed range of the second byte
        if (lead < 0x80) {
            ++i;
            continue;
        } else if (lead >= 0xc2 && lead <= 0xdf) {
            extra = 1;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            extra = 2;
            if (lead == 0xe0) {
                low = 0xa0; // overlong below U+0800
            } else if (lead == 0xed) {
                high = 0x9f; // surrogates
            }
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            extra = 3;
            if (lead == 0xf0) {
                low = 0x90; // overlong below U+10000
            } else if (lead == 0xf4) {
                high = 0x8f; // above U+10FFFF
            }
        } else {
            return false; // continuation byte, C0, C1 or F5 to FF
        }
        if (text.size() - i <= extra) {
            return false;
        }
        for (size_t k = 1; k <= extra; ++k) {
            const unsigned char c = static_cast<unsigned char>(text[i + k]);
            if (k == 1 ? (c < low || c > high) : (c < 0x80 || c > 0xbf)) {
                return false;
            }
        }
        i += extra + 1;
    }
    return true;
}

// `source` without a leading UTF-8 byte order mark (EF BB BF), which Luau does not accept.
inline std::string WithoutUtf8Bom(const std::string& source) {
    return source.compare(0, 3, "\xef\xbb\xbf") == 0 ? source.substr(3) : source;
}

// True when `name` can be the name of a new script file, `<scripts folder>\<name>.luau`, that the engine will run:
// 1 to 64 bytes of well-formed UTF-8 (a malformed name would become U+FFFD in the file name, so the script could not
// be found by its name); no control character and none of < > : " / \ | ? *; not starting with a dot or a space
// (the engine keeps its own files there, such as .storage); not ending with a dot or a space (Windows drops them);
// not ending with ".d" (a *.d.luau file is a type declaration and never runs); not a Windows device name (CON, PRN,
// AUX, NUL, COM1 to COM9, LPT1 to LPT9, and COM or LPT followed by a superscript 1, 2 or 3, also before a dot).
inline bool ValidScriptName(const std::string& name) {
    if (name.empty() || name.size() > 64 || !WellFormedUtf8(name)) {
        return false;
    }
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f) {
            return false;
        }
        for (char bad : std::string("<>:\"/\\|?*")) {
            if (c == bad) {
                return false;
            }
        }
    }
    if (name.front() == '.' || name.front() == ' ' || name.back() == '.' || name.back() == ' ') {
        return false;
    }
    if (name.size() >= 2 && name[name.size() - 2] == '.' && std::tolower(static_cast<unsigned char>(name.back())) == 'd') {
        return false;
    }
    std::string base = name.substr(0, name.find('.'));
    while (!base.empty() && base.back() == ' ') {
        base.pop_back();
    }
    for (char& c : base) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL") {
        return false;
    }
    if (base.compare(0, 3, "COM") == 0 || base.compare(0, 3, "LPT") == 0) {
        const std::string digit = base.substr(3);
        if ((digit.size() == 1 && digit[0] >= '1' && digit[0] <= '9') || digit == "\xc2\xb9" || digit == "\xc2\xb2" ||
            digit == "\xc2\xb3") {
            return false;
        }
    }
    return true;
}

// A Luau compile error. line is 0 when the compiler gave none.
struct CompileError {
    int line = 0;
    std::string message;
};

// The error Luau puts after the 0 byte of failed bytecode (":12: Expected 'end' ..."), as a CompileError: the line
// (saturated at the largest int) and the message after ": ". Text that does not start with ":<digits>:" is the whole
// message, with line 0.
inline CompileError ParseCompileError(const std::string& text) {
    CompileError error;
    size_t at = 1;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
        ++at;
    }
    if (text.size() < 3 || text[0] != ':' || at == 1 || at >= text.size() || text[at] != ':') {
        error.message = text;
        return error;
    }
    long long line = 0;
    for (size_t i = 1; i < at; ++i) {
        line = line * 10 + (text[i] - '0');
        if (line > 2147483647LL) {
            line = 2147483647LL;
        }
    }
    error.line = static_cast<int>(line);
    ++at;
    if (at < text.size() && text[at] == ' ') {
        ++at;
    }
    error.message = text.substr(at);
    return error;
}

// The text of a script and its revision: the SHA-256 of its bytes on disk, 64 lowercase hex characters.
struct ScriptSource {
    std::string text;
    std::string sha256;
};

// 32 bytes as 64 lowercase hex characters.
inline std::string Sha256Hex(const uint8_t digest[32]) {
    static const char kDigits[] = "0123456789abcdef";
    std::string hex(64, '0');
    for (int i = 0; i < 32; ++i) {
        hex[2 * i] = kDigits[digest[i] >> 4];
        hex[2 * i + 1] = kDigits[digest[i] & 15];
    }
    return hex;
}

// True when `text` is 64 lowercase hex characters (a revision as Sha256Hex writes it).
inline bool ValidSha256Hex(const std::string& text) {
    if (text.size() != 64) {
        return false;
    }
    for (char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

// The result of a write, a restore or a removal.
enum class ScriptFileResult {
    Done,        // written, restored or removed
    InvalidName, // not a name in List() and not ValidScriptName
    TooLarge,    // more than kMaxScriptBytes after WithoutUtf8Bom
    Conflict,    // the file is not at the expected revision (or exists when it must not)
    NotFound,    // RestoreBackup: no backup
    Failed,      // the disk refused; the script file did not change
};
constexpr size_t kMaxScriptBytes = 1024 * 1024;

// The held list (scripts\.nickel\held.txt): one name per line; blank lines, invalid names (ValidScriptName) and
// repeats (ignoring case, the first is kept) are dropped; '\r' is ignored, so a list saved with CRLF reads the same.
inline std::vector<std::string> ParseHeldList(const std::string& text) {
    std::vector<std::string> names;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line;
        for (size_t i = start; i < end; ++i) {
            if (text[i] != '\r') {
                line += text[i];
            }
        }
        bool repeated = false;
        for (const std::string& name : names) {
            repeated = repeated || SameScriptName(name, line);
        }
        if (!line.empty() && !repeated && ValidScriptName(line)) {
            names.push_back(line);
        }
        start = end + 1;
    }
    return names;
}

// The text of the held list: each name followed by '\n'.
inline std::string FormatHeldList(const std::vector<std::string>& names) {
    std::string text;
    for (const std::string& name : names) {
        text += name;
        text += '\n';
    }
    return text;
}

// What the folder scan does with one file (docs/architecture/script-management-next.md, section 1). An unheld script
// behaves as before held scripts existed: a new or changed file loads (which clears the user's in-memory Stop), a
// change under lib/ reloads it if it runs. A held script never loads: a new or changed file is compiled only (its
// errors show, nothing runs), and a held script that still runs (Hold from another thread) is unloaded.
struct ScanStep {
    bool unload = false;      // unload the running script first (its state becomes Stopped)
    bool load = false;        // load and run the file
    bool compileOnly = false; // compile without a VM: Stopped, or FailedToLoad with the error
};

// isNew: first time the scan sees the file; stampChanged: its time or size changed; libChanged: a lib/ file changed;
// running: Script::state == Running; held: the name is in the held list.
inline ScanStep DecideScan(bool isNew, bool stampChanged, bool libChanged, bool running, bool held) {
    ScanStep step;
    if (held) {
        step.unload = running;
        step.compileOnly = isNew || stampChanged;
        return step;
    }
    step.load = isNew || stampChanged || (libChanged && running);
    return step;
}

// The scripts and what can be done with them. Every call except Start and Stop may be made from any thread: they take
// the engine's locks themselves and run no Lua (a held or removed script is unloaded by the next folder scan, on the
// render thread). Start and Stop run the script's Lua (its main chunk, its modules' enable and disable handlers) and
// change the menu's modules: render thread only, never from inside a script callback. A file written or restored
// through the library is held: it does not run before Start.
// Scripts are named by ScriptInfo::name; a name that is not in List() makes Start, Stop, Hold and ReadSource return
// false.
class ScriptLibrary {
public:
    virtual ~ScriptLibrary() = default;

    // ---- reading

    // Every script of the scripts folder, sorted by name (ignoring case). Any thread.
    virtual std::vector<ScriptInfo> List() const = 0;

    // The scripts folder (%APPDATA%\Nickel\scripts in the game), or empty when it is not known. Any thread.
    virtual std::wstring Folder() const = 0;

    // The source of script `name` as it is on disk (UTF-8 Luau) and its revision, both from the same read. False when
    // the script or its file is missing. Any thread.
    virtual bool ReadSource(const std::string& name, ScriptSource& source) const = 0;

    // Luau compile errors of `source` (empty: it compiles), with the engine's options and without a leading UTF-8 byte
    // order mark, so that what compiles here is what loads; no VM is created. Any thread.
    virtual std::vector<CompileError> Compile(const std::string& source) const = 0;

    // True when script `name` has a backup (scripts\.nickel\backup\<name>.luau.bak), also after Remove. A file check:
    // call it for one script, not for every row. Any thread.
    virtual bool HasBackup(const std::string& name) const = 0;

    // ---- files (any thread; a written or restored script is held)

    // Creates or replaces the file of script `name` with `source` (UTF-8 Luau), in one step (the file never appears
    // half written; a leading UTF-8 byte order mark is dropped). expectedSha256: the revision last read, or empty when
    // the file must not exist. The replaced file becomes the backup. The script is held: a running script stops at the
    // next folder scan and nothing runs until Start; List() shows its compile errors within about a second. `name` is
    // a name in List() or a ValidScriptName.
    virtual ScriptFileResult WriteSource(const std::string& name, const std::string& source,
                                         const std::string& expectedSha256) = 0;

    // Deletes the file of script `name` when it is at revision expectedSha256 (required: empty gives Conflict), after
    // copying it to the backup; the script is no longer held. It is unloaded at the next folder scan.
    virtual ScriptFileResult Remove(const std::string& name, const std::string& expectedSha256) = 0;

    // WriteSource with the backup's bytes: the backup becomes the file, and the replaced file the backup, so a second
    // restore undoes the first. NotFound without a backup. expectedSha256 as for WriteSource.
    virtual ScriptFileResult RestoreBackup(const std::string& name, const std::string& expectedSha256) = 0;

    // Keeps script `name` stopped across restarts until Start; a running script stops at the next folder scan. False
    // when the name is not in List() or the held list cannot be saved.
    virtual bool Hold(const std::string& name) = 0;

    // ---- running (render thread only)

    // Loads script `name` again from its file and runs it (the Start and Reload buttons): a script stopped by the
    // user, stopped after errors or that failed to load runs again. Its modules keep their saved state and key. A held
    // script is released first (it no longer is held, across restarts). Render thread only.
    virtual bool Start(const std::string& name) = 0;

    // Turns the modules of script `name` off and unloads it; it stays stopped until it is started or its file is saved.
    // Not persisted, as the tab's Stop button always was. Render thread only.
    virtual bool Stop(const std::string& name) = 0;
};

// The Start / Stop button of a row in the Scripting tab: stops a running script, starts any other.
inline bool ToggleScript(ScriptLibrary& library, const ScriptInfo& script) {
    return script.state == ScriptState::Running ? library.Stop(script.name) : library.Start(script.name);
}

} // namespace overlay
