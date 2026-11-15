// Script modules on a fake host (native/script_modules.hpp, native/script_host.hpp): what scripts do to Nickel's
// modules (declare them, choose a category, add settings, change built-in settings, turn modules on and off) is
// checked here without Minecraft, the menu or Lua. The fake host records saves, key bindings and preference reads.
// Not covered here: the Luau bindings in script_engine.hpp that call these functions (they still need the menu to
// compile); they are checked in game.
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "script_host.hpp"
#include "script_modules.hpp"

namespace {

using namespace overlay;

class FakeHost final : public ScriptHost {
public:
    explicit FakeHost(size_t capacity) : capacity_(capacity) {}

    std::vector<Module> list;
    std::map<std::pair<std::string, std::string>, std::string> preferences;
    std::map<std::string, std::pair<std::string, int>> keys;
    std::vector<std::pair<int, std::string>> saves; // module index, setting id or "enabled"
    std::vector<int> removed;
    std::vector<std::string> notifications;
    std::vector<std::string> logs;
    int adds = 0;
    mutable int preferenceReads = 0;

    int Count() const override { return static_cast<int>(list.size()); }

    Module& At(int index) override { return list.at(index); }

    int Find(const std::string& id) const override {
        int found = -1;
        for (int i = 0; i < Count(); ++i) {
            if (list[i].id == id) {
                found = i;
            }
        }
        return found;
    }

    int Add(const std::string& id) override {
        if (list.size() >= capacity_) {
            return -1;
        }
        ++adds;
        Module fresh{};
        fresh.id = id;
        list.push_back(fresh);
        return Count() - 1;
    }

    void SaveModule(int index, const Setting* changed) override {
        saves.emplace_back(index, changed != nullptr ? changed->id : std::string("enabled"));
    }

    std::string ReadPreference(const std::string& section, const std::string& key, size_t maxLength) const override {
        ++preferenceReads;
        const auto found = preferences.find({section, key});
        return found == preferences.end() ? std::string() : found->second.substr(0, maxLength);
    }

    int ReadPreferenceInt(const std::string& section, const std::string& key, int fallback) const override {
        ++preferenceReads;
        const auto found = preferences.find({section, key});
        return found == preferences.end() ? fallback : std::atoi(found->second.c_str());
    }

    int KeyOf(const std::string& id) const override {
        const auto found = keys.find(id);
        return found == keys.end() ? 0 : found->second.second;
    }

    void BindKey(const std::string& id, const std::string& name, int vk) override { keys[id] = {name, vk}; }

    bool GameKeyDown(int) const override { return false; }

    bool HotkeysAllowed() const override { return true; }

    void OnModuleRemoved(int index) override { removed.push_back(index); }

    void Notify(const std::string& title, const std::string&) override { notifications.push_back(title); }

    void Log(const std::string& line) override { logs.push_back(line); }

    ScriptCanvas& Canvas(CanvasLayer) override { return canvas_; }

    CanvasPoint DisplaySize() const override { return {1280, 720}; }

    HudFrame PlaceHud(const std::string&, float, float) override { return {20, 200, 1}; }

private:
    // Drawing is checked in tests/script_canvas_checks.cpp; here it only has to exist.
    class NullCanvas final : public ScriptCanvas {
    public:
        void Line(CanvasPoint, CanvasPoint, CanvasColor, float) override {}

        void Rect(CanvasPoint, CanvasPoint, CanvasColor, float) override {}

        void FillRect(CanvasPoint, CanvasPoint, CanvasColor, float) override {}

        void FillGradient(CanvasPoint, CanvasPoint, CanvasColor, CanvasColor, CanvasColor, CanvasColor) override {}

        void Circle(CanvasPoint, float, CanvasColor, float) override {}

        void FillCircle(CanvasPoint, float, CanvasColor) override {}

        void FillQuad(CanvasPoint, CanvasPoint, CanvasPoint, CanvasPoint, CanvasColor) override {}

