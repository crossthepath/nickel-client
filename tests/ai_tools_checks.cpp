// Nickel AI's client tools (native/ai_tools.hpp) on fakes: every tool of the protocol contract, its input validation,
// its errors, the confirmation rule (an action without the player's confirmation changes nothing), the confirmation
// cards drawn from real data, the 16 KB result cap, held script writes with their revision check, and revert_changes.
// No Minecraft, no menu, no Lua, no network.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "ai_tools.hpp"

namespace {

using namespace overlay;
using nickel::ai::tools::AssistantTools;
using nickel::ai::tools::ConfirmCard;
using nickel::ai::tools::ToolResult;
namespace json = nickel::json;

int failed = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++failed;
    }
}

class FakeRegistry final : public ModuleRegistry {
public:
    std::vector<Module> list;
    std::map<std::string, int> keys;
    std::vector<std::pair<int, std::string>> saves; // module, setting id ("" for the on/off state)

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
    void SaveModule(int index, const Setting* changed) override {
        saves.emplace_back(index, changed != nullptr ? changed->id : std::string());
    }
    std::string ReadPreference(const std::string&, const std::string&, size_t) const override { return {}; }
    int ReadPreferenceInt(const std::string&, const std::string&, int fallback) const override { return fallback; }
    int KeyOf(const std::string& id) const override {
        const auto found = keys.find(id);
        return found == keys.end() ? 0 : found->second;
    }
    void BindKey(const std::string&, const std::string&, int) override {}
};

// A revision: 64 lowercase hex characters derived from the text (not SHA-256, but it changes with the text).
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

// Scripts in memory, with the library's write rules: expected revision, held after a write.
class FakeLibrary final : public ScriptLibrary {
public:
    std::vector<ScriptInfo> scripts;
    std::map<std::string, std::string> files; // listed name -> text
    std::vector<std::string> started, stopped;
    int writes = 0;
    bool failStart = false;

    void AddScript(const std::string& name, const std::string& text, ScriptState state = ScriptState::Stopped) {
        ScriptInfo info;
        info.name = name;
        info.state = state;
        scripts.push_back(info);
        files[name] = text;
    }

    ScriptInfo* Info(const std::string& name) {
        const int index = FindScript(scripts, name);
        return index < 0 ? nullptr : &scripts[static_cast<size_t>(index)];
    }

    std::vector<ScriptInfo> List() const override { return scripts; }
    std::wstring Folder() const override { return L"C:\\fake\\scripts"; }
    bool ReadSource(const std::string& name, ScriptSource& source) const override {
        const int index = FindScript(scripts, name);
        if (index < 0) {
            return false;
        }
        const auto file = files.find(scripts[static_cast<size_t>(index)].name);
        if (file == files.end()) {
            return false;
        }
        source.text = file->second;
        source.sha256 = FakeSha(file->second);
        return true;
    }
    // "error N" anywhere in the source gives an error at line N.
    std::vector<CompileError> Compile(const std::string& source) const override {
        std::vector<CompileError> errors;
        const size_t at = source.find("error ");
        if (at != std::string::npos) {
            errors.push_back({std::atoi(source.c_str() + at + 6), "Expected 'end', got <eof>"});
        }
        return errors;
    }
    bool HasBackup(const std::string&) const override { return false; }
    ScriptFileResult WriteSource(const std::string& name, const std::string& source,
                                 const std::string& expectedSha256) override {
        ++writes;
        ScriptInfo* info = Info(name);
        if (info == nullptr && !ValidScriptName(name)) {
            return ScriptFileResult::InvalidName;
        }
        if (source.size() > kMaxScriptBytes) {
            return ScriptFileResult::TooLarge;
        }
        const bool exists = info != nullptr;
        if (expectedSha256.empty() ? exists : (!exists || FakeSha(files[info->name]) != expectedSha256)) {
            return ScriptFileResult::Conflict;
        }
        if (!exists) {
            AddScript(name, source);
            info = Info(name);
        }
        files[info->name] = source;
        info->held = true;
        return ScriptFileResult::Done;
    }
    ScriptFileResult Remove(const std::string&, const std::string&) override { return ScriptFileResult::Failed; }
    ScriptFileResult RestoreBackup(const std::string&, const std::string&) override {
        return ScriptFileResult::NotFound;
    }
    bool Hold(const std::string&) override { return false; }
    bool Start(const std::string& name) override {
        ScriptInfo* info = Info(name);
        if (info == nullptr || failStart) {
            return false;
        }
        started.push_back(name);
        info->state = ScriptState::Running;
        info->held = false;
        return true;
    }
    bool Stop(const std::string& name) override {
        ScriptInfo* info = Info(name);
        if (info == nullptr) {
            return false;
        }
        stopped.push_back(name);
        info->state = ScriptState::Stopped;
        return true;
    }
};

class FakeUi final : public nickel::ai::tools::UiNavigator {
public:
    std::vector<std::string> shown;
    bool ShowModule(int index, const std::string& id) override {
        shown.push_back("module:" + id + "@" + std::to_string(index));
        return true;
    }
    bool ShowTab(const std::string& tab) override {
        if (tab == "settings" || tab == "Scripting") {
            shown.push_back("tab:" + tab);
            return true;
        }
        return false;
    }
};

class FakeStatus final : public nickel::ai::tools::ClientStatusSource {
public:
    nickel::ai::tools::ClientStatus status{"1.1.0-beta", "1.26.62.1", true, ""};
    nickel::ai::tools::ClientStatus Status() const override { return status; }
};

Setting MakeSetting(const std::string& id, const std::string& name, const std::string& kind, double min, double max,
                    double step, double number, const std::string& text = "", std::vector<std::string> options = {},
                    const std::string& unit = "") {
    Setting setting{};
    setting.id = id;
    setting.name = name;
    setting.description = name + " description";
    setting.kind = kind;
    setting.minimum = min;
    setting.maximum = max;
    setting.step = step;
    setting.number = number;
    setting.text = text;
    setting.options = std::move(options);
    setting.unit = unit;
    return setting;
}

Module MakeModule(const std::string& id, const std::string& name, const std::string& category,
                  const std::string& description, std::vector<Setting> settings = {}, bool enabled = false) {
    Module module{};
    module.id = id;
    module.name = name;
    module.category = category;
    module.description = description;
    module.settings = std::move(settings);
    module.enabled = enabled;
    return module;
}

json::Value Parse(const std::string& text) {
    json::Value value;
    json::Parse(text, &value);
    return value;
}

json::Value Input(const std::string& text) {
    json::Value value;
    if (!json::Parse(text, &value)) {
        std::printf("bad test input: %s\n", text.c_str());
        ++failed;
    }
    return value;
}

