#pragma once
// Nickel AI's tools in the client: the twelve tools the model may call, run against Nickel's real modules and scripts.
//
// The service declares the tools to the model and forwards each call; the client runs it here and posts the result
// (POST /turns/{turn_id}/tool-results). Tool names, inputs and results follow the protocol contract of
// docs/design/nickel-ai/tools.md as amended by the contract of the AI service (one shape on both sides).
//
// Safety rules kept here:
// - Every tool that changes something (set_setting, revert_changes, set_module_enabled, write_script,
//   set_script_running) needs the player's confirmation. Run refuses it with "confirmation_required" unless the caller
//   says the player confirmed. The confirmation card comes from Describe, which reads the registry and the script
//   library, never the model's text.
// - Scripts written by the assistant are held (ScriptLibrary::WriteSource): they never run before set_script_running.
// - Results are JSON text of at most 16 KB; lists and long texts are cut with "truncated": true.
// - Private text settings (a hidden nickname, a disconnect command, sign text, a fake server address) are sent as
//   "<hidden>".
//
// No Windows, no Lua, no menu, no network: the components come in through Deps (interfaces), so
// tests/ai_tools_checks.cpp runs every tool on fakes.
//
// Threads: the registry does not lock, so the tools that read or change modules (and Start / Stop of
// set_script_running) run on the render thread; NeedsRenderThread says which. list_scripts, read_script,
// validate_script and write_script only use ScriptLibrary calls that are safe on any thread.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "json_value.hpp"
#include "module_registry.hpp"
#include "script_library.hpp"

namespace nickel::ai {

constexpr size_t kMaxToolResultBytes = 16 * 1024;   // a tool result's JSON text
constexpr size_t kMaxScriptSourceBytes = 64 * 1024; // write_script / validate_script source
constexpr size_t kMaxSettingTextBytes = 256;        // a text setting's value
constexpr size_t kMaxDiffBytes = 32 * 1024;         // the diff on a write_script card
constexpr int kMaxSearchResults = 25;

// ---- what the tools need from the client

// The client's state for get_client_status.
struct ClientStatus {
    std::string nickelVersion;
    std::string gameVersion;
    bool inWorld = false;
    std::string server; // what the client chooses to say about the server; empty: left out of the result
};

class ClientStatusSource {
public:
    virtual ~ClientStatusSource() = default;
    virtual ClientStatus Status() const = 0;
};

// Opens a place of the Nickel menu (navigate_ui). Never closes the menu, never clicks.
class UiNavigator {
public:
    virtual ~UiNavigator() = default;
    // Selects module `index` (id `id`) in its category. False when it cannot be shown.
    virtual bool ShowModule(int index, const std::string& id) = 0;
    // Opens the tab called `tab` (compared without case: "home", "settings", "scripting", "combat"...). False when
    // there is no such tab.
    virtual bool ShowTab(const std::string& tab) = 0;
};

// Applies the assistant's changes to the modules, so the menu can apply them its own way (and save them).
class SettingsWriter {
public:
    virtual ~SettingsWriter() = default;
    // Stores value.number and value.text in setting `setting` of module `module`, and saves it.
    virtual void WriteSetting(int module, int setting, const overlay::Setting& value) = 0;
    // Turns module `module` on or off, and saves it.
    virtual void WriteEnabled(int module, bool on) = 0;
};

// A SettingsWriter that writes the registry's modules directly and saves through ModuleRegistry::SaveModule, as the
// Luau bindings do.
class RegistrySettingsWriter final : public SettingsWriter {
public:
    explicit RegistrySettingsWriter(overlay::ModuleRegistry& registry) : registry_(registry) {}

    void WriteSetting(int module, int setting, const overlay::Setting& value) override {
        overlay::Setting& target = registry_.At(module).settings.at(static_cast<size_t>(setting));
        target.number = value.number;
        target.text = value.text;
        registry_.SaveModule(module, &target);
    }

    void WriteEnabled(int module, bool on) override {
        overlay::Module& target = registry_.At(module);
        if (target.enabled != on) {
            target.enabled = on;
            registry_.SaveModule(module, nullptr);
        }
    }

private:
    overlay::ModuleRegistry& registry_;
};

struct Deps {
    overlay::ModuleRegistry& registry;
    overlay::ScriptLibrary& scripts;
    SettingsWriter& settings;
    UiNavigator& ui;
    const ClientStatusSource& status;
    // The catalogue as generated, before saved preferences were applied: the "default" of get_module. Optional.
    const std::vector<overlay::Module>* defaults = nullptr;
};

// ---- results and cards

struct ToolResult {
    bool isError = false;
    std::string content; // JSON text, at most kMaxToolResultBytes; {"error": code, "message": text} on error
    std::string error;   // the error code when isError
};

// One change shown on a confirmation card: what, the current value and the new one, as the player reads them.
struct ConfirmLine {
    std::string what;
    std::string before;
    std::string after;
};

// The confirmation card of an action tool, from the registry and the script library.
struct ConfirmCard {
    bool ok = false;       // false: the call cannot run; error and message say why (Run returns the same error)
    std::string error;
    std::string message;
    std::string tool;
    std::string title;
    std::vector<ConfirmLine> lines; // one per change
    // write_script
    std::string script;    // the script's name
    bool newScript = false;
    std::string diff;      // "-" removed, "+" added, " " context lines; a new script is all "+"
    bool diffTruncated = false;
    int added = 0;
    int removed = 0;
    std::vector<overlay::CompileError> compileErrors; // the new source's (it can still be saved)
    // set_script_running when starting: module("...") declarations found in the source (best effort)
    std::vector<std::string> declaredModules;
};

// One change the assistant made this session (set_setting or set_module_enabled), for revert_changes.
struct Change {
    std::string moduleId;
    std::string settingId; // empty: the module's on/off state
    overlay::Setting before;
    overlay::Setting after;
    bool enabledBefore = false;
    bool enabledAfter = false;
};

namespace detail {

inline std::string Lower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

inline bool IEquals(const std::string& a, const std::string& b) {
    return overlay::SameScriptName(a, b);
}

// `text` cut to at most `maxBytes` bytes without splitting a UTF-8 sequence.
inline std::string CutUtf8(const std::string& text, size_t maxBytes) {
    if (text.size() <= maxBytes) {
        return text;
    }
    size_t end = maxBytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        --end;
    }
    return text.substr(0, end);
}

inline size_t JsonSize(const json::Value& value) {
    return json::Serialize(value).size();
}

inline size_t JsonStringSize(const std::string& text) {
    std::string out;
    json::SerializeString(text, out);
    return out.size();
}

inline std::string FormatNumber(double value) {
    char text[32]{};
    std::snprintf(text, sizeof(text), "%.6g", value);
    return text;
}

// A virtual-key code as the menu names it; "None" for 0.
inline std::string KeyName(int vk) {
    if (vk == 0) {
        return "None";
    }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        return std::string(1, static_cast<char>(vk));
    }
    if (vk >= 0x70 && vk <= 0x87) {
        return "F" + std::to_string(vk - 0x70 + 1);
    }
    if (vk >= 0x60 && vk <= 0x69) {
        return "Numpad " + std::to_string(vk - 0x60);
    }
    switch (vk) {
        case 0x01: return "Left Mouse";
        case 0x02: return "Right Mouse";
        case 0x04: return "Middle Mouse";
        case 0x05: return "Mouse 4";
        case 0x06: return "Mouse 5";
        case 0x08: return "Backspace";
        case 0x09: return "Tab";
        case 0x0D: return "Enter";
        case 0x10: return "Shift";
        case 0x11: return "Ctrl";
        case 0x12: return "Alt";
        case 0x14: return "Caps Lock";
        case 0x20: return "Space";
        case 0x21: return "Page Up";
        case 0x22: return "Page Down";
        case 0x23: return "End";
        case 0x24: return "Home";
        case 0x25: return "Left";
        case 0x26: return "Up";
        case 0x27: return "Right";
        case 0x28: return "Down";
        case 0x2D: return "Insert";
        case 0x2E: return "Delete";
        case 0xA0: return "Left Shift";
        case 0xA1: return "Right Shift";
        case 0xA2: return "Left Ctrl";
        case 0xA3: return "Right Ctrl";
        case 0xA4: return "Left Alt";
        case 0xA5: return "Right Alt";
        default: return "Key " + std::to_string(vk);
    }
}