        void Text(CanvasFont, float, CanvasPoint, const char*, CanvasColor) override {}

        float MeasureText(CanvasFont, float, const char*) override { return 0; }
    };

    size_t capacity_;
    NullCanvas canvas_;
};

int failed = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++failed;
    }
}

Module BuiltIn(const char* id, const char* name, const char* category) {
    Module module{};
    module.id = id;
    module.name = name;
    module.category = category;
    return module;
}

Setting Make(const char* id, const char* kind, double number, double minimum, double maximum, const char* text = "") {
    Setting setting{};
    setting.id = id;
    setting.name = id;
    setting.kind = kind;
    setting.number = number;
    setting.minimum = minimum;
    setting.maximum = maximum;
    setting.step = 1;
    setting.text = text;
    return setting;
}

void Names() {
    Check(Slug("My Module!") == "my_module", "Slug: lower case, other characters as one '_'");
    Check(Slug("__Kill  Aura__") == "kill_aura", "Slug: no '_' at either end, runs collapsed");
    Check(Slug("!!!") == "x" && Slug("") == "x", "Slug: never empty");
    Check(ScriptModuleId("My Script", "Auto Totem") == "script.my_script.auto_totem",
          "module id: script.<script>.<module>");

    const char* const scriptNames[] = {"Combat", "Movement", "Player", "Visual", "Utility", "HUD"};
    const char* const menuNames[] = {"Combat", "Movement", "Player", "Visuals", "Utility", "HUD"};
    bool mapped = true;
    for (int i = 0; i < 6; ++i) {
        const char* category = ScriptMenuCategory(scriptNames[i]);
        mapped &= category != nullptr && std::string(category) == menuNames[i];
        mapped &= ScriptCategoryName(menuNames[i]) == scriptNames[i];
    }
    Check(mapped, "the six categories, Visual shown as Visuals in the menu and back");
    Check(ScriptMenuCategory("Visuals") == nullptr && ScriptMenuCategory("combat") == nullptr
                  && ScriptMenuCategory("") == nullptr,
          "other category names are refused (exact spelling)");
}

void Declare() {
    FakeHost host(8);
    host.list.push_back(BuiltIn("kill_aura", "Kill Aura", "Combat"));
    const std::string id = ScriptModuleId("tests", "Saved");
    host.preferences[{id, "enabled"}] = "1";
    host.preferences[{id, "keybind"}] = "K";
    host.preferences[{id, "keycode"}] = "75";

    const int saved = DeclareScriptModule(host, id, "Saved", "Utility", "a module", "J", 'J');
    Check(saved == 1 && host.adds == 1, "a new script module is added after the built-in ones");
    const Module& module = host.At(saved);
    Check(module.name == "Saved" && module.category == "Utility" && module.description == "a module",
          "name, menu category and description are set");
    Check(module.enabled, "its saved enabled state is read back");
    Check(host.KeyOf(id) == 'K' && host.keys[id].first == "K", "its saved key wins over the script's default key");

    const std::string other = ScriptModuleId("tests", "Fresh");
    host.preferences[{other, "keybind"}] = "K"; // a name without a code is not a binding
    const int fresh = DeclareScriptModule(host, other, "Fresh", "Visuals", "", "J", 'J');
    Check(!host.At(fresh).enabled, "a module with nothing saved starts off");
    Check(host.KeyOf(other) == 'J' && host.keys[other].first == "J",
          "the script's default key is bound when none is saved");

    const std::string unbound = ScriptModuleId("tests", "Unbound");
    DeclareScriptModule(host, unbound, "Unbound", "HUD", "", "None", 0);
    Check(host.KeyOf(unbound) == 0 && host.keys.count(unbound) == 0, "no default key: nothing is bound");

    // The script is reloaded: same id, the module is found again, its state and key are kept, its settings cleared.
    host.At(saved).settings.push_back(Make("speed", "number", 1, 0, 2));
    host.At(saved).enabled = false;
    host.keys[id] = {"L", 'L'};
    const int addsBefore = host.adds;
    const int readsBefore = host.preferenceReads;
    const int again = DeclareScriptModule(host, id, "Saved", "Combat", "new text", "J", 'J');
    Check(again == saved && host.adds == addsBefore, "a reloaded script takes back its module (no new entry)");
    Check(host.preferenceReads == readsBefore, "nothing is read again from the preferences on reload");
    Check(!host.At(again).enabled && host.KeyOf(id) == 'L', "the current enabled state and key are kept");
    Check(host.At(again).settings.empty() && host.At(again).category == "Combat"
                  && host.At(again).description == "new text",
          "settings are cleared and category and description updated (the script declares them again)");
    Check(host.saves.empty(), "declaring modules saves nothing");

    FakeHost full(1);
    full.list.push_back(BuiltIn("kill_aura", "Kill Aura", "Combat"));
    Check(DeclareScriptModule(full, ScriptModuleId("tests", "Late"), "Late", "Utility", "", "None", 0) == -1
                  && full.Count() == 1 && full.keys.empty(),
          "a full registry refuses the module and changes nothing");
}