bool IsError(const ToolResult& result, const std::string& code) {
    const json::Value body = Parse(result.content);
    return result.isError && result.error == code && body["error"].AsString() == code &&
           body["message"].IsString() && !body["message"].AsString().empty();
}

struct World {
    FakeRegistry registry;
    FakeLibrary library;
    nickel::ai::tools::RegistrySettingsWriter writer{registry};
    FakeUi ui;
    FakeStatus status;
    std::vector<Module> defaults;

    World() {
        registry.list.push_back(MakeModule(
                "kill_aura", "Kill Aura", "Combat", "Automatically attacks nearby targets",
                {MakeSetting("distance", "Distance", "number", 0, 20, 0.1, 3, "", {}, " blocks"),
                 MakeSetting("mode", "Mode", "enum", 0, 1, 0.01, 0, "Default", {"Default", "Silent", "Teleport"}),
                 MakeSetting("players_only", "Players Only", "bool", 0, 1, 1, 0),
                 MakeSetting("color", "Color", "color", 0, 1, 0.01, 0, "#ffffffff"),
                 MakeSetting("delay", "Delay", "number", 0, 1000, 10, 100, "", {}, " ms"),
                 MakeSetting("attacks_per_second", "Attacks Per Second", "range", 1, 50, 0.1, 8, "12.0"),
                 MakeSetting("targets", "Targets", "list", 0, 1, 0.01, 0, ""),
                 MakeSetting("internal", "Internal", "hidden", 0, 1, 1, 0)}));
        registry.list.push_back(MakeModule("fullbright", "Fullbright", "Visuals", "Brightens dark areas"));
        registry.list.push_back(MakeModule(
                "nick_hider", "Nick Hider", "Utility", "Hides your name",
                {MakeSetting("name", "Name", "text", 0, 1, 0.01, 0, "SecretName")}, true));
        registry.list.push_back(MakeModule("item_name", "Item Name", "Visuals", "Renames held items",
                                           {MakeSetting("format", "Format", "text", 0, 1, 0.01, 0, "{name}!")}));
        registry.list.push_back(MakeModule("script.old.gone", "Gone Module", "", "A stopped script's module", {},
                                           true));
        registry.list.push_back(MakeModule("script.night.bright", "Night Bright", "Visuals", "Brightens at night",
                                           {}, true));
        registry.list.push_back(MakeModule("watermark", "Watermark", "HUD", "Shows the client name"));
        registry.keys["kill_aura"] = 'R';
        defaults = registry.list;

        library.AddScript("aimbot",
                          "local m = nickel.module{ name = \"Aim\", category = \"Combat\" }\nm:onEnable(function() end)\n",
                          ScriptState::StoppedAfterErrors);
        library.scripts.back().errors = {"aimbot:1: e1", "aimbot:2: e2", "aimbot:3: e3", "aimbot:4: e4"};
        library.scripts.back().modules.push_back({"Aim", 5, true});
        library.AddScript("Auto Totem", "-- totem\nlocal t = nickel.module({ category = 'Player', name = 'Totem' })\n",
                          ScriptState::Running);
        library.AddScript("held one", "print('x')\n");
        library.scripts.back().held = true;
    }

    AssistantTools Tools() { return AssistantTools({registry, library, writer, ui, status, &defaults}); }
};

void ChecksBasics() {
    World world;
    AssistantTools tools = world.Tools();
    Check(AssistantTools::ToolNames().size() == 12, "twelve tools");
    Check(IsError(tools.Run("delete_everything", Input("{}")), "unknown_tool"), "an unknown tool is refused");
    Check(IsError(tools.Run("get_module", Input("[1]")), "invalid_input"), "an input that is not an object is refused");
    Check(!tools.Run("get_client_status", json::Value()).isError, "a null input counts as {}");
    for (const char* name : {"set_setting", "revert_changes", "set_module_enabled", "write_script",
                             "set_script_running"}) {
        Check(AssistantTools::NeedsConfirmation(name, Input("{}")), "every action tool needs confirmation");
    }
    for (const char* name : {"search_modules", "get_module", "get_client_status", "list_scripts", "read_script",
                             "validate_script", "navigate_ui"}) {
        Check(!AssistantTools::NeedsConfirmation(name, Input("{}")), "read tools need no confirmation");
    }
    Check(!AssistantTools::NeedsRenderThread("write_script") && !AssistantTools::NeedsRenderThread("read_script") &&
                  AssistantTools::NeedsRenderThread("set_setting") &&
                  AssistantTools::NeedsRenderThread("set_script_running"),
          "the threads of the tools");
    const ToolResult declined = AssistantTools::Declined();
    Check(IsError(declined, "declined"), "a refused action is the error declined");
    const ConfirmCard notAction = tools.Describe("get_module", Input("{\"id\":\"kill_aura\"}"));
    Check(!notAction.ok && notAction.error == "not_an_action", "a read tool has no card");
}

void ChecksSearch() {
    World world;
    AssistantTools tools = world.Tools();
    ToolResult result = tools.Run("search_modules", Input("{\"query\":\"aura\"}"));
    json::Value body = Parse(result.content);
    Check(!result.isError && body["modules"].size() == 1 && body["modules"].At(0)["id"].AsString() == "kill_aura",
          "search finds a module by a word of its name");
    Check(body["modules"].At(0)["category"].AsString() == "combat" && !body["modules"].At(0)["enabled"].AsBool() &&
                  body["modules"].At(0)["description"].AsString() == "Automatically attacks nearby targets" &&
                  body["modules"].At(0)["name"].AsString() == "Kill Aura",
          "a search result has id, name, category, enabled and description");

    body = Parse(tools.Run("search_modules", Input("{\"query\":\"bright\",\"category\":\"visuals\",\"x\":1}")).content);
    Check(body["modules"].size() == 2 && body["modules"].At(0)["id"].AsString() == "fullbright" &&
                  body["modules"].At(1)["source"].AsString() == "script",
          "category filter, unknown fields ignored, script modules marked");

    body = Parse(tools.Run("search_modules", Input("{\"query\":\"\"}")).content);
    bool hiddenListed = false;
    for (size_t i = 0; i < body["modules"].size(); ++i) {
        hiddenListed = hiddenListed || body["modules"].At(i)["id"].AsString() == "script.old.gone";
    }
    Check(body["modules"].size() == 6 && !hiddenListed && body["total"].AsInt64() == 6,
          "an empty query lists every visible module, never a hidden one");

    body = Parse(tools.Run("search_modules", Input("{\"query\":\"\",\"category\":\"hud\"}")).content);
    Check(body["modules"].size() == 1 && body["modules"].At(0)["id"].AsString() == "watermark", "the HUD category");

    Check(IsError(tools.Run("search_modules", Input("{\"query\":\"x\",\"category\":\"Weapons\"}")), "invalid_input"),
          "an unknown category is invalid input");
    Check(IsError(tools.Run("search_modules", Input("{\"query\":3}")), "invalid_input"), "a query of the wrong type");
    Check(IsError(tools.Run("search_modules", Input("{}")), "invalid_input"), "a missing query");
    body = Parse(tools.Run("search_modules", Input("{\"query\":\"nothing-like-this\"}")).content);
    Check(body["modules"].IsArray() && body["modules"].size() == 0, "no match is an empty list");
}