inline bool Visible(const overlay::Module& module) {
    return !module.category.empty();
}

inline bool ScriptModule(const overlay::Module& module) {
    return module.id.compare(0, 7, "script.") == 0;
}

// Text settings whose value is the player's private data: never sent to the model.
inline bool PrivateSetting(const std::string& moduleId, const overlay::Setting& setting) {
    return setting.kind == "text" && (moduleId == "nick_hider" || moduleId == "server_ip" ||
                                      moduleId == "auto_sign" || moduleId == "auto_disconnect");
}

inline bool Settable(const overlay::Setting& setting) {
    return setting.kind == "bool" || setting.kind == "number" || setting.kind == "enum" || setting.kind == "color" ||
           setting.kind == "text";
}

// The value of `setting` as JSON: bool, number, [low, high] for a range, the key's name, or the text; null for kinds
// the assistant does not read (list, gradient).
inline json::Value SettingValueJson(const std::string& moduleId, const overlay::Setting& setting) {
    if (setting.kind == "bool") {
        return json::Value::MakeBool(setting.number != 0);
    }
    if (setting.kind == "number") {
        return json::Value::MakeNumber(setting.number);
    }
    if (setting.kind == "range") {
        json::Value pair = json::Value::MakeArray();
        pair.Push(json::Value::MakeNumber(setting.number));
        pair.Push(json::Value::MakeNumber(std::strtod(setting.text.c_str(), nullptr)));
        return pair;
    }
    if (setting.kind == "key") {
        const size_t bar = setting.text.find('|');
        const std::string name = bar == std::string::npos ? setting.text : setting.text.substr(0, bar);
        return json::Value::MakeString(name.empty() ? "None" : name);
    }
    if (setting.kind == "enum" || setting.kind == "color" || setting.kind == "text") {
        return json::Value::MakeString(PrivateSetting(moduleId, setting) ? "<hidden>"
                                                                         : CutUtf8(setting.text, 512));
    }
    return json::Value();
}

// The value of `setting` as the player reads it on a card ("4.5 blocks", "On", "#ff0000ff").
inline std::string SettingValueText(const overlay::Setting& setting) {
    if (setting.kind == "bool") {
        return setting.number != 0 ? "On" : "Off";
    }
    if (setting.kind == "number") {
        return FormatNumber(setting.number) + setting.unit;
    }
    return setting.text.empty() ? "(empty)" : setting.text;
}

inline bool SameValue(const overlay::Setting& a, const overlay::Setting& b) {
    if (a.kind == "bool" || a.kind == "number") {
        return a.number == b.number;
    }
    return a.text == b.text;
}