void Settings() {
    Module module{};
    module.id = "script.tests.settings";
    const int speed = PutScriptSetting(module, Make("speed", "number", 1.5, 0, 3));
    Check(speed == 0 && module.settings.size() == 1, "a new setting is added");
    module.settings[speed].number = 2.5;
    PutScriptSetting(module, Make("speed", "number", 1, 0, 2));
    Check(module.settings[speed].number == 2 && module.settings[speed].maximum == 2,
          "a redeclared number keeps its value, clamped to the new range");

    Setting mode = Make("mode", "enum", 0, 0, 1, "Fast");
    mode.options = {"Fast", "Slow"};
    const int modeIndex = PutScriptSetting(module, mode);
    module.settings[modeIndex].text = "Slow";
    PutScriptSetting(module, mode);
    Check(module.settings[modeIndex].text == "Slow", "a redeclared choice keeps a value that is still an option");
    Setting narrowed = mode;
    narrowed.options = {"Fast", "Safe"};
    PutScriptSetting(module, narrowed);
    Check(module.settings[modeIndex].text == "Fast", "a choice that is no longer an option goes back to the default");

    const int label = PutScriptSetting(module, Make("label", "text", 0, 0, 1, "hello"));
    module.settings[label].text = "changed";
    PutScriptSetting(module, Make("label", "color", 0, 0, 1, "#FFFFFFFF"));
    Check(module.settings[label].kind == "color" && module.settings[label].text == "#FFFFFFFF",
          "a setting redeclared with another kind starts from its new default");

    Module crowded{};
    for (size_t i = 0; i < kMaxScriptSettings; ++i) {
        PutScriptSetting(crowded, Make(("s" + std::to_string(i)).c_str(), "bool", 0, 0, 1));
    }
    Check(PutScriptSetting(crowded, Make("one_more", "bool", 0, 0, 1)) == -1
                  && crowded.settings.size() == kMaxScriptSettings,
          "at most 128 settings per module");
    Check(PutScriptSetting(crowded, Make("s5", "bool", 1, 0, 1)) == 5, "an existing setting can still be redeclared");
}