void ChecksGetModule() {
    World world;
    AssistantTools tools = world.Tools();
    world.registry.list[0].settings[0].number = 4.5; // the player changed it; the default stays 3
    ToolResult result = tools.Run("get_module", Input("{\"id\":\"kill_aura\"}"));
    json::Value body = Parse(result.content);
    const json::Value& settings = body["settings"];
    Check(!result.isError && body["id"].AsString() == "kill_aura" && body["name"].AsString() == "Kill Aura" &&
                  body["category"].AsString() == "combat" && !body["enabled"].AsBool() &&
                  body["key"].AsString() == "R" && body["description"].IsString(),
          "get_module: id, name, category, description, enabled, key");
    Check(settings.size() == 7, "get_module lists every setting but hidden ones");
    Check(settings.At(0)["name"].AsString() == "Distance" && settings.At(0)["type"].AsString() == "number" &&
                  settings.At(0)["value"].AsDouble() == 4.5 && settings.At(0)["default"].AsDouble() == 3 &&
                  settings.At(0)["min"].AsDouble() == 0 && settings.At(0)["max"].AsDouble() == 20 &&
                  settings.At(0)["settable"].AsBool(),
          "a number setting: value, default, min, max");
    Check(settings.At(1)["value"].AsString() == "Default" && settings.At(1)["choices"].size() == 3 &&
                  settings.At(1)["choices"].At(2).AsString() == "Teleport",
          "an enum setting: value and choices");
    Check(settings.At(2)["value"].IsBool() && !settings.At(2)["value"].AsBool(), "a bool setting is true or false");
    Check(settings.At(5)["value"].IsArray() && settings.At(5)["value"].At(1).AsDouble() == 12 &&
                  !settings.At(5)["settable"].AsBool(),
          "a range is [low, high] and read-only");
    Check(settings.At(6)["value"].IsNull() && !settings.At(6)["settable"].AsBool(), "a list is read-only");

    body = Parse(tools.Run("get_module", Input("{\"id\":\"nick_hider\"}")).content);
    Check(body["settings"].At(0)["value"].AsString() == "<hidden>" && body["key"].AsString() == "None",
          "a private text value is never sent");
    body = Parse(tools.Run("get_module", Input("{\"id\":\"item_name\"}")).content);
    Check(body["settings"].At(0)["value"].AsString() == "{name}!", "other text values are sent");

    Check(IsError(tools.Run("get_module", Input("{\"id\":\"no_such\"}")), "not_found"), "an unknown module");
    Check(IsError(tools.Run("get_module", Input("{\"id\":\"script.old.gone\"}")), "not_found"),
          "a hidden module is not found");
    Check(IsError(tools.Run("get_module", Input("{\"id\":7}")), "invalid_input"), "an id of the wrong type");

    AssistantTools noDefaults({world.registry, world.library, world.writer, world.ui, world.status});
    body = Parse(noDefaults.Run("get_module", Input("{\"id\":\"kill_aura\"}")).content);
    Check(body["settings"].At(0)["default"].IsNull(), "default is null without the catalogue");
}

void ChecksStatus() {
    World world;
    AssistantTools tools = world.Tools();
    json::Value body = Parse(tools.Run("get_client_status", Input("{}")).content);
    Check(body["nickel_version"].AsString() == "1.1.0-beta" && body["game_version"].AsString() == "1.26.62.1" &&
                  body["in_world"].AsBool() && body.Find("server") == nullptr,
          "client status without a server");
    Check(body["enabled_modules"].size() == 2 && body["enabled_modules"].At(0).AsString() == "nick_hider" &&
                  body["enabled_modules"].At(1).AsString() == "script.night.bright",
          "enabled_modules lists the visible enabled modules");
    world.status.status.server = "server";
    body = Parse(tools.Run("get_client_status", Input("{}")).content);
    Check(body["server"].AsString() == "server", "the server when the client gives one");
}

void ChecksScripts() {
    World world;
    AssistantTools tools = world.Tools();
    json::Value body = Parse(tools.Run("list_scripts", Input("{}")).content);
    const json::Value& first = body["scripts"].At(0);
    Check(body["scripts"].size() == 3 && first["name"].AsString() == "aimbot" &&
                  first["state"].AsString() == "Stopped after errors" && !first["held"].AsBool() &&
                  first["modules"].size() == 1 && first["modules"].At(0).AsString() == "Aim",
          "list_scripts: name, state, held, modules");
    Check(first["errors"].size() == 3 && first["errors"].At(0).AsString() == "aimbot:4: e4" &&
                  first["errors"].At(2).AsString() == "aimbot:2: e2",
          "list_scripts: the last three errors, newest first");
    Check(body["scripts"].At(2)["held"].AsBool(), "a held script is marked");

    ToolResult result = tools.Run("read_script", Input("{\"name\":\"AUTO TOTEM\"}"));
    body = Parse(result.content);
    Check(!result.isError && body["name"].AsString() == "Auto Totem" &&
                  body["source"].AsString() == world.library.files["Auto Totem"] &&
                  body["sha256"].AsString() == FakeSha(world.library.files["Auto Totem"]) &&
                  body.Find("truncated") == nullptr,
          "read_script by a name in any case: name, source, sha256");
    Check(IsError(tools.Run("read_script", Input("{\"name\":\"missing\"}")), "not_found"), "read_script: unknown");
    Check(IsError(tools.Run("read_script", Input("{\"name\":false}")), "invalid_input"), "read_script: wrong type");

    std::string big;
    for (int i = 0; i < 4000; ++i) {
        big += "print(\"line " + std::to_string(i) + " \xc3\xa9\")\n";
    }
    world.library.AddScript("big", big);
    result = tools.Run("read_script", Input("{\"name\":\"big\"}"));
    body = Parse(result.content);
    Check(!result.isError && result.content.size() <= nickel::ai::tools::kMaxToolResultBytes &&
                  body["truncated"].AsBool() && body["sha256"].AsString() == FakeSha(big) &&
                  big.compare(0, body["source"].AsString().size(), body["source"].AsString()) == 0 &&
                  body["source"].AsString().size() > 12000,
          "a long script is cut to 16 KB with a marker, the revision is the whole file's");

    body = Parse(tools.Run("validate_script", Input("{\"source\":\"print(1)\"}")).content);
    Check(body["ok"].AsBool() && body["errors"].size() == 0, "validate_script: a script that compiles");
    body = Parse(tools.Run("validate_script", Input("{\"source\":\"error 12\"}")).content);
    Check(!body["ok"].AsBool() && body["errors"].size() == 1 && body["errors"].At(0)["line"].AsInt64() == 12 &&
                  !body["errors"].At(0)["message"].AsString().empty(),
          "validate_script: errors with their line");
    json::Value tooBig = json::Value::MakeObject();
    tooBig.Set("source", json::Value::MakeString(std::string(nickel::ai::tools::kMaxScriptSourceBytes + 1, 'a')));
    Check(IsError(tools.Run("validate_script", tooBig), "too_large"), "validate_script: more than 64 KB");
    Check(IsError(tools.Run("validate_script", Input("{}")), "invalid_input"), "validate_script: no source");
}

