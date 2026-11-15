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
| Script canvas interface | `script_canvas.hpp` (`ScriptCanvas`, colours, `HudFrame`) | nothing |
| Script drawing | `script_draw.hpp` (text alignment, projection, box edges and faces) | a `ScriptCanvas` |
| Engine | `script_engine.hpp`, `script_api21.hpp` | the host, the rules, the canvas, Lua, the game (`game::`) |
| ImGui canvas | `imgui_script_canvas.hpp` (`ImGuiScriptCanvas`) | ImGui draw lists |
| Script library interface | `script_library.hpp` (`ScriptLibrary`, `ScriptInfo` snapshots) | the registry; no Lua, no Windows, no menu |
| Engine interface for the menu and hooks | `script_engine_api.hpp` | the engine's entry points only, and `ScriptsLibrary()` |
| Menu's host | `menu_script_host.hpp` (`MenuScriptHost`) | the menu's globals |
| Menu | `menu.cpp`, `script_ui.hpp` | everything above |

- The menu reaches the engine only through `script_engine_api.hpp`. The Scripting tab sees the scripts only through
  `ScriptLibrary`: snapshots (`List`), commands (`Start`, `Stop`, `Hold`) and source files (`ReadSource`, `Compile`, `WriteSource`, `RestoreBackup`,
  `Remove`).
  Its design, its thread rules and its future callers (PUBLIC, Nickel AI) are described in `script-management.md`.
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
- The engine draws `render2d` HUDs and `render3d` world overlays on a `ScriptCanvas` that it gets from
  `ScriptHost::Canvas`. It reads the screen size with `ScriptHost::DisplaySize` and places script HUD elements with
  `ScriptHost::PlaceHud`, so the HUD layout editor still moves and scales them. The menu's canvas
  (`MenuScriptCanvas`) is an `ImGuiScriptCanvas` that draws text in Nickel's fonts. Each primitive is the ImDrawList
  call the engine made before, with the same arguments. `tests/script_canvas_checks.cpp` checks that the vertices,
  indices and draw commands are byte for byte the same, and that the canvas allocates nothing more per frame.

## Keeping the boundaries

`tools/check_boundaries.py` (CTest `boundary_checks`) counts the menu names used by the engine files and the engine
names used by the menu files (`tools/boundaries.json`). A count may only go down; see
`docs/contributing/refactoring.md`.

| Boundary | Before #25 | After #25 | After ScriptHost part 1 | After ScriptCanvas | After the script snapshot |
| --- | --- | --- | --- | --- | --- |
| Menu → engine internals | 5 | 0 | 0 | 0 | 0 |
| Engine → menu names | 158 | 158 | 66 | 4 | 4 |
| Scripting tab → engine internals | 28 | 28 | 28 | 28 | 0 |

Five of the 92 names removed by ScriptHost part 1 were a local variable in `ScanScripts` that shadowed the menu's
`selected`. It was renamed to `kept`. The real engine-to-menu uses went from 153 to 66.

The 4 names left after ScriptCanvas are the text conversions `Wide` (2) and `Utf8` (1) and `WriteLog` (1). The engine
no longer uses any ImGui type or function.

The script snapshot step lowered the Scripting tab's count from 28 to 0. The tab rule also lists the engine's loading
functions (`ScriptFolder`, `StartScript`, `StopScript`, `ScanScripts`, `ScriptLoad`, `ScriptUnload`, `ScriptNamed`),
so the tab cannot start calling them again.

## What remains

- **Shared helpers.** `Wide`, `Utf8` and `WriteLog` can move to a small shared header used by both the menu and the
  engine. The engine's file calls use `ScriptWide` from `script_files.hpp`. `WriteLog` is only the logger passed to
  `game::InstallScreenHook`, which could become a host call.
- **Compiling the engine alone.** The engine is still compiled inside `menu.cpp`. Drawing is behind `ScriptCanvas`
  and the Scripting tab is behind `ScriptLibrary`. The engine can now become its own translation unit and be tested on
  a fake host with a real Luau VM. `script-management.md` gives a plan in four steps.
- **Removing scripts.** `ScriptLibrary` has no `Remove` yet; PUBLIC's uninstall will need it.
- **Raw game offsets.** Script functions that read the game use raw offsets. They will move behind `game::` accessors,
  so that the 1.26.62/1.26.63 build profiles cover them.
- **The module list in the catalogue header.** `generated_catalog.hpp` also defines the module list itself
  (`modules`). Splitting the data types from the list would let the interfaces include only the types.