void SavedValues() {
    FakeHost host(4);
    const std::string id = "script.tests.saved";
    auto saved = [&](const char* key, const char* value) {
        host.preferences[{id, key}] = value;
    };
    Setting number = Make("speed", "number", 1, 0, 10);
    saved("speed", "7.5");
    ApplySavedSetting(host, id, number);
    Check(number.number == 7.5, "a saved number in range is applied");
    bool rejected = true;
    for (const char* bad : {"11", "-1", "7.5x", "inf", "nan", " "}) {
        Setting copy = Make("speed", "number", 1, 0, 10);
        saved("speed", bad);
        ApplySavedSetting(host, id, copy);
        if (copy.number != 1) {
            std::printf("     applied bad value '%s'\n", bad);
            rejected = false;
        }
    }
    Check(rejected, "out of range, partly numeric, infinite, NaN or blank numbers are ignored");

    Setting toggle = Make("on", "bool", 0, 0, 1);
    saved("on", "1");
    ApplySavedSetting(host, id, toggle);
    Check(toggle.number == 1, "a saved boolean is applied");

    Setting colour = Make("tint", "color", 0, 0, 1, "#FFFFFFFF");
    saved("tint", "#11223344");
    ApplySavedSetting(host, id, colour);
    Setting shortColour = Make("tint2", "color", 0, 0, 1, "#FFFFFFFF");
    saved("tint2", "#123");
    ApplySavedSetting(host, id, shortColour);
    Check(colour.text == "#11223344" && shortColour.text == "#FFFFFFFF", "colours are applied only as #RRGGBBAA");

    Setting choice = Make("mode", "enum", 0, 0, 1, "Fast");
    choice.options = {"Fast", "Slow"};
    Setting unknown = choice;
    saved("mode", "Slow");
    ApplySavedSetting(host, id, choice);
    unknown.id = "mode2";
    saved("mode2", "Turbo");
    ApplySavedSetting(host, id, unknown);
    Check(choice.text == "Slow" && unknown.text == "Fast", "a saved choice is applied only when it is an option");

    Setting text = Make("label", "text", 0, 0, 1, "default");
    ApplySavedSetting(host, id, text);
    Check(text.text == "default", "nothing saved: the default stays");
    saved("label", "hello");
    ApplySavedSetting(host, id, text);
    Check(text.text == "hello", "saved text is applied");
    Check(host.saves.empty(), "reading saved values saves nothing");
}

void EnableAndFind() {
    FakeHost host(8);
    host.list.push_back(BuiltIn("kill_aura", "Kill Aura", "Combat"));
    host.list.push_back(BuiltIn("script.old.gone", "Gone", "")); // a hidden module of a stopped script
    host.list.push_back(BuiltIn("aim_assist", "Aim Assist", "Combat"));
    host.list[0].settings.push_back(Make("range", "number", 3, 1, 6));
    host.list[0].settings.push_back(Make("custom_keys", "hidden", 0, 0, 1));

    SetModuleEnabled(host, 0, true);
    SetModuleEnabled(host, 0, true);
    Check(host.list[0].enabled && host.saves.size() == 1 && host.saves[0] == std::make_pair(0, std::string("enabled")),
          "turning a module on saves once; asking again saves nothing");
    SetModuleEnabled(host, 0, false);
    Check(!host.list[0].enabled && host.saves.size() == 2, "turning it off saves the change");
    ToggleModule(host, 2);
    ToggleModule(host, 2);
    Check(!host.list[2].enabled && host.saves.size() == 4, "toggle always flips and saves");

    const std::vector<std::string> names = VisibleModuleNames(host);
    Check(names == std::vector<std::string>({"Kill Aura", "Aim Assist"}),
          "modules.list(): visible modules, menu order");
    Check(FindVisibleModule(host, "Aim Assist") == 2 && FindVisibleModule(host, "Gone") == -1,
          "modules.get(): visible modules only");
    Check(FindVisibleSetting(host.list[0], "range") == 0 && FindVisibleSetting(host.list[0], "custom_keys") == -1,
          "a built-in module's settings are found by name or id, hidden ones never");

    HideModule(host, 0);
    Check(host.list[0].category.empty() && FindVisibleModule(host, "Kill Aura") == -1 && host.Count() == 3,
          "a hidden module stays in the list (indices never change) but is no longer visible");
}

} // namespace

int main() {
    Names();
    Declare();
    Settings();
    SavedValues();
    EnableAndFind();
    std::printf("%s\n", failed ? "script host checks FAILED" : "script host checks passed");
    return failed ? 1 : 0;
}