void ChecksNavigate() {
    World world;
    AssistantTools tools = world.Tools();
    Check(!tools.Run("navigate_ui", Input("{\"target\":\"kill_aura\"}")).isError &&
                  world.ui.shown.back() == "module:kill_aura@0",
          "navigate_ui to a module id");
    ToolResult result = tools.Run("navigate_ui", Input("{\"target\":\"settings\"}"));
    Check(!result.isError && Parse(result.content)["ok"].AsBool() && world.ui.shown.back() == "tab:settings",
          "navigate_ui to a tab");
    Check(IsError(tools.Run("navigate_ui", Input("{\"target\":\"nowhere\"}")), "not_found"), "navigate_ui: unknown");
    Check(IsError(tools.Run("navigate_ui", Input("{\"target\":\"script.old.gone\"}")), "not_found"),
          "navigate_ui: a hidden module is not shown");
    Check(IsError(tools.Run("navigate_ui", Input("{\"target\":\"\"}")), "invalid_input"), "navigate_ui: empty");
}

void ChecksSetSetting() {
    World world;
    AssistantTools tools = world.Tools();
    const json::Value input = Input("{\"module_id\":\"kill_aura\",\"setting\":\"distance\",\"value\":4.5}");
    Check(IsError(tools.Run("set_setting", input), "confirmation_required") &&
                  world.registry.list[0].settings[0].number == 3 && world.registry.saves.empty() &&
                  tools.Journal().empty(),
          "set_setting without confirmation changes nothing");

    ConfirmCard card = tools.Describe("set_setting", input);
    Check(card.ok && card.lines.size() == 1 && card.lines[0].what == "Kill Aura: Distance" &&
                  card.lines[0].before == "3 blocks" && card.lines[0].after == "4.5 blocks",
          "the card shows the module, the setting, the current and the new value");

    ToolResult result = tools.Run("set_setting", input, true);
    json::Value body = Parse(result.content);
    Check(!result.isError && body["ok"].AsBool() && body["previous"].AsDouble() == 3 &&
                  body["value"].AsDouble() == 4.5 && world.registry.list[0].settings[0].number == 4.5 &&
                  world.registry.saves.size() == 1 && world.registry.saves[0].second == "distance" &&
                  tools.Journal().size() == 1,
          "set_setting confirmed: written, saved, journalled, previous value returned");

    body = Parse(tools.Run("set_setting", Input("{\"module_id\":\"kill_aura\",\"setting\":\"Distance\","
                                                "\"value\":99}"), true).content);
    Check(body["value"].AsDouble() == 20 && world.registry.list[0].settings[0].number == 20,
          "a number is clamped to its range, a setting found by its name");
    body = Parse(tools.Run("set_setting", Input("{\"module_id\":\"kill_aura\",\"setting\":\"delay\",\"value\":123.6}"),
                           true).content);
    Check(body["value"].AsDouble() == 124, "a number with a whole step is rounded");
    body = Parse(tools.Run("set_setting", Input("{\"module_id\":\"kill_aura\",\"setting\":\"mode\","
                                                "\"value\":\"silent\"}"), true).content);
    Check(world.registry.list[0].settings[1].text == "Silent" && body["previous"].AsString() == "Default",
          "an enum option in any case is stored as the option");
    Check(IsError(tools.Run("set_setting", Input("{\"module_id\":\"kill_aura\",\"setting\":\"mode\","
                                                 "\"value\":\"Rage\"}"), true), "invalid_value"),
          "an unknown option is an invalid value");
    tools.Run("set_setting", Input("{\"module_id\":\"kill_aura\",\"setting\":\"color\",\"value\":\"#FF0000\"}"), true);
    Check(world.registry.list[0].settings[3].text == "#ff0000ff", "a colour is stored as #rrggbbaa");
    Check(IsError(tools.Run("set_setting", Input("{\"module_id\":\"kill_aura\",\"setting\":\"color\","
                                                 "\"value\":\"red\"}"), true), "invalid_value"),
          "a colour that is not hex is an invalid value");
    tools.Run("set_setting", Input("{\"module_id\":\"kill_aura\",\"setting\":\"players_only\",\"value\":true}"), true);
    Check(world.registry.list[0].settings[2].number == 1, "a bool setting");
    Check(IsError(tools.Run("set_setting", Input("{\"module_id\":\"kill_aura\",\"setting\":\"players_only\","
                                                 "\"value\":\"yes\"}"), true), "invalid_value"),
          "a bool setting refuses a string");
    Check(IsError(tools.Run("set_setting", Input("{\"module_id\":\"kill_aura\",\"setting\":\"distance\","
                                                 "\"value\":\"far\"}"), true), "invalid_value"),
          "a number setting refuses a string");
    Check(IsError(tools.Run("set_setting", Input("{\"module_id\":\"kill_aura\",\"setting\":\"targets\","
                                                 "\"value\":\"x\"}"), true), "read_only_setting"),
          "a list setting is read-only");
    Check(IsError(tools.Run("set_setting", Input("{\"module_id\":\"kill_aura\",\"setting\":\"attacks_per_second\","
                                                 "\"value\":5}"), true), "read_only_setting"),
          "a range setting is read-only");
    Check(IsError(tools.Run("set_setting", Input("{\"module_id\":\"kill_aura\",\"setting\":\"internal\","
                                                 "\"value\":1}"), true), "not_found"),
          "a hidden setting is not found");
    Check(IsError(tools.Run("set_setting", Input("{\"module_id\":\"nope\",\"setting\":\"x\",\"value\":1}"), true),
                  "not_found"),
          "an unknown module");
    Check(IsError(tools.Run("set_setting", Input("{\"module_id\":1,\"setting\":\"x\",\"value\":1}"), true),
                  "invalid_input"),
          "a module id of the wrong type");
    Check(IsError(tools.Run("set_setting", Input("{\"module_id\":\"kill_aura\",\"setting\":\"distance\"}"), true),
                  "invalid_input"),
          "a missing value");

    body = Parse(tools.Run("set_setting", Input("{\"module_id\":\"nick_hider\",\"setting\":\"name\","
                                                "\"value\":\"Other\"}"), true).content);
    Check(world.registry.list[2].settings[0].text == "Other" && body["previous"].AsString() == "<hidden>" &&
                  body["value"].AsString() == "<hidden>",
          "a private text is written, its values are not sent");
    Check(IsError(tools.Run("set_setting", Input("{\"module_id\":\"item_name\",\"setting\":\"format\",\"value\":\"" +
                                                 std::string(300, 'x') + "\"}"), true), "invalid_value"),
          "a text over 256 bytes is an invalid value");

    const size_t journalled = tools.Journal().size();
    tools.Run("set_setting", Input("{\"module_id\":\"kill_aura\",\"setting\":\"mode\",\"value\":\"Silent\"}"), true);
    Check(tools.Journal().size() == journalled, "a change to the same value is not journalled");

    card = tools.Describe("set_setting", Input("{\"module_id\":\"nope\",\"setting\":\"x\",\"value\":1}"));
    Check(!card.ok && card.error == "not_found", "a card for an invalid call names the error");
}