// Index of the visible setting of `module` whose id is `id` exactly, or -1.
inline int FindSettingById(const overlay::Module& module, const std::string& id) {
    for (size_t i = 0; i < module.settings.size(); ++i) {
        if (module.settings[i].kind != "hidden" && module.settings[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Index of the visible setting called `name` (id first, then display name ignoring case), or -1.
inline int FindSetting(const overlay::Module& module, const std::string& name) {
    const int byId = FindSettingById(module, name);
    if (byId >= 0) {
        return byId;
    }
    for (size_t i = 0; i < module.settings.size(); ++i) {
        if (module.settings[i].kind != "hidden" && IEquals(module.settings[i].name, name)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

inline int HexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// "#RRGGBB" or "#RRGGBBAA" (any case) as the menu stores colours, "#rrggbbaa"; empty when it is not a colour.
inline std::string NormalizeColor(const std::string& text) {
    if ((text.size() != 7 && text.size() != 9) || text[0] != '#') {
        return {};
    }
    for (size_t i = 1; i < text.size(); ++i) {
        if (HexDigit(text[i]) < 0) {
            return {};
        }
    }
    std::string color = Lower(text);
    if (color.size() == 7) {
        color += "ff";
    }
    return color;
}

inline std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    if (text.empty()) {
        return lines;
    }
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
        if (end == text.size()) {
            break;
        }
        start = end + 1;
    }
    if (!lines.empty() && lines.back().empty() && !text.empty() && text.back() == '\n') {
        lines.pop_back(); // a final newline does not start a line
    }
    return lines;
}

struct LineDiff {
    std::string text;
    int added = 0;
    int removed = 0;
    bool truncated = false;
};

// A line diff of `before` -> `after`: the common start and end are kept as up to three context lines; the middle is
// diffed by longest common subsequence when it is small enough, otherwise shown as a removed then an added block.
inline LineDiff DiffLines(const std::string& before, const std::string& after, size_t maxBytes = kMaxDiffBytes) {
    const std::vector<std::string> a = SplitLines(before);
    const std::vector<std::string> b = SplitLines(after);
    LineDiff diff;
    auto emit = [&](char mark, const std::string& line) {
        if (mark == '+') {
            ++diff.added;
        } else if (mark == '-') {
            ++diff.removed;
        }
        if (diff.truncated) {
            return;
        }
        if (diff.text.size() + line.size() + 2 > maxBytes) {
            diff.truncated = true;
            return;
        }
        diff.text += mark;
        diff.text += line;
        diff.text += '\n';
    };
    size_t prefix = 0;
    while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix]) {
        ++prefix;
    }
    size_t suffix = 0;
    while (suffix < a.size() - prefix && suffix < b.size() - prefix &&
           a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix]) {
        ++suffix;
    }
    const size_t na = a.size() - prefix - suffix;
    const size_t nb = b.size() - prefix - suffix;
    if (na == 0 && nb == 0) {
        return diff; // same lines
    }
    const size_t contextStart = prefix > 3 ? prefix - 3 : 0;
    if (contextStart > 0 || !a.empty()) {
        if (!diff.truncated) {
            diff.text += "@@ line " + std::to_string(contextStart + 1) + " @@\n";
        }
    }
    for (size_t i = contextStart; i < prefix; ++i) {
        emit(' ', a[i]);
    }
    if (na > 0 && nb > 0 && na * nb <= 1000000) {
        // lcs[i][j]: longest common subsequence of a[prefix+i..] and b[prefix+j..] within the middle
        std::vector<int> lcs((na + 1) * (nb + 1), 0);
        auto at = [&](size_t i, size_t j) -> int& { return lcs[i * (nb + 1) + j]; };
        for (size_t i = na; i-- > 0;) {
            for (size_t j = nb; j-- > 0;) {
                at(i, j) = a[prefix + i] == b[prefix + j] ? at(i + 1, j + 1) + 1 : std::max(at(i + 1, j), at(i, j + 1));
            }
        }
        size_t i = 0, j = 0;
        while (i < na || j < nb) {
            if (i < na && j < nb && a[prefix + i] == b[prefix + j]) {
                emit(' ', a[prefix + i]);
                ++i;
                ++j;
            } else if (j >= nb || (i < na && at(i + 1, j) >= at(i, j + 1))) {
                emit('-', a[prefix + i]);
                ++i;
            } else {
                emit('+', b[prefix + j]);
                ++j;
            }
        }
    } else {
        for (size_t i = 0; i < na; ++i) {
            emit('-', a[prefix + i]);
        }
        for (size_t j = 0; j < nb; ++j) {
            emit('+', b[prefix + j]);
        }
    }
    for (size_t k = 0; k < suffix && k < 3; ++k) {
        emit(' ', a[a.size() - suffix + k]);
    }
    return diff;
}

// The names in module("Name", ...) calls of a Luau source (best effort: the first argument when it is a plain string
// literal), without repeats, at most 20.
inline std::vector<std::string> DeclaredModules(const std::string& source) {
    std::vector<std::string> names;
    size_t at = 0;
    while (names.size() < 20 && (at = source.find("module", at)) != std::string::npos) {
        const size_t start = at;
        at += 6;
        if (start > 0) {
            const unsigned char before = static_cast<unsigned char>(source[start - 1]);
            if (std::isalnum(before) || before == '_' || before == '.' || before == ':') {
                continue;
            }
        }
        size_t p = at;
        while (p < source.size() && (source[p] == ' ' || source[p] == '\t')) {
            ++p;
        }
        if (p >= source.size() || source[p] != '(') {
            continue;
        }
        ++p;
        while (p < source.size() && (source[p] == ' ' || source[p] == '\t')) {
            ++p;
        }
        if (p >= source.size() || (source[p] != '"' && source[p] != '\'')) {
            continue;
        }
        const char quote = source[p];
        const size_t end = source.find(quote, p + 1);
        if (end == std::string::npos || end - p - 1 > 64 || source.find('\n', p) < end) {
            continue;
        }
        const std::string name = source.substr(p + 1, end - p - 1);
        bool repeated = false;
        for (const std::string& known : names) {
            repeated = repeated || known == name;
        }
        if (!name.empty() && !repeated) {
            names.push_back(name);
        }
        at = end;
    }
    return names;
}

// Sets object[key] to as many of `items` as fit in `limit` bytes of JSON text with the rest of `object`; adds
// "truncated": true when some were left out.
inline void SetBounded(json::Value& object, const std::string& key, const std::vector<json::Value>& items,
                       size_t limit = kMaxToolResultBytes) {
    size_t used = JsonSize(object) + JsonStringSize(key) + 4 + 20; // ,"key":[] and room for "truncated":true
    json::Value array = json::Value::MakeArray();
    bool cut = false;
    for (const json::Value& item : items) {
        const size_t size = JsonSize(item) + 1;
        if (used + size > limit) {
            cut = true;
            break;
        }
        used += size;
        array.Push(item);
    }
    object.Set(key, std::move(array));
    if (cut) {
        object.Set("truncated", json::Value::MakeBool(true));
    }
}

// Sets object[key] to `text`, cut so that the result stays within `limit` bytes; adds "truncated": true when cut.
inline void SetBoundedText(json::Value& object, const std::string& key, const std::string& text,
                           size_t limit = kMaxToolResultBytes) {
    const size_t used = JsonSize(object) + JsonStringSize(key) + 2 + 20;
    const size_t budget = limit > used + 2 ? limit - used : 2;
    std::string value = text;
    bool cut = false;
    size_t size = JsonStringSize(value);
    while (size > budget && !value.empty()) {
        // shrink in proportion to the escaped size, a little more each round
        const size_t target = static_cast<size_t>(static_cast<double>(value.size()) * budget / size * 0.98);
        value = CutUtf8(value, target < value.size() ? target : value.size() - 1);
        cut = true;
        size = JsonStringSize(value);
    }
    object.Set(key, json::Value::MakeString(value));
    if (cut) {
        object.Set("truncated", json::Value::MakeBool(true));
    }
}

// Reads the members of a tool's input. Unknown members are ignored; a member of the wrong type, a missing required
// member or a string over its size makes ok() false with a message (the "invalid_input" error).
class InputReader {
public:
    explicit InputReader(const json::Value& input) : input_(input) {}

    bool ok() const { return message_.empty(); }
    const std::string& message() const { return message_; }

    // A string member. Optional members that are missing or null leave `out` unchanged and set *present to false.
    bool String(const char* key, std::string* out, bool required, size_t maxBytes, bool* present = nullptr) {
        const json::Value* value = Member(key, required);
        if (present != nullptr) {
            *present = value != nullptr;
        }
        if (value == nullptr) {
            return ok();
        }
        if (!value->IsString()) {
            return Fail(std::string("'") + key + "' must be a string");
        }
        if (value->AsString().size() > maxBytes) {
            return Fail(std::string("'") + key + "' is longer than " + std::to_string(maxBytes) + " bytes");
        }
        if (!overlay::WellFormedUtf8(value->AsString())) {
            return Fail(std::string("'") + key + "' is not valid UTF-8");
        }
        *out = value->AsString();
        return true;
    }

    bool Bool(const char* key, bool* out, bool required) {
        const json::Value* value = Member(key, required);
        if (value == nullptr) {
            return ok();
        }
        if (!value->IsBool()) {
            return Fail(std::string("'") + key + "' must be true or false");
        }
        *out = value->AsBool();
        return true;
    }

    // Any value (not null) for a required member.
    const json::Value* Any(const char* key) { return Member(key, true); }

private:
    const json::Value* Member(const char* key, bool required) {
        const json::Value* value = input_.Find(key);
        if (value == nullptr || value->IsNull()) {
            if (required) {
                Fail(std::string("'") + key + "' is required");
            }
            return nullptr;
        }
        return value;
    }

    bool Fail(const std::string& message) {
        if (message_.empty()) {
            message_ = message;
        }
        return false;
    }

    const json::Value& input_;
    std::string message_;
};

} // namespace detail

class AssistantTools {
public:
    explicit AssistantTools(Deps deps) : deps_(deps) {}

    // The twelve tool names, read tools first.
    static const std::vector<std::string>& ToolNames() {
        static const std::vector<std::string> kNames = {
                "search_modules", "get_module",     "get_client_status",  "list_scripts",
                "read_script",    "validate_script", "navigate_ui",       "set_setting",
                "revert_changes", "set_module_enabled", "write_script",   "set_script_running"};
        return kNames;
    }

    static bool IsTool(const std::string& name) {
        const auto& names = ToolNames();
        return std::find(names.begin(), names.end(), name) != names.end();
    }

    // True for the tools that change something: Run needs `confirmed` for them, after the player accepted the card
    // from Describe. `input` is not used yet: every call of these tools is confirmed, whatever it changes.
    static bool NeedsConfirmation(const std::string& name, const json::Value& input) {
        (void)input;
        return name == "set_setting" || name == "revert_changes" || name == "set_module_enabled" ||
               name == "write_script" || name == "set_script_running";
    }

    // True for the tools that read or change the module registry or run Lua: call Run and Describe for them on the
    // render thread. The others use only ScriptLibrary calls that are safe on any thread.
    static bool NeedsRenderThread(const std::string& name) {
        return name != "list_scripts" && name != "read_script" && name != "validate_script" && name != "write_script";
    }

    // The tool result for a call the player refused (or did not answer).
    static ToolResult Declined(const std::string& message = "The player declined this action") {
        return Error("declined", message);
    }

    // Runs tool `name` with `input` (a JSON object; null counts as {}). A tool for which NeedsConfirmation is true
    // returns the error "confirmation_required" unless `confirmed`.
    ToolResult Run(const std::string& name, const json::Value& input, bool confirmed = false) {
        if (!IsTool(name)) {
            return Error("unknown_tool", "There is no tool called '" + detail::CutUtf8(name, 64) + "'");
        }
        const json::Value empty = json::Value::MakeObject();
        const json::Value& in = input.IsNull() ? empty : input;
        if (!in.IsObject()) {
            return Error("invalid_input", "The input must be a JSON object");
        }
        if (NeedsConfirmation(name, in) && !confirmed) {
            return Error("confirmation_required", "This action needs the player's confirmation");
        }
        if (name == "search_modules") return SearchModules(in);
        if (name == "get_module") return GetModule(in);
        if (name == "get_client_status") return GetClientStatus();
        if (name == "list_scripts") return ListScripts();
        if (name == "read_script") return ReadScript(in);
        if (name == "validate_script") return ValidateScript(in);
        if (name == "navigate_ui") return NavigateUi(in);
        if (name == "set_setting") return SetSetting(in);
        if (name == "revert_changes") return RevertChanges();
        if (name == "set_module_enabled") return SetModuleEnabled(in);
        if (name == "write_script") return WriteScript(in);
        return SetScriptRunning(in);
    }

    // The confirmation card for an action tool, from real data: module and setting names, current and new values,
    // the script's diff and compile errors. A card with ok == false names the error Run would return.
    ConfirmCard Describe(const std::string& name, const json::Value& input) {
        ConfirmCard card;
        card.tool = name;
        const json::Value empty = json::Value::MakeObject();
        const json::Value& in = input.IsNull() ? empty : input;
        if (!IsTool(name)) {
            return Refuse(card, "unknown_tool", "There is no tool called '" + detail::CutUtf8(name, 64) + "'");
        }
        if (!NeedsConfirmation(name, in)) {
            return Refuse(card, "not_an_action", "This tool changes nothing and needs no confirmation");
        }
        if (!in.IsObject()) {
            return Refuse(card, "invalid_input", "The input must be a JSON object");
        }
        Failure failure;
        if (name == "set_setting") {
            SettingPlan plan;
            if (!PlanSetting(in, plan, failure)) {
                return Refuse(card, failure.code, failure.message);
            }
            const overlay::Module& module = deps_.registry.At(plan.module);
            card.title = "Change a setting of " + module.name;
            card.lines.push_back({module.name + ": " + plan.before.name, detail::SettingValueText(plan.before),
                                  detail::SettingValueText(plan.after)});
        } else if (name == "set_module_enabled") {
            EnabledPlan plan;
            if (!PlanEnabled(in, plan, failure)) {
                return Refuse(card, failure.code, failure.message);
            }
            const overlay::Module& module = deps_.registry.At(plan.module);
            card.title = std::string(plan.on ? "Turn on " : "Turn off ") + module.name;
            card.lines.push_back({module.name, plan.before ? "On" : "Off", plan.on ? "On" : "Off"});
        } else if (name == "revert_changes") {
            const RevertPlan plan = PlanRevert();
            card.title = plan.steps.empty() ? "Undo the assistant's changes (nothing to undo)"
                                            : "Undo the assistant's changes";
            for (const RevertStep& step : plan.steps) {
                const Change& change = journal_[step.change];
                const overlay::Module& module = deps_.registry.At(step.module);
                if (change.settingId.empty()) {
                    card.lines.push_back({module.name, change.enabledAfter ? "On" : "Off",
                                          change.enabledBefore ? "On" : "Off"});
                } else {
                    card.lines.push_back({module.name + ": " + change.after.name,
                                          detail::SettingValueText(change.after),
                                          detail::SettingValueText(change.before)});
                }
            }
        } else if (name == "write_script") {
            WritePlan plan;
            if (!PlanWrite(in, plan, failure)) {
                return Refuse(card, failure.code, failure.message);
            }
            card.script = plan.name;
            card.newScript = !plan.exists;
            card.title = (plan.exists ? "Save a new version of the script " : "Save the new script ") + plan.name;
            const detail::LineDiff diff = detail::DiffLines(plan.exists ? plan.old.text : std::string(), plan.source);
            card.diff = diff.text;
            card.diffTruncated = diff.truncated;
            card.added = diff.added;
            card.removed = diff.removed;
            card.compileErrors = deps_.scripts.Compile(plan.source);
            if (card.compileErrors.size() > 10) {
                card.compileErrors.resize(10);
            }
            const size_t newLines = detail::SplitLines(plan.source).size();
            card.lines.push_back({plan.name,
                                  plan.exists ? std::to_string(detail::SplitLines(plan.old.text).size()) + " lines"
                                              : "(new script)",
                                  std::to_string(newLines) + " lines, not started"});
        } else {
            RunPlan plan;
            if (!PlanRun(in, plan, failure)) {
                return Refuse(card, failure.code, failure.message);
            }
            card.script = plan.info.name;
            card.title = (plan.running ? "Start the script " : "Stop the script ") + plan.info.name;
            card.lines.push_back({plan.info.name, overlay::ScriptStatusText(plan.info),
                                  plan.running ? "Running" : "Stopped"});
            if (plan.running) {
                overlay::ScriptSource source;
                if (deps_.scripts.ReadSource(plan.info.name, source)) {
                    card.declaredModules = detail::DeclaredModules(source.text);
                }
            }
        }
        card.ok = true;
        return card;
    }

    // The changes this session made with set_setting and set_module_enabled, oldest first.
    const std::vector<Change>& Journal() const { return journal_; }

private:
    struct Failure {
        std::string code;
        std::string message;
    };

    struct SettingPlan {
        int module = -1;
        int setting = -1;
        overlay::Setting before;
        overlay::Setting after;
    };

    struct EnabledPlan {
        int module = -1;
        bool before = false;
        bool on = false;
    };

    struct RevertStep {
        size_t change = 0; // index in journal_
        int module = -1;
        int setting = -1;  // -1: the on/off state
    };

    struct RevertPlan {
        std::vector<RevertStep> steps; // newest change first
        int skipped = 0;
    };

    struct WritePlan {
        std::string name;     // the listed name when the script exists
        std::string source;
        std::string expected; // sha256 or empty
        bool exists = false;
        bool wasRunning = false;
        overlay::ScriptSource old;
    };

    struct RunPlan {
        overlay::ScriptInfo info;
        bool running = false;
    };

    static ToolResult Error(const std::string& code, const std::string& message) {
        json::Value body = json::Value::MakeObject();
        body.Set("error", json::Value::MakeString(code));
        body.Set("message", json::Value::MakeString(detail::CutUtf8(message, 400)));
        ToolResult result;
        result.isError = true;
        result.error = code;
        result.content = json::Serialize(body);
        return result;
    }

    static ToolResult Done(const json::Value& body) {
        ToolResult result;
        result.content = json::Serialize(body);
        if (result.content.size() > kMaxToolResultBytes) {
            return Error("too_large", "The result is larger than 16 KB");
        }
        return result;
    }

    static ConfirmCard& Refuse(ConfirmCard& card, const std::string& code, const std::string& message) {
        card.ok = false;
        card.error = code;
        card.message = message;
        return card;
    }

    static bool Fail(Failure& failure, const std::string& code, const std::string& message) {
        failure.code = code;
        failure.message = message;
        return false;
    }

    // Index of the visible module `id`, or -1.
    int VisibleModule(const std::string& id) const {
        const int index = deps_.registry.Find(id);
        return index >= 0 && detail::Visible(deps_.registry.At(index)) ? index : -1;
    }

    const overlay::Setting* DefaultOf(const std::string& moduleId, const std::string& settingId) const {
        if (deps_.defaults == nullptr) {
            return nullptr;
        }
        for (const overlay::Module& module : *deps_.defaults) {
            if (module.id == moduleId) {
                for (const overlay::Setting& setting : module.settings) {
                    if (setting.id == settingId) {
                        return &setting;
                    }
                }
                return nullptr;
            }
        }
        return nullptr;
    }

    // ---- read tools

    ToolResult SearchModules(const json::Value& in) {
        detail::InputReader reader(in);
        std::string query, category;
        reader.String("query", &query, true, 200);
        reader.String("category", &category, false, 16);
        if (!reader.ok()) {
            return Error("invalid_input", reader.message());
        }
        static const char* const kCategories[] = {"combat", "movement", "player", "visuals", "utility", "hud"};
        if (!category.empty() &&
            std::find_if(std::begin(kCategories), std::end(kCategories),
                         [&](const char* known) { return category == known; }) == std::end(kCategories)) {
            return Error("invalid_input",
                         "'category' must be one of combat, movement, player, visuals, utility, hud");
        }
        std::vector<std::string> words;
        std::string word;
        for (char c : detail::Lower(query) + " ") {
            if (std::isalnum(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) >= 0x80) {
                word += c;
            } else if (!word.empty()) {
                words.push_back(word);
                word.clear();
            }
        }
        struct Hit {
            int index;
            int score;
        };
        std::vector<Hit> hits;
        for (int i = 0; i < deps_.registry.Count(); ++i) {
            const overlay::Module& module = deps_.registry.At(i);
            if (!detail::Visible(module) || (!category.empty() && detail::Lower(module.category) != category)) {
                continue;
            }
            int score = 0;
            const std::string name = detail::Lower(module.name), id = detail::Lower(module.id),
                              description = detail::Lower(module.description);
            for (const std::string& w : words) {
                score += (name.find(w) != std::string::npos ? 3 : 0) + (id.find(w) != std::string::npos ? 2 : 0) +
                         (description.find(w) != std::string::npos ? 1 : 0);
            }
            if (words.empty() || score > 0) {
                hits.push_back({i, score});
            }
        }
        std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
        json::Value body = json::Value::MakeObject();
        body.Set("total", json::Value::MakeInt(static_cast<int64_t>(hits.size())));
        std::vector<json::Value> items;
        for (size_t k = 0; k < hits.size() && k < static_cast<size_t>(kMaxSearchResults); ++k) {
            const overlay::Module& module = deps_.registry.At(hits[k].index);
            json::Value item = json::Value::MakeObject();
            item.Set("id", json::Value::MakeString(module.id));
            item.Set("name", json::Value::MakeString(detail::CutUtf8(module.name, 96)));
            item.Set("category", json::Value::MakeString(detail::Lower(module.category)));
            item.Set("enabled", json::Value::MakeBool(module.enabled));
            item.Set("description", json::Value::MakeString(detail::CutUtf8(module.description, 300)));
            if (detail::ScriptModule(module)) {
                item.Set("source", json::Value::MakeString("script"));
            }
            items.push_back(std::move(item));
        }
        detail::SetBounded(body, "modules", items);
        if (hits.size() > static_cast<size_t>(kMaxSearchResults)) {
            body.Set("truncated", json::Value::MakeBool(true));
        }
        return Done(body);
    }

    ToolResult GetModule(const json::Value& in) {
        detail::InputReader reader(in);
        std::string id;
        reader.String("id", &id, true, 128);
        if (!reader.ok()) {
            return Error("invalid_input", reader.message());
        }
        const int index = VisibleModule(id);
        if (index < 0) {
            return Error("not_found", "No module has the id '" + id + "'; use search_modules to find it");
        }
        const overlay::Module& module = deps_.registry.At(index);
        json::Value body = json::Value::MakeObject();
        body.Set("id", json::Value::MakeString(module.id));
        body.Set("name", json::Value::MakeString(detail::CutUtf8(module.name, 96)));
        body.Set("category", json::Value::MakeString(detail::Lower(module.category)));
        body.Set("description", json::Value::MakeString(detail::CutUtf8(module.description, 300)));
        body.Set("enabled", json::Value::MakeBool(module.enabled));
        body.Set("key", json::Value::MakeString(detail::KeyName(deps_.registry.KeyOf(module.id))));
        if (detail::ScriptModule(module)) {
            body.Set("source", json::Value::MakeString("script"));
        }
        std::vector<json::Value> settings;
        for (const overlay::Setting& setting : module.settings) {
            if (setting.kind == "hidden") {
                continue;
            }
            json::Value item = json::Value::MakeObject();
            item.Set("id", json::Value::MakeString(setting.id));
            item.Set("name", json::Value::MakeString(detail::CutUtf8(setting.name, 96)));
            item.Set("type", json::Value::MakeString(setting.kind));
            item.Set("value", detail::SettingValueJson(module.id, setting));
            const overlay::Setting* fallback = DefaultOf(module.id, setting.id);
            item.Set("default", fallback != nullptr && fallback->kind == setting.kind
                                        ? detail::SettingValueJson(module.id, *fallback)
                                        : json::Value());
            if (setting.kind == "number" || setting.kind == "range") {
                item.Set("min", json::Value::MakeNumber(setting.minimum));
                item.Set("max", json::Value::MakeNumber(setting.maximum));
                if (!setting.unit.empty()) {
                    item.Set("unit", json::Value::MakeString(setting.unit));
                }
            }
            if (setting.kind == "enum") {
                json::Value choices = json::Value::MakeArray();
                for (const std::string& option : setting.options) {
                    choices.Push(json::Value::MakeString(option));
                }
                item.Set("choices", std::move(choices));
            }
            item.Set("settable", json::Value::MakeBool(detail::Settable(setting)));
            settings.push_back(std::move(item));
        }
        detail::SetBounded(body, "settings", settings);
        return Done(body);
    }

    ToolResult GetClientStatus() {
        const ClientStatus status = deps_.status.Status();
        json::Value body = json::Value::MakeObject();
        body.Set("nickel_version", json::Value::MakeString(detail::CutUtf8(status.nickelVersion, 64)));
        body.Set("game_version", json::Value::MakeString(detail::CutUtf8(status.gameVersion, 64)));
        body.Set("in_world", json::Value::MakeBool(status.inWorld));
        if (!status.server.empty()) {
            body.Set("server", json::Value::MakeString(detail::CutUtf8(status.server, 128)));
        }
        std::vector<json::Value> enabled;
        for (int i = 0; i < deps_.registry.Count(); ++i) {
            const overlay::Module& module = deps_.registry.At(i);
            if (detail::Visible(module) && module.enabled) {
                enabled.push_back(json::Value::MakeString(module.id));
            }
        }
        detail::SetBounded(body, "enabled_modules", enabled);
        return Done(body);
    }

    ToolResult ListScripts() {
        const std::vector<overlay::ScriptInfo> scripts = deps_.scripts.List();
        std::vector<json::Value> items;
        for (const overlay::ScriptInfo& script : scripts) {
            json::Value item = json::Value::MakeObject();
            item.Set("name", json::Value::MakeString(script.name));
            item.Set("state", json::Value::MakeString(overlay::ScriptStateName(script.state)));
            item.Set("held", json::Value::MakeBool(script.held));
            json::Value modules = json::Value::MakeArray();
            for (const overlay::ScriptModuleInfo& module : script.modules) {
                modules.Push(json::Value::MakeString(detail::CutUtf8(module.name, 96)));
            }
            item.Set("modules", std::move(modules));
            json::Value errors = json::Value::MakeArray();
            for (size_t k = 0; k < script.errors.size() && k < 3; ++k) {
                errors.Push(json::Value::MakeString(detail::CutUtf8(script.errors[script.errors.size() - 1 - k], 300)));
            }
            item.Set("errors", std::move(errors));
            items.push_back(std::move(item));
        }
        json::Value body = json::Value::MakeObject();
        detail::SetBounded(body, "scripts", items);
        return Done(body);
    }

    ToolResult ReadScript(const json::Value& in) {
        detail::InputReader reader(in);
        std::string name;
        reader.String("name", &name, true, 255);
        if (!reader.ok()) {
            return Error("invalid_input", reader.message());
        }
        const std::vector<overlay::ScriptInfo> scripts = deps_.scripts.List();
        const int index = overlay::FindScript(scripts, name);
        overlay::ScriptSource source;
        if (index < 0 || !deps_.scripts.ReadSource(scripts[index].name, source)) {
            return Error("not_found", "No script is called '" + name + "'; use list_scripts");
        }
        json::Value body = json::Value::MakeObject();
        body.Set("name", json::Value::MakeString(scripts[index].name));
        body.Set("sha256", json::Value::MakeString(source.sha256));
        detail::SetBoundedText(body, "source", source.text);
        return Done(body);
    }

    ToolResult ValidateScript(const json::Value& in) {
        detail::InputReader reader(in);
        std::string source;
        reader.String("source", &source, true, std::string::npos);
        if (!reader.ok()) {
            return Error("invalid_input", reader.message());
        }
        if (source.size() > kMaxScriptSourceBytes) {
            return Error("too_large", "A script source is at most 64 KB");
        }
        const std::vector<overlay::CompileError> errors = deps_.scripts.Compile(source);
        json::Value body = json::Value::MakeObject();
        body.Set("ok", json::Value::MakeBool(errors.empty()));
        std::vector<json::Value> items;
        for (const overlay::CompileError& error : errors) {
            json::Value item = json::Value::MakeObject();
            item.Set("line", json::Value::MakeInt(error.line));
            item.Set("message", json::Value::MakeString(detail::CutUtf8(error.message, 400)));
            items.push_back(std::move(item));
        }
        detail::SetBounded(body, "errors", items);
        return Done(body);
    }

    ToolResult NavigateUi(const json::Value& in) {
        detail::InputReader reader(in);
        std::string target;
        reader.String("target", &target, true, 128);
        if (!reader.ok()) {
            return Error("invalid_input", reader.message());
        }
        if (target.empty()) {
            return Error("invalid_input", "'target' is empty");
        }
        const int index = VisibleModule(target);
        const bool shown = index >= 0 ? deps_.ui.ShowModule(index, deps_.registry.At(index).id)
                                      : deps_.ui.ShowTab(target);
        if (!shown) {
            return Error("not_found", "No tab or module is called '" + target + "'");
        }
        json::Value body = json::Value::MakeObject();
        body.Set("ok", json::Value::MakeBool(true));
        return Done(body);
    }

    // ---- action tools

    bool PlanSetting(const json::Value& in, SettingPlan& plan, Failure& failure) {
        detail::InputReader reader(in);
        std::string moduleId, settingName;
        reader.String("module_id", &moduleId, true, 128);
        reader.String("setting", &settingName, true, 128);
        const json::Value* value = reader.Any("value");
        if (!reader.ok()) {
            return Fail(failure, "invalid_input", reader.message());
        }
        plan.module = VisibleModule(moduleId);
        if (plan.module < 0) {
            return Fail(failure, "not_found", "No module has the id '" + moduleId + "'; use search_modules");
        }
        const overlay::Module& module = deps_.registry.At(plan.module);
        plan.setting = detail::FindSetting(module, settingName);
        if (plan.setting < 0) {
            return Fail(failure, "not_found",
                        "Module '" + moduleId + "' has no setting '" + settingName + "'; use get_module");
        }
        plan.before = module.settings[static_cast<size_t>(plan.setting)];
        plan.after = plan.before;
        const overlay::Setting& setting = plan.before;
        if (setting.kind == "bool") {
            if (!value->IsBool()) {
                return Fail(failure, "invalid_value", setting.name + " takes true or false");
            }
            plan.after.number = value->AsBool() ? 1 : 0;
        } else if (setting.kind == "number") {
            if (!value->IsNumber() || !std::isfinite(value->AsDouble())) {
                return Fail(failure, "invalid_value", setting.name + " takes a number");
            }
            double number = value->AsDouble();
            if (setting.step >= 1) {
                number = std::round(number);
            }
            plan.after.number = std::clamp(number, setting.minimum, setting.maximum);
        } else if (setting.kind == "enum") {
            std::string choices;
            for (const std::string& option : setting.options) {
                choices += (choices.empty() ? "" : ", ") + option;
                if (value->IsString() && detail::IEquals(option, value->AsString())) {
                    plan.after.text = option;
                    return true;
                }
            }
            return Fail(failure, "invalid_value", setting.name + " takes one of: " + choices);
        } else if (setting.kind == "color") {
            const std::string color = value->IsString() ? detail::NormalizeColor(value->AsString()) : std::string();
            if (color.empty()) {
                return Fail(failure, "invalid_value", setting.name + " takes a colour \"#RRGGBB\" or \"#RRGGBBAA\"");
            }
            plan.after.text = color;
        } else if (setting.kind == "text") {
            if (!value->IsString() || !overlay::WellFormedUtf8(value->AsString())) {
                return Fail(failure, "invalid_value", setting.name + " takes text");
            }
            if (value->AsString().size() > kMaxSettingTextBytes) {
                return Fail(failure, "invalid_value", setting.name + " takes at most 256 bytes of text");
            }
            plan.after.text = value->AsString();
        } else {
            return Fail(failure, "read_only_setting",
                        setting.name + " (" + setting.kind + ") cannot be changed by the assistant; navigate_ui can "
                        "show it to the player");
        }
        return true;
    }

    ToolResult SetSetting(const json::Value& in) {
        SettingPlan plan;
        Failure failure;
        if (!PlanSetting(in, plan, failure)) {
            return Error(failure.code, failure.message);
        }
        const std::string moduleId = deps_.registry.At(plan.module).id;
        if (!detail::SameValue(plan.before, plan.after)) {
            deps_.settings.WriteSetting(plan.module, plan.setting, plan.after);
            Change change;
            change.moduleId = moduleId;
            change.settingId = plan.before.id;
            change.before = plan.before;
            change.after = plan.after;
            journal_.push_back(std::move(change));
        }
        json::Value body = json::Value::MakeObject();
        body.Set("ok", json::Value::MakeBool(true));
        body.Set("previous", detail::SettingValueJson(moduleId, plan.before));
        body.Set("value", detail::SettingValueJson(moduleId, plan.after));
        return Done(body);
    }

    bool PlanEnabled(const json::Value& in, EnabledPlan& plan, Failure& failure) {
        detail::InputReader reader(in);
        std::string moduleId;
        reader.String("module_id", &moduleId, true, 128);
        reader.Bool("enabled", &plan.on, true);
        if (!reader.ok()) {
            return Fail(failure, "invalid_input", reader.message());
        }
        plan.module = VisibleModule(moduleId);
        if (plan.module < 0) {
            return Fail(failure, "not_found", "No module has the id '" + moduleId + "'; use search_modules");
        }
        plan.before = deps_.registry.At(plan.module).enabled;
        return true;
    }

    ToolResult SetModuleEnabled(const json::Value& in) {
        EnabledPlan plan;
        Failure failure;
        if (!PlanEnabled(in, plan, failure)) {
            return Error(failure.code, failure.message);
        }
        if (plan.before != plan.on) {
            deps_.settings.WriteEnabled(plan.module, plan.on);
            Change change;
            change.moduleId = deps_.registry.At(plan.module).id;
            change.enabledBefore = plan.before;
            change.enabledAfter = plan.on;
            journal_.push_back(std::move(change));
        }
        json::Value body = json::Value::MakeObject();
        body.Set("ok", json::Value::MakeBool(true));
        body.Set("previous", json::Value::MakeBool(plan.before));
        return Done(body);
    }

    // The journal's changes that can be undone, newest first, each checked against the state the newer undos leave: a
    // change is skipped when its module, setting or value is no longer the one this session set.
    RevertPlan PlanRevert() {
        RevertPlan plan;
        std::map<std::string, overlay::Setting> settings; // values after the newer undos, by "module\nsetting"
        std::map<std::string, bool> enabled;
        for (size_t k = journal_.size(); k-- > 0;) {
            const Change& change = journal_[k];
            const int index = deps_.registry.Find(change.moduleId);
            if (index < 0) {
                ++plan.skipped;
                continue;
            }
            const overlay::Module& module = deps_.registry.At(index);
            if (change.settingId.empty()) {
                const auto known = enabled.find(change.moduleId);
                const bool now = known != enabled.end() ? known->second : module.enabled;
                if (now != change.enabledAfter) {
                    ++plan.skipped;
                    continue;
                }
                enabled[change.moduleId] = change.enabledBefore;
                plan.steps.push_back({k, index, -1});
                continue;
            }
            const int setting = detail::FindSettingById(module, change.settingId);
            if (setting < 0 || module.settings[static_cast<size_t>(setting)].kind != change.after.kind) {
                ++plan.skipped;
                continue;
            }
            const std::string key = change.moduleId + "\n" + change.settingId;
            const auto known = settings.find(key);
            const overlay::Setting& now =
                    known != settings.end() ? known->second : module.settings[static_cast<size_t>(setting)];
            if (!detail::SameValue(now, change.after)) {
                ++plan.skipped;
                continue;
            }
            settings[key] = change.before;
            plan.steps.push_back({k, index, setting});
        }
        return plan;
    }

    ToolResult RevertChanges() {
        const RevertPlan plan = PlanRevert();
        for (const RevertStep& step : plan.steps) {
            const Change& change = journal_[step.change];
            if (step.setting < 0) {
                deps_.settings.WriteEnabled(step.module, change.enabledBefore);
            } else {
                deps_.settings.WriteSetting(step.module, step.setting, change.before);
            }
        }
        journal_.clear();
        json::Value body = json::Value::MakeObject();
        body.Set("ok", json::Value::MakeBool(true));
        body.Set("reverted", json::Value::MakeInt(static_cast<int64_t>(plan.steps.size())));
        body.Set("skipped", json::Value::MakeInt(plan.skipped));
        return Done(body);
    }

    bool PlanWrite(const json::Value& in, WritePlan& plan, Failure& failure) {
        detail::InputReader reader(in);
        std::string name;
        reader.String("name", &name, true, 255);
        reader.String("source", &plan.source, true, std::string::npos);
        reader.String("expected_sha256", &plan.expected, false, 80);
        if (!reader.ok()) {
            return Fail(failure, "invalid_input", reader.message());
        }
        if (plan.source.size() > kMaxScriptSourceBytes) {
            return Fail(failure, "too_large", "A script source is at most 64 KB");
        }
        if (plan.expected.compare(0, 7, "sha256:") == 0) {
            plan.expected = plan.expected.substr(7);
        }
        if (!plan.expected.empty() && !overlay::ValidSha256Hex(plan.expected)) {
            return Fail(failure, "invalid_input", "'expected_sha256' must be the sha256 that read_script returned");
        }
        const std::vector<overlay::ScriptInfo> scripts = deps_.scripts.List();
        const int index = overlay::FindScript(scripts, name);
        if (index < 0 && !overlay::ValidScriptName(name)) {
            return Fail(failure, "invalid_name",
                        "'" + name + "' cannot be a script name: 1 to 64 bytes, no < > : \" / \\ | ? *, no leading or "
                        "trailing dot or space, not a Windows device name");
        }
        plan.name = index >= 0 ? scripts[index].name : name;
        plan.wasRunning = index >= 0 && scripts[index].state == overlay::ScriptState::Running;
        plan.exists = index >= 0 && deps_.scripts.ReadSource(plan.name, plan.old);
        if (plan.expected.empty() && plan.exists) {
            return Fail(failure, "conflict",
                        "A script called '" + plan.name + "' exists; read it with read_script and pass its sha256 as "
                        "expected_sha256");
        }
        if (!plan.expected.empty() && !plan.exists) {
            return Fail(failure, "conflict", "There is no script called '" + plan.name + "' to replace");
        }
        if (!plan.expected.empty() && plan.old.sha256 != plan.expected) {
            return Fail(failure, "conflict",
                        "The script '" + plan.name + "' changed since it was read; read it again");
        }
        return true;
    }

    ToolResult WriteScript(const json::Value& in) {
        WritePlan plan;
        Failure failure;
        if (!PlanWrite(in, plan, failure)) {
            return Error(failure.code, failure.message);
        }
        switch (deps_.scripts.WriteSource(plan.name, plan.source, plan.expected)) {
            case overlay::ScriptFileResult::Done:
                break;
            case overlay::ScriptFileResult::InvalidName:
                return Error("invalid_name", "'" + plan.name + "' cannot be a script name");
            case overlay::ScriptFileResult::TooLarge:
                return Error("too_large", "The script is too large");
            case overlay::ScriptFileResult::Conflict:
                return Error("conflict", plan.expected.empty() ? "The script exists now; read it first"
                                                               : "The script changed since it was read");
            case overlay::ScriptFileResult::NotFound:
                return Error("not_found", "The script was not found");
            default:
                return Error("failed", "The script could not be written");
        }
        overlay::ScriptSource written;
        deps_.scripts.ReadSource(plan.name, written);
        json::Value body = json::Value::MakeObject();
        body.Set("ok", json::Value::MakeBool(true));
        body.Set("name", json::Value::MakeString(plan.name));
        body.Set("sha256", json::Value::MakeString(written.sha256));
        body.Set("held", json::Value::MakeBool(true));
        if (plan.wasRunning) {
            body.Set("was_running", json::Value::MakeBool(true));
        }
        return Done(body);
    }

    bool PlanRun(const json::Value& in, RunPlan& plan, Failure& failure) {
        detail::InputReader reader(in);
        std::string name;
        reader.String("name", &name, true, 255);
        reader.Bool("running", &plan.running, true);
        if (!reader.ok()) {
            return Fail(failure, "invalid_input", reader.message());
        }
        const std::vector<overlay::ScriptInfo> scripts = deps_.scripts.List();
        const int index = overlay::FindScript(scripts, name);
        if (index < 0) {
            return Fail(failure, "not_found", "No script is called '" + name + "'; use list_scripts");
        }
        plan.info = scripts[index];
        return true;
    }

    ToolResult SetScriptRunning(const json::Value& in) {
        RunPlan plan;
        Failure failure;
        if (!PlanRun(in, plan, failure)) {
            return Error(failure.code, failure.message);
        }
        const bool done = plan.running ? deps_.scripts.Start(plan.info.name) : deps_.scripts.Stop(plan.info.name);
        if (!done) {
            return Error("failed", std::string("The script could not be ") + (plan.running ? "started" : "stopped"));
        }
        json::Value body = json::Value::MakeObject();
        body.Set("ok", json::Value::MakeBool(true));
        const std::vector<overlay::ScriptInfo> scripts = deps_.scripts.List();
        const int index = overlay::FindScript(scripts, plan.info.name);
        if (index >= 0) {
            body.Set("state", json::Value::MakeString(overlay::ScriptStateName(scripts[index].state)));
            body.Set("held", json::Value::MakeBool(scripts[index].held));
        }
        return Done(body);
    }

    Deps deps_;
    std::vector<Change> journal_;
};

} // namespace nickel::ai
