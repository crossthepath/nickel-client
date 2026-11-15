# The Luau script engine and its boundaries

The overlay runs Luau scripts from `%APPDATA%\Nickel\scripts`. The engine is being separated from the in-game menu
step by step, so that it can be tested without Minecraft and so that other components (installing community scripts
and configs, an assistant that manages modules and scripts) can use modules and scripts without going through the menu.

## Layers

| Layer | Files | Knows about |
| --- | --- | --- |
| Module data | `generated_catalog.hpp` (`Module`, `Setting`) | nothing |
| Module registry interface | `module_registry.hpp` (`ModuleRegistry`) | module data |
| Script host interface | `script_host.hpp` (`ScriptHost` : `ModuleRegistry`) | the registry, plus keys, notifications, log |
| Script module rules | `script_modules.hpp` | a `ModuleRegistry`; no Lua, no Windows, no menu |
| Event names | `script_events.hpp` | nothing |
| Engine | `script_engine.hpp`, `script_api21.hpp` | the host, the rules, Lua, the game (`game::`), ImGui for drawing |
| Engine interface for the menu and hooks | `script_engine_api.hpp` | the engine's entry points only |
| Menu's host | `menu_script_host.hpp` (`MenuScriptHost`) | the menu's globals |
| Menu | `menu.cpp`, `script_ui.hpp` | everything above |

- The menu reaches the engine only through `script_engine_api.hpp`.
- The engine reaches the module list, the preferences file, the key bindings, the keyboard, notifications and the log
  only through `ScriptHost`. `InitializeMenu` passes `MenuScriptHostInstance()` to `SetScriptHost` before any script
  runs.
- What a script does to modules is defined once, in `script_modules.hpp`:
  - module ids `script.<script>.<module>`;
  - the six categories (Visual is Visuals in the menu);
  - a reloaded script taking back its module with its saved state and key;
  - a redeclared setting keeping its value;
  - saved values being validated;
  - a module being enabled and saved.

  `tests/script_host_checks.cpp` checks these rules on a fake host.

## Keeping the boundaries

`tools/check_boundaries.py` (CTest `boundary_checks`) counts the menu names used by the engine files and the engine
names used by the menu files (`tools/boundaries.json`). A count may only go down; see
`docs/contributing/refactoring.md`.

| Boundary | Before #25 | After #25 | After ScriptHost part 1 |
| --- | --- | --- | --- |
| Menu → engine internals | 5 | 0 | 0 |
| Engine → menu names | 158 | 158 | 66 |
| Scripting tab → engine internals | 28 | 28 | 28 |

Five of the 92 names removed by ScriptHost part 1 were a local variable in `ScanScripts` that shadowed the menu's
`selected`. It was renamed to `kept`. The real engine-to-menu uses went from 153 to 66.

## What remains

- **Drawing.** The engine still draws with ImGui types and the menu's text helpers (about 60 names). The next step is
  a `ScriptCanvas` interface for drawing and a host call for HUD placement.
- **The Scripting tab.** It still reads the engine's script list. The step after that gives it a read-only snapshot.
- **Compiling the engine alone.** The engine is still compiled inside `menu.cpp`. Once drawing and the Scripting tab are
  behind interfaces, it can become its own translation unit and be tested on a fake host with a real Luau VM.
- **Raw game offsets.** Script functions that read the game use raw offsets. They will move behind `game::` accessors,
  so that the 1.26.62/1.26.63 build profiles cover them.
- **The module list in the catalogue header.** `generated_catalog.hpp` also defines the module list itself
  (`modules`). Splitting the data types from the list would let the interfaces include only the types.