void ChecksEnabledAndRevert() {
    World world;
    AssistantTools tools = world.Tools();
    const json::Value on = Input("{\"module_id\":\"fullbright\",\"enabled\":true}");
    Check(IsError(tools.Run("set_module_enabled", on), "confirmation_required") && !world.registry.list[1].enabled,
          "set_module_enabled without confirmation changes nothing");
    ConfirmCard card = tools.Describe("set_module_enabled", on);
    Check(card.ok && card.title == "Turn on Fullbright" && card.lines[0].before == "Off" &&
                  card.lines[0].after == "On",
          "the card of set_module_enabled");
    json::Value body = Parse(tools.Run("set_module_enabled", on, true).content);
    Check(body["ok"].AsBool() && !body["previous"].AsBool() && world.registry.list[1].enabled &&
                  world.registry.saves.back() == std::make_pair(1, std::string()),
          "set_module_enabled confirmed: on and saved, previous returned");
    Check(IsError(tools.Run("set_module_enabled", Input("{\"module_id\":\"fullbright\",\"enabled\":\"yes\"}"), true),
                  "invalid_input"),
          "enabled must be a bool");
    Check(IsError(tools.Run("set_module_enabled", Input("{\"module_id\":\"script.old.gone\",\"enabled\":false}"),
                            true), "not_found"),
          "a hidden module cannot be switched");
    const size_t journalled = tools.Journal().size();
    tools.Run("set_module_enabled", on, true);
    Check(tools.Journal().size() == journalled, "switching to the current state is not journalled");

    // distance 3 -> 5 -> 7, mode Default -> Silent; then the player changes mode to Teleport himself
    tools.Run("set_setting", Input("{\"module_id\":\"kill_aura\",\"setting\":\"distance\",\"value\":5}"), true);
    tools.Run("set_setting", Input("{\"module_id\":\"kill_aura\",\"setting\":\"distance\",\"value\":7}"), true);
    tools.Run("set_setting", Input("{\"module_id\":\"kill_aura\",\"setting\":\"mode\",\"value\":\"Silent\"}"), true);
    world.registry.list[0].settings[1].text = "Teleport";

    Check(IsError(tools.Run("revert_changes", Input("{}")), "confirmation_required") &&
                  world.registry.list[0].settings[0].number == 7,
          "revert_changes without confirmation changes nothing");
    card = tools.Describe("revert_changes", Input("{}"));
    Check(card.ok && card.lines.size() == 3 && card.lines[0].what == "Kill Aura: Distance" &&
                  card.lines[0].before == "7 blocks" && card.lines[0].after == "5 blocks" &&
                  card.lines[1].after == "3 blocks" && card.lines[2].what == "Fullbright" &&
                  card.lines[2].after == "Off",
          "the revert card lists what goes back, newest first, without the change the player overrode");
    ToolResult result = tools.Run("revert_changes", Input("{}"), true);
    body = Parse(result.content);
    Check(!result.isError && body["ok"].AsBool() && body["reverted"].AsInt64() == 3 &&
                  body["skipped"].AsInt64() == 1 && world.registry.list[0].settings[0].number == 3 &&
                  world.registry.list[0].settings[1].text == "Teleport" && !world.registry.list[1].enabled &&
                  tools.Journal().empty(),
          "revert_changes restores this session's changes and keeps the player's own");
    body = Parse(tools.Run("revert_changes", Input("{}"), true).content);
    Check(body["reverted"].AsInt64() == 0, "nothing left to revert");
}

