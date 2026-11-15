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
    std::vector<ScriptModuleInfo> modules;
    std::vector<std::string> errors; // latest errors with their line, oldest first (the engine keeps 8)
    size_t memory = 0;               // bytes used by the script's Luau VM
    double frameMs = 0;              // time spent in the script per frame, averaged by the engine
};

// The snapshot of one engine script. `EngineScript` is the engine's Script (or a test double) with the members name,
// serial, file, state (int), mods (each with index and name), errors (any sequence of strings), memory and frameMs; a module
// is enabled when its index is valid in `registry` and that module is enabled.
template <class EngineScript>
ScriptInfo DescribeScript(const EngineScript& script, ModuleRegistry& registry) {
    ScriptInfo info;
    info.name = script.name;
    info.serial = script.serial;
    info.file = script.file;
    info.state = ScriptStateOf(script.state);
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

// The scripts and what can be done with them. List, Folder, ReadSource and WriteSource may be called from any thread
// (the engine takes its own lock and runs no Lua for them). Start and Stop run the script's Lua (its main chunk, its
// modules' enable and disable handlers) and change the menu's modules: call them on the render thread only, and
// never from inside a script callback.
// Scripts are named by ScriptInfo::name; a name that is not in List() makes Start, Stop and ReadSource return false.
class ScriptLibrary {
public:
    virtual ~ScriptLibrary() = default;

    // ---- reading

    // Every script of the scripts folder, sorted by name (ignoring case). Any thread.
    virtual std::vector<ScriptInfo> List() const = 0;

    // The scripts folder (%APPDATA%\Nickel\scripts in the game), or empty when it is not known. Any thread.
    virtual std::wstring Folder() const = 0;

    // The source of script `name` as it is on disk (UTF-8 Luau). False when the script or its file is missing.
    // Any thread.
    virtual bool ReadSource(const std::string& name, std::string& source) const = 0;

    // ---- commands

    // Loads script `name` again from its file and runs it (the Start and Reload buttons): a script stopped by the
    // user, stopped after errors or that failed to load runs again. Its modules keep their saved state and key.
    // Render thread only.
    virtual bool Start(const std::string& name) = 0;

    // Turns the modules of script `name` off and unloads it; it stays stopped until it is started or its file is saved.
    // Render thread only.
    virtual bool Stop(const std::string& name) = 0;

    // Creates or replaces the file of script `name` with `source` (UTF-8 Luau), in one step (the file never appears
    // half written; a leading UTF-8 byte order mark is dropped). Any thread. The engine loads it at its next folder
    // scan, within about a second, as when the user saves the file; List() then shows its state and errors. False
    // when ValidScriptName(name) is false or the file cannot be written.
    virtual bool WriteSource(const std::string& name, const std::string& source) = 0;
};

// The Start / Stop button of a row in the Scripting tab: stops a running script, starts any other.
inline bool ToggleScript(ScriptLibrary& library, const ScriptInfo& script) {
    return script.state == ScriptState::Running ? library.Stop(script.name) : library.Start(script.name);
}

} // namespace overlay
