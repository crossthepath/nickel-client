#pragma once
// What the Luau engine (script_engine.hpp) needs from the program that runs it.
//
// The engine reaches the modules, the preferences, the key bindings, the keyboard, notifications and the log only
// through this interface. The menu provides the implementation used in the game (menu_script_host.hpp); tests provide
// a fake one (tests/script_host_checks.cpp). Drawing (ImGui) is not part of it yet.
// Layers and what remains: docs/architecture/script-engine.md.
#include <string>

#include "module_registry.hpp"

namespace overlay {

class ScriptHost : public ModuleRegistry {
public:
    // ---- input

    // True while key `vk` is held and Minecraft is the foreground window (keys typed in other programs are never
    // reported to scripts).
    virtual bool GameKeyDown(int vk) const = 0;

    // True when module hotkeys may act: the menu is closed and the game shows a screen where keys are allowed.
    virtual bool HotkeysAllowed() const = 0;

    // ---- events from the engine

    // A script module at `index` was unloaded (its category is cleared, so it is hidden): the host forgets any
    // reference to it, such as a selection in the menu.
    virtual void OnModuleRemoved(int index) = 0;

    // A notification for the player (client.notify).
    virtual void Notify(const std::string& title, const std::string& text) = 0;

    // A line for overlay.log.
    virtual void Log(const std::string& line) = 0;
};

} // namespace overlay