void ChecksWriteScript() {
    World world;
    AssistantTools tools = world.Tools();
    json::Value create = json::Value::MakeObject();
    create.Set("name", json::Value::MakeString("fly helper"));
    create.Set("source", json::Value::MakeString("local m = nickel.module{name = \"Fly\"}\n"));
    create.Set("expected_sha256", json::Value());
    Check(IsError(tools.Run("write_script", create), "confirmation_required") && world.library.writes == 0,
          "write_script without confirmation writes nothing");
    ConfirmCard card = tools.Describe("write_script", create);
    Check(card.ok && card.newScript && card.script == "fly helper" && card.diff == "+local m = nickel.module{name = \"Fly\"}\n" &&
                  card.added == 1 && card.removed == 0 && card.compileErrors.empty() &&
                  card.lines[0].before == "(new script)",
          "the card of a new script shows its whole code");
    ToolResult result = tools.Run("write_script", create, true);
    json::Value body = Parse(result.content);
    Check(!result.isError && body["ok"].AsBool() && body["held"].AsBool() &&
                  body["sha256"].AsString() == FakeSha("local m = nickel.module{name = \"Fly\"}\n") &&
                  world.library.Info("fly helper") != nullptr && world.library.Info("fly helper")->held &&
                  world.library.started.empty(),
          "a written script is held and not started");

    const json::Value replaceBlind = Input("{\"name\":\"aimbot\",\"source\":\"print(2)\"}");
    Check(IsError(tools.Run("write_script", replaceBlind, true), "conflict"),
          "replacing an existing script needs its revision");
    Check(IsError(tools.Run("write_script", Input("{\"name\":\"aimbot\",\"source\":\"x\",\"expected_sha256\":\"" +
                                                  std::string(64, 'a') + "\"}"), true), "conflict"),
          "a stale revision is a conflict");
    Check(IsError(tools.Run("write_script", Input("{\"name\":\"aimbot\",\"source\":\"x\","
                                                  "\"expected_sha256\":\"abc\"}"), true), "invalid_input"),
          "a malformed revision is invalid input");

    const std::string oldText = world.library.files["Auto Totem"];
    const std::string newText = "-- totem\nlocal t = nickel.module({ category = 'Player', name = 'Totem' })\n"
                                "t:onEnable(function() end)\nerror 4\n";
    json::Value replace = json::Value::MakeObject();
    replace.Set("name", json::Value::MakeString("auto totem"));
    replace.Set("source", json::Value::MakeString(newText));
    replace.Set("expected_sha256", json::Value::MakeString("sha256:" + FakeSha(oldText)));
    card = tools.Describe("write_script", replace);
    Check(card.ok && !card.newScript && card.script == "Auto Totem" && card.added == 2 && card.removed == 0 &&
                  card.diff.find("+t:onEnable(function() end)\n") != std::string::npos &&
                  card.diff.find(" local t = nickel.module({ category = 'Player', name = 'Totem' })\n") !=
                          std::string::npos &&
                  card.compileErrors.size() == 1 && card.compileErrors[0].line == 4,
          "the card of a new version shows the diff and the compile errors");
    result = tools.Run("write_script", replace, true);
    body = Parse(result.content);
    Check(!result.isError && body["name"].AsString() == "Auto Totem" && body["was_running"].AsBool() &&
                  world.library.files["Auto Totem"] == newText && world.library.Info("Auto Totem")->held,
          "replacing with the right revision (sha256: prefix allowed) writes and holds; was_running is reported");

    Check(IsError(tools.Run("write_script", Input("{\"name\":\"CON\",\"source\":\"x\"}"), true), "invalid_name"),
          "a device name is not a script name");
    Check(IsError(tools.Run("write_script", Input("{\"name\":\"a/b\",\"source\":\"x\"}"), true), "invalid_name"),
          "a path is not a script name");
    json::Value tooBig = json::Value::MakeObject();
    tooBig.Set("name", json::Value::MakeString("big"));
    tooBig.Set("source", json::Value::MakeString(std::string(nickel::ai::tools::kMaxScriptSourceBytes + 1, 'a')));
    Check(IsError(tools.Run("write_script", tooBig, true), "too_large"), "a source over 64 KB is too large");
    Check(IsError(tools.Run("write_script", Input("{\"name\":\"x\"}"), true), "invalid_input"), "a missing source");

    // a diff of a middle change
    const nickel::ai::tools::detail::LineDiff diff = nickel::ai::tools::detail::DiffLines("a\nb\nc\nd\ne\n", "a\nb\nX\nd\ne\n");
    Check(diff.added == 1 && diff.removed == 1 && diff.text == "@@ line 1 @@\n a\n b\n-c\n+X\n d\n e\n",
          "a line diff with context");
}

void ChecksRunScript() {
    World world;
    AssistantTools tools = world.Tools();
    const json::Value start = Input("{\"name\":\"held one\",\"running\":true}");
    Check(IsError(tools.Run("set_script_running", start), "confirmation_required") && world.library.started.empty(),
          "set_script_running without confirmation starts nothing");
    ConfirmCard card = tools.Describe("set_script_running", Input("{\"name\":\"auto totem\",\"running\":true}"));
    Check(card.ok && card.declaredModules.size() == 1 && card.declaredModules[0] == "Totem" &&
                  card.lines[0].before == "Running",
          "the start card lists the modules the source declares");
    card = tools.Describe("set_script_running", start);
    Check(card.ok && card.lines[0].before == "Not started" && card.lines[0].after == "Running",
          "the start card of a held script");
    ToolResult result = tools.Run("set_script_running", start, true);
    json::Value body = Parse(result.content);
    Check(!result.isError && body["ok"].AsBool() && body["state"].AsString() == "Running" &&
                  !body["held"].AsBool() && world.library.started.size() == 1,
          "set_script_running confirmed: started and released");
    result = tools.Run("set_script_running", Input("{\"name\":\"Auto Totem\",\"running\":false}"), true);
    Check(!result.isError && world.library.stopped.size() == 1 && world.library.stopped[0] == "Auto Totem",
          "stopping a script");
    Check(IsError(tools.Run("set_script_running", Input("{\"name\":\"none\",\"running\":true}"), true), "not_found"),
          "an unknown script");
    Check(IsError(tools.Run("set_script_running", Input("{\"name\":\"aimbot\",\"running\":1}"), true),
                  "invalid_input"),
          "running must be a bool");
    world.library.failStart = true;
    Check(IsError(tools.Run("set_script_running", Input("{\"name\":\"aimbot\",\"running\":true}"), true), "failed"),
          "a refused start is failed");
}

void ChecksSizeCap() {
    World world;
    for (int i = 0; i < 400; ++i) {
        world.registry.list.push_back(MakeModule("filler_" + std::to_string(i), "Filler " + std::to_string(i),
                                                 "Utility", std::string(250, 'd'), {}, true));
    }
    AssistantTools tools = world.Tools();
    ToolResult result = tools.Run("search_modules", Input("{\"query\":\"filler\"}"));
    json::Value body = Parse(result.content);
    Check(!result.isError && result.content.size() <= nickel::ai::tools::kMaxToolResultBytes && body["truncated"].AsBool() &&
                  body["modules"].size() <= 25 && body["total"].AsInt64() == 400,
          "search results are capped with a marker");
    result = tools.Run("get_client_status", Input("{}"));
    body = Parse(result.content);
    Check(!result.isError && result.content.size() <= nickel::ai::tools::kMaxToolResultBytes,
          "the client status stays within 16 KB");
    Module wide = MakeModule("wide", "Wide", "Utility", "Many settings");
    for (int i = 0; i < 300; ++i) {
        wide.settings.push_back(MakeSetting("s" + std::to_string(i), "Setting number " + std::to_string(i), "enum", 0,
                                            1, 0.01, 0, "A", {"A", "B", "C", "D", "E"}));
    }
    world.registry.list.push_back(wide);
    result = tools.Run("get_module", Input("{\"id\":\"wide\"}"));
    body = Parse(result.content);
    Check(!result.isError && result.content.size() <= nickel::ai::tools::kMaxToolResultBytes && body["truncated"].AsBool() &&
                  body["settings"].size() > 10,
          "get_module of a large module is cut with a marker");
}

