#pragma once
// What a Luau script does to Nickel's modules, without Lua, Windows or the menu (tests/script_host_checks.cpp).
//
// The Luau bindings in script_engine.hpp read the arguments of module(), module:boolean(), module:setEnabled(),
// modules.get() and so on, then call these functions with a ModuleRegistry. Every rule about script modules lives
// here: their ids, their categories, how a reloaded script finds its module again with its saved state and key, how
// a redeclared setting keeps its value, and when a change is saved.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "module_registry.hpp"

namespace overlay {

// Lower-case letters and digits; any run of other characters becomes one '_' ("My Module!" -> "my_module").
inline std::string Slug(const std::string& name) {
    std::string out;
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (!out.empty() && out.back() != '_') {
            out += '_';
        }
    }
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    return out.empty() ? "x" : out;
}

// The menu category for the category a script names, or nullptr when it is not one of the six.
inline const char* ScriptMenuCategory(const std::string& category) {
    static const char* const kCategories[][2] = {
            {"Combat", "Combat"},
            {"Movement", "Movement"},
            {"Player", "Player"},
            {"Visual", "Visuals"},
            {"Utility", "Utility"},
            {"HUD", "HUD"},
    };
    for (const auto& pair : kCategories) {
        if (category == pair[0]) {
            return pair[1];
        }
    }
    return nullptr;
}

// The category name scripts see for a menu category ("Visuals" is "Visual" for scripts).
inline std::string ScriptCategoryName(const std::string& menuCategory) {
    return menuCategory == "Visuals" ? "Visual" : menuCategory;
}

// The id of module `module` declared by script `script`; it is also its section in the preferences file.
inline std::string ScriptModuleId(const std::string& script, const std::string& module) {
    return "script." + Slug(script) + "." + Slug(module);
}

// Declares a script module, or takes back the one with the same id (the script was reloaded, or the module was hidden
// when its script stopped). A new module gets its saved enabled state and key, or the script's default key
// (`defaultKey`, shown as `defaultKeyName`; 0 for none). Name, category and description are set again and the
// settings are cleared: the script declares them again. Returns the module's index, or -1 when the registry is full.
inline int DeclareScriptModule(ModuleRegistry& registry,
                               const std::string& id,
                               const std::string& name,
                               const char* menuCategory,
                               const std::string& description,
                               const std::string& defaultKeyName,
                               int defaultKey) {
    int index = registry.Find(id);
    if (index < 0) {
        index = registry.Add(id);
        if (index < 0) {
            return -1;
        }
        registry.At(index).enabled = registry.ReadPreferenceInt(id, "enabled", 0) == 1;
        const std::string savedName = registry.ReadPreference(id, "keybind", 64);
        const int savedKey = registry.ReadPreferenceInt(id, "keycode", 0);
        if (!savedName.empty() && savedKey > 0) {
            registry.BindKey(id, savedName, savedKey);
        } else if (defaultKey != 0) {
            registry.BindKey(id, defaultKeyName, defaultKey);
        }
    }
    Module& module = registry.At(index);
    module.name = name;
    module.category = menuCategory;
    module.description = description;
    module.settings.clear();
    return index;
}

inline constexpr size_t kMaxScriptSettings = 128;

// Adds `setting` to `module`, or replaces the setting with the same id. A replaced setting keeps its value when it
// still fits: same kind, and for a choice the old choice is still an option; a number is clamped to the new range.
// Returns the setting's index, or -1 when the module already has kMaxScriptSettings settings.
inline int PutScriptSetting(Module& module, const Setting& setting) {
    int index = -1;
    for (int i = 0; i < static_cast<int>(module.settings.size()); ++i) {
        if (module.settings[i].id == setting.id) {
            index = i;
        }
    }
    if (index < 0) {
        if (module.settings.size() >= kMaxScriptSettings) {
            return -1;
        }
        module.settings.push_back(setting);
        return static_cast<int>(module.settings.size()) - 1;
    }
    Setting& old = module.settings[index];
    const bool keep =
            old.kind == setting.kind
            && (setting.kind != "enum"
                || std::find(setting.options.begin(), setting.options.end(), old.text) != setting.options.end());
    Setting fresh = setting;
    if (keep) {
        fresh.number = setting.kind == "number" ? std::clamp(old.number, setting.minimum, setting.maximum) : old.number;
        fresh.text = old.text;
    }
    old = fresh;
    return index;
}

// Gives `setting` of module `moduleId` its saved value, when one is saved and valid for the setting: a finite number
// within the range for numbers and booleans, "#RRGGBBAA" for colours, one of the options for choices.
inline void ApplySavedSetting(const ModuleRegistry& registry, const std::string& moduleId, Setting& setting) {
    const std::string text = registry.ReadPreference(moduleId, setting.id, 8191);
    if (text.empty()) {
        return;
    }
    if (setting.kind == "number" || setting.kind == "bool") {
        char* end = nullptr;
        const double value = std::strtod(text.c_str(), &end);
        if (end != nullptr && *end == '\0' && std::isfinite(value) && value >= setting.minimum
            && value <= setting.maximum) {
            setting.number = value;
        }
        return;
    }
    const bool valid = setting.kind == "color"
                               ? text.size() == 9 && text[0] == '#'
                               : setting.kind != "enum"
                                         || std::find(setting.options.begin(), setting.options.end(), text)
                                                    != setting.options.end();
    if (valid) {
        setting.text = text;
    }
}

// Turns module `index` on or off; saves only a change.
inline void SetModuleEnabled(ModuleRegistry& registry, int index, bool on) {
    Module& module = registry.At(index);
    if (module.enabled != on) {
        module.enabled = on;
        registry.SaveModule(index, nullptr);
    }
}

// Flips module `index` and saves it.
inline void ToggleModule(ModuleRegistry& registry, int index) {
    Module& module = registry.At(index);
    module.enabled = !module.enabled;
    registry.SaveModule(index, nullptr);
}

// Hides module `index` from the menu and from scripts (an unloaded script's module; it is never removed).
inline void HideModule(ModuleRegistry& registry, int index) {
    registry.At(index).category.clear();
}

// The names of the modules shown in the menu, in menu order (modules.list()).
inline std::vector<std::string> VisibleModuleNames(ModuleRegistry& registry) {
    std::vector<std::string> names;
    for (int i = 0; i < registry.Count(); ++i) {
        const Module& module = registry.At(i);
        if (!module.category.empty()) {
            names.push_back(module.name);
        }
    }
    return names;
}

// The index of the first module shown in the menu whose name is `name`, or -1 (modules.get()).
inline int FindVisibleModule(ModuleRegistry& registry, const std::string& name) {
    for (int i = 0; i < registry.Count(); ++i) {
        const Module& module = registry.At(i);
        if (!module.category.empty() && module.name == name) {
            return i;
        }
    }
    return -1;
}

// The index of the first setting of `module` that scripts can see (not "hidden") named `name` or with id `name`, or -1.
inline int FindVisibleSetting(const Module& module, const std::string& name) {
    for (int i = 0; i < static_cast<int>(module.settings.size()); ++i) {
        const Setting& setting = module.settings[i];
        if (setting.kind != "hidden" && (setting.name == name || setting.id == name)) {
            return i;
        }
    }
    return -1;
}

} // namespace overlay