void ChecksDeclaredModules() {
    using nickel::ai::tools::detail::DeclaredModules;
    const std::string source =
            "local a = nickel.module{ name = \"A\", category = \"Combat\" }\n"
            "local b = nickel.module({ settings = { name = \"inner\" }, name = 'B' })\n"
            "local module = nickel.module\n"
            "local c = module{ category = \"HUD\", -- name = \"Commented\"\n name = \"C\" }\n"
            "local d = nickel.module{ description = \"name = 'N'\", name = \"D\" }\n"
            "local again = nickel.module{ name = \"A\" }\n"
            "local x = foo.module{ name = \"X\" }\n"
            "local y = othermodule{ name = \"Y\" }\n"
            "local z = obj:module{ name = \"Z\" }\n"
            "local w = nickel.modules.get(\"W\")\n"
            "local dyn = nickel.module{ name = someName }\n";
    const std::vector<std::string> names = DeclaredModules(source);
    Check(names == std::vector<std::string>({"A", "B", "C", "D"}),
          "declared modules: nickel.module{...}, nickel.module({...}), a local alias; nested tables, comments, "
          "strings, other tables' module and repeats are skipped");
    Check(DeclaredModules("local m = module(\"Old\")\n").empty(), "a string argument is not the real API");

    World world;
    AssistantTools tools = world.Tools();
    ConfirmCard card = tools.Describe("set_script_running", Input("{\"name\":\"aimbot\",\"running\":true}"));
    Check(card.ok && card.declaredModules == std::vector<std::string>({"Aim"}),
          "the start card of a real script lists its nickel.module declaration");
    world.library.AddScript("dynamic", "local n = 'Dyn'\nlocal m = nickel.module{ name = n }\n");
    world.library.scripts.back().modules.push_back({"Dyn", -1, false});
    card = tools.Describe("set_script_running", Input("{\"name\":\"dynamic\",\"running\":true}"));
    Check(card.ok && card.declaredModules == std::vector<std::string>({"Dyn"}),
          "without a literal name the start card uses the modules the library knows");
}

void ChecksReadPages() {
    World world;
    AssistantTools tools = world.Tools();
    std::string big;
    for (int i = 0; i < 2500; ++i) {
        big += "print(\"line " + std::to_string(i) + " \xc3\xa9\")\n";
    }
    world.library.AddScript("big", big);
    ToolResult result = tools.Run("read_script", Input("{\"name\":\"big\"}"));
    json::Value body = Parse(result.content);
    const int64_t firstEnd = body["end_line"].AsInt64();
    Check(!result.isError && result.content.size() <= nickel::ai::tools::kMaxToolResultBytes && body["truncated"].AsBool() &&
                  body["start_line"].AsInt64() == 1 && firstEnd > 100 && firstEnd < 2500 &&
                  body["next_start_line"].AsInt64() == firstEnd + 1 && body["total_lines"].AsInt64() == 2500 &&
                  body["total_bytes"].AsInt64() == static_cast<int64_t>(big.size()),
          "read_script of a long script: the first page with its line numbers and the totals");

    json::Value write = json::Value::MakeObject();
    write.Set("name", json::Value::MakeString("big"));
    write.Set("source", json::Value::MakeString(body["source"].AsString()));
    write.Set("expected_sha256", json::Value::MakeString(body["sha256"].AsString()));
    const int writes = world.library.writes;
    Check(IsError(tools.Run("write_script", write, true), "partial_read") && world.library.writes == writes &&
                  world.library.files["big"] == big,
          "write_script refuses to replace a script read in part (the cut copy is not written)");
    const ConfirmCard partialCard = tools.Describe("write_script", write);
    Check(!partialCard.ok && partialCard.error == "partial_read", "the card names partial_read too");

    std::string joined = body["source"].AsString();
    bool capped = true;
    int pages = 1;
    for (int64_t next = body["next_start_line"].AsInt64(); next > 0 && pages < 50; ++pages) {
        result = tools.Run("read_script", Input("{\"name\":\"big\",\"start_line\":" + std::to_string(next) + "}"));
        body = Parse(result.content);
        capped = capped && !result.isError && result.content.size() <= nickel::ai::tools::kMaxToolResultBytes &&
                 body["start_line"].AsInt64() == next;
        joined += body["source"].AsString();
        next = body.Find("next_start_line") != nullptr ? body["next_start_line"].AsInt64() : 0;
    }
    Check(capped && pages > 1 && joined == big && body["end_line"].AsInt64() == 2500,
          "the pages cover the whole script, each within 16 KB");
    result = tools.Run("write_script", write, true);
    Check(!result.isError && world.library.files["big"] == write["source"].AsString(),
          "once every page was read, the replace is accepted");

    Check(IsError(tools.Run("read_script", Input("{\"name\":\"big\",\"start_line\":99999}")), "invalid_input"),
          "a start_line after the last line");
    Check(IsError(tools.Run("read_script", Input("{\"name\":\"big\",\"start_line\":0}")), "invalid_input") &&
                  IsError(tools.Run("read_script", Input("{\"name\":\"big\",\"start_line\":1.5}")), "invalid_input") &&
                  IsError(tools.Run("read_script", Input("{\"name\":\"big\",\"start_line\":\"2\"}")), "invalid_input"),
          "start_line is a whole number from 1");

    body = Parse(tools.Run("read_script", Input("{\"name\":\"held one\"}")).content);
    Check(body.Find("truncated") == nullptr && body.Find("next_start_line") == nullptr &&
                  body["total_lines"].AsInt64() == 1 && body["end_line"].AsInt64() == 1,
          "a short script is one page");

    // one line longer than a page: cut, and never counted as read
    const std::string longLine = std::string(20000, 'x') + "\nprint(1)\n";
    world.library.AddScript("long line", longLine);
    result = tools.Run("read_script", Input("{\"name\":\"long line\"}"));
    body = Parse(result.content);
    Check(!result.isError && result.content.size() <= nickel::ai::tools::kMaxToolResultBytes && body["truncated"].AsBool() &&
                  body["end_line"].AsInt64() == 1 && body["next_start_line"].AsInt64() == 2 &&
                  body["source"].AsString().size() > 10000,
          "a line longer than a page is cut");
    tools.Run("read_script", Input("{\"name\":\"long line\",\"start_line\":2}"));
    json::Value replaceLong = json::Value::MakeObject();
    replaceLong.Set("name", json::Value::MakeString("long line"));
    replaceLong.Set("source", json::Value::MakeString("print(2)\n"));
    replaceLong.Set("expected_sha256", json::Value::MakeString(body["sha256"].AsString()));
    Check(IsError(tools.Run("write_script", replaceLong, true), "partial_read"),
          "a script with a cut line cannot be replaced from the cut copy");
}

void ChecksDiffHunks() {
    using nickel::ai::tools::detail::DiffLines;
    std::string before, after;
    for (int i = 0; i < 30; ++i) {
        before += "l" + std::to_string(i) + "\n";
        after += (i == 2 ? "X2" : i == 27 ? "X27" : "l" + std::to_string(i)) + "\n";
    }
    nickel::ai::tools::detail::LineDiff diff = DiffLines(before, after);
    Check(diff.text == "@@ line 1 @@\n l0\n l1\n-l2\n+X2\n l3\n l4\n l5\n"
                       "@@ line 25 @@\n l24\n l25\n l26\n-l27\n+X27\n l28\n l29\n" &&
                  diff.added == 2 && diff.removed == 2 && !diff.truncated,
          "unchanged runs are folded into hunks with three lines of context");

    before.clear();
    after.clear();
    for (int i = 0; i < 3000; ++i) {
        const std::string line = "local value" + std::to_string(i) + " = " + std::to_string(i) + "\n";
        before += line;
        after += (i == 1 ? "-- first change\n" : i == 2990 ? "-- last change\n" : line);
    }
    diff = DiffLines(before, after);
    Check(diff.text.find("+-- first change\n") != std::string::npos &&
                  diff.text.find("+-- last change\n") != std::string::npos && diff.text.size() < 2000 &&
                  !diff.truncated,
          "two changes far apart in a 60 KB script both show");

    before.clear();
    after.clear();
    for (int i = 0; i < 400; ++i) {
        before += "old line number " + std::to_string(i) + "\n";
        after += (i % 8 == 0 ? "new line number " : "old line number ") + std::to_string(i) + "\n";
    }
    const size_t fullSize = DiffLines(before, after, size_t{1} << 30).text.size();
    diff = DiffLines(before, after, 4000);
    Check(fullSize > 8000 && diff.text.size() <= 4000 && !diff.truncated && diff.added == 50 &&
                  diff.text.find("+new line number 0\n") != std::string::npos &&
                  diff.text.find("+new line number 392\n") != std::string::npos &&
                  diff.text.find(" old line number 1\n") == std::string::npos,
          "over the budget, the context is dropped before any changed line");
    diff = DiffLines(before, after, 600);
    Check(diff.text.size() <= 600 && diff.truncated && diff.added == 50 && diff.removed == 50,
          "a diff that cannot fit is cut with the marker and full counts");

    diff = DiffLines("a\nb\nc\nd\n", "a\nX\nb\nd\n");
    Check(diff.text == "@@ line 1 @@\n a\n+X\n b\n-c\n d\n" && diff.added == 1 && diff.removed == 1,
          "an addition and a removal in the same hunk");
    diff = DiffLines("1\n2\n3\n", "1\n3\n2\n3\n");
    Check(diff.added == 1 && diff.removed == 0, "the shortest edit script");
    before.clear();
    after.clear();
    for (int i = 0; i < 1500; ++i) {
        before += "a" + std::to_string(i) + "\n";
        after += "b" + std::to_string(i) + "\n";
    }
    diff = DiffLines(before, after);
    Check(diff.added == 1500 && diff.removed == 1500 && diff.text.compare(0, 17, "@@ line 1 @@\n-a0\n") == 0,
          "a full rewrite is a removed then an added block");

    diff = DiffLines("a\n", std::string(40000, 'y') + "\n");
    Check(diff.truncated && diff.text.size() <= nickel::ai::tools::kMaxDiffBytes && diff.text.size() > 30000 &&
                  diff.text.compare(0, 18, "@@ line 1 @@\n-a\n+y") == 0,
          "a line longer than the diff budget is cut, not left out");
}

void ChecksWriteCardRunning() {
    World world;
    AssistantTools tools = world.Tools();
    json::Value replace = json::Value::MakeObject();
    replace.Set("name", json::Value::MakeString("Auto Totem"));
    replace.Set("source", json::Value::MakeString("print(1)\n"));
    replace.Set("expected_sha256", json::Value::MakeString(FakeSha(world.library.files["Auto Totem"])));
    ConfirmCard card = tools.Describe("write_script", replace);
    Check(card.ok && card.wasRunning && card.lines.size() == 2 && card.lines[1].what == "State" &&
                  card.lines[1].before == "Running" && card.lines[1].after.find("Stopped") == 0,
          "the write card warns that a running script stops");
    replace.Set("name", json::Value::MakeString("held one"));
    replace.Set("expected_sha256", json::Value::MakeString(FakeSha(world.library.files["held one"])));
    card = tools.Describe("write_script", replace);
    Check(card.ok && !card.wasRunning && card.lines.size() == 1, "no warning for a script that is not running");
}

void ChecksUnavailable() {
    World world;
    world.registry.list.push_back(MakeModule("disabler", "Disabler", "Utility", "Disables anticheat checks",
                                             {MakeSetting("mode", "Mode", "enum", 0, 1, 0.01, 0, "A", {"A", "B"})}));
    const int disabler = world.registry.Count() - 1;
    nickel::ai::tools::Deps deps{world.registry, world.library, world.writer, world.ui, world.status, &world.defaults};
    deps.unavailable = [](const std::string& id) { return id == "disabler"; };
    AssistantTools tools(deps);

    json::Value body = Parse(tools.Run("search_modules", Input("{\"query\":\"disabler\"}")).content);
    Check(body["modules"].size() == 1 && body["modules"].At(0).Find("available") != nullptr &&
                  !body["modules"].At(0)["available"].AsBool(),
          "search marks a module that cannot be switched on");
    body = Parse(tools.Run("search_modules", Input("{\"query\":\"aura\"}")).content);
    Check(body["modules"].At(0).Find("available") == nullptr, "other modules carry no mark");
    body = Parse(tools.Run("get_module", Input("{\"id\":\"disabler\"}")).content);
    Check(body.Find("available") != nullptr && !body["available"].AsBool(), "get_module marks it too");

    const json::Value on = Input("{\"module_id\":\"disabler\",\"enabled\":true}");
    const ConfirmCard card = tools.Describe("set_module_enabled", on);
    Check(!card.ok && card.error == "not_available", "no card to turn it on");
    Check(IsError(tools.Run("set_module_enabled", on, true), "not_available") &&
                  !world.registry.list[static_cast<size_t>(disabler)].enabled && tools.Journal().empty(),
          "set_module_enabled refuses it and changes nothing");
    Check(IsError(tools.Run("set_setting", Input("{\"module_id\":\"disabler\",\"setting\":\"mode\",\"value\":\"B\"}"),
                            true), "not_available") &&
                  world.registry.list[static_cast<size_t>(disabler)].settings[0].text == "A",
          "set_setting refuses it too");
    const ToolResult off = tools.Run("set_module_enabled", Input("{\"module_id\":\"disabler\",\"enabled\":false}"), true);
    Check(!off.isError && tools.Journal().empty(), "turning it off (already off) is allowed and changes nothing");
}

} // namespace

int main() {
    ChecksBasics();
    ChecksSearch();
    ChecksGetModule();
    ChecksStatus();
    ChecksScripts();
    ChecksNavigate();
    ChecksSetSetting();
    ChecksEnabledAndRevert();
    ChecksWriteScript();
    ChecksRunScript();
    ChecksSizeCap();
    ChecksDeclaredModules();
    ChecksReadPages();
    ChecksDiffHunks();
    ChecksWriteCardRunning();
    ChecksUnavailable();
    std::printf(failed == 0 ? "all ai tools checks passed\n" : "%d ai tools checks failed\n", failed);
    return failed == 0 ? 0 : 1;
}
