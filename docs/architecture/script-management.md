# Managing scripts without the menu

The Scripting tab used to draw straight from the Luau engine's script list, under the engine's lock, and to call
its loading functions. ScriptHost part 3 puts a small interface between them, `ScriptLibrary`
(`native/script_library.hpp`). The tab now uses only that interface. Other components that manage scripts will use it
too: installing community scripts (PUBLIC) and an assistant that writes and fixes scripts (Nickel AI). None of them
needs the menu or the engine's internals. For the engine's other boundaries, see `script-engine.md`.

## The snapshot

`ScriptLibrary::List()` returns a `std::vector<ScriptInfo>`, sorted by name. The vector is a copy: it stays valid
after the engine reloads or removes a script, and reading it takes no lock.

| Field | Meaning |
| --- | --- |
| `name` | File name without `.luau`, in UTF-8. It identifies the script; names are compared without case (`SameScriptName`). |
| `serial` | Identity of this entry. A file that disappears and comes back, or is renamed, gets a new serial. |
| `file` | Full path of the `.luau` file. |
| `state` | `ScriptState`: `Stopped`, `Running`, `StoppedAfterErrors`, `FailedToLoad` (the engine's 0 to 3). |
| `modules` | The modules the script declared, each with its name, its `ModuleRegistry` index and whether it is enabled. |
| `errors` | The latest errors, each with its line, oldest first. The engine keeps 8. |
| `memory`, `frameMs` | Bytes used by the script's VM, and its average time per frame. |

`DescribeScript` builds one `ScriptInfo` from the engine's `Script`. It is a template, so the tests can run the same
rule on a test double.

## The commands

| Call | Effect |
| --- | --- |
| `Start(name)` | Loads the file again and runs it: the Start and Reload buttons. A script that was stopped, stopped after errors or failed to load runs again. Its modules keep their saved state and key. A held script is released first. |
| `Stop(name)` | Turns the script's modules off and unloads it. It stays stopped until it is started again or its file is saved. Not persisted. |
| `ReadSource(name, source)` | Reads the file as it is on disk, with its revision (SHA-256 of its bytes). |
| `Compile(source)` | Luau compile errors with their line, without a VM (part 4a). |
| `WriteSource(name, source, expectedSha256)`, `RestoreBackup`, `Remove`, `Hold`, `HasBackup` | The write contract and held scripts (part 4b): see `script-management-next.md`. The file is written to a temporary file of its own (`.<name>.<thread>.<count>.write`), which the folder scan ignores, then moved over the target. A leading UTF-8 byte order mark is dropped, because Luau does not load a file that starts with one. A file written through the library is held: it does not run until `Start`. New names are checked with `ValidScriptName`: 1 to 64 bytes of well-formed UTF-8, no reserved character, no Windows device name (including `COM` and `LPT` followed by a superscript 1, 2 or 3). |
| `Folder()` | The scripts folder: `%APPDATA%\Nickel\scripts`. |

Threads:

- Every call except `Start` and `Stop` may be made from any thread. The engine takes `scriptGuard` where it reads its
  script list or changes the held list, and the file calls take `scriptFiles`; no Lua runs for these calls.
- `Start` and `Stop` run the script's Lua (its main chunk, and its modules' enable and disable handlers), and they
  change the menu's modules. Call them on the render thread only, and never from inside a script callback. A
  component that works on another thread, such as an assistant waiting for a model, writes the file with
  `WriteSource` (the script is then held) and hands `Start` and `Stop` over to the render thread.

The engine's implementation is `EngineScriptLibrary` in `script_engine.hpp`, and `ScriptsLibrary()` in
`script_engine_api.hpp` hands it out. Its file half (revisions, writes, backups, the held list) is in
`script_files.hpp`, tested on a temporary folder by `tests/script_files_checks.cpp`.

Helpers shared by every caller:

- `FindScript` finds a script by name.
- `FindSelectedScript` finds the tab's selection by serial.
- `MatchingScripts` is the tab's search, which ignores case.
- `ToggleScript` is the row button: it stops a running script and starts any other.
- `WellFormedUtf8`, `ValidScriptName` and `WithoutUtf8Bom` check and clean what `WriteSource` receives.

## The Scripting tab

The tab now owns its own state: the search text (`scriptingSearch`) and the selected script's serial
(`scriptingSelected`). Both used to be engine globals. `ScanScripts` used to keep the selection by identity (a
pointer to the engine's `Script`): it followed the script when the list was sorted again, and was cleared when that
script disappeared, whichever tab was shown. The serial gives the same behaviour:

- A script that disappears never has its serial again, so the selection does not come back to a new file of the
  same name.
- A case-only rename also gives a new serial: the engine removes the old entry and adds a new one.

The one difference is when the selection is cleared: the next time the tab draws, instead of at the scan. Nothing
reads the selection in between. Each of the two drawing functions takes one snapshot per frame. A command issued during a
frame shows up in the next frame's snapshot. That means a single frame where a button still shows the old state; in
the old code the row was redrawn from live data within the same frame.

`tools/boundaries.json` now counts 0 engine names in `script_ui.hpp` (it was 28). The rule also lists the engine's
loading functions (`ScriptFolder`, `StartScript`, `StopScript`, `ScanScripts`, `ScriptLoad`, `ScriptUnload`,
`ScriptNamed`), so the tab cannot call them again without the check failing.

## What PUBLIC and Nickel AI will call

- **PUBLIC, installing a community script**: `WriteSource(name, source, "")`; the script is held, and `List()` shows
  its compile errors within about a second. An update passes the installed revision (a `Conflict` means the player
  edited it); uninstalling is `Remove(name, revision)`; revocation is `Hold(name)`.
- **Nickel AI, writing or fixing a script**: `ReadSource` for the current code and revision, `Compile` to validate,
  `WriteSource` with that revision for the corrected code, then `List()` to check the result. Saving never runs the
  script; `Start` and `Stop` act as the buttons do, on the render thread. The script's modules are changed through
  `ModuleRegistry`, as for any other module.
- Details, thread rules and risks: `script-management-next.md`.
- Neither caller needs the menu. In the game they get the library from `ScriptsLibrary()`. In tests they implement
  `ScriptLibrary` with a fake.

## Next step: the engine as its own translation unit

The engine (`script_engine.hpp`, `script_api21.hpp`) is still included inside `menu.cpp`'s anonymous namespace. As a
result, `EngineScriptLibrary` and the Luau bindings are tested only in the game. Once drawing goes through
`ScriptCanvas` (ScriptCanvas branch), the engine uses 4 menu names: `Wide` (2), `Utf8` (1) and `WriteLog` (1).

Plan:

1. **Text and hook.** Replace `Wide` and `Utf8` with engine-owned conversions (`ScriptWide`, added in this step, plus
   a matching `ScriptUtf8`). `WriteLog` is only the logger that `ScriptsFrame` passes to `game::InstallScreenHook` when
   a script first needs the world projection. Make that a host call (for example
   `ScriptHost::WantWorldProjection()`), so that installing a hook stays with the program that owns the hooks. The
   engine then uses 0 menu names.
2. **Game reads behind a header.** The engine also calls `game::` functions (player, world, packets, actions,
   rotations). Collect what it uses in an interface, `script_game.hpp` (`ScriptGame`, or a set of free functions
   declared there), so that a test can link a fake. The raw offsets in the script functions move behind `game::`
   accessors at the same time (see "What remains" in `script-engine.md`).
3. **Its own file.** Create `native/script_engine.cpp` with its own namespace (`nickel::script::engine` or
   `overlay::script`). It includes `lua.h`, `lualib.h`, `luacode.h`, `script_host.hpp`, `script_library.hpp`,
   `script_engine_api.hpp` and the game interface, and defines what `script_engine_api.hpp` declares.
   `script_engine_api.hpp` then becomes a normal header in a named namespace, included at the top of `menu.cpp` and
   `overlay.cpp` instead of inside the anonymous namespace. Add the new `.cpp` to `nickel_overlay`. Check the move
   with `tools/same_tokens.py`: a new `.cpp` cannot be compared token by token with a revision that lacks it, so first
   move the code unchanged into a header included at the same place, then turn that header into the `.cpp`.
4. **A real VM on a fake host.** Add a test target, `nickel_script_engine_checks`, that links `script_engine.cpp`,
   Luau.VM and Luau.Compiler with the fake host from `tests/script_host_checks.cpp`, a recording `ScriptCanvas` and
   a fake game. It writes `.luau` files to a temporary folder (the folder comes from the host, not from `%APPDATA%`).
   Then it checks:
   - load, state and errors through `ScriptsLibrary().List()`;
   - `Start` and `Stop`;
   - five errors stopping a script;
   - reload after `WriteSource`;
   - `module()` declaring a registry module;
   - `module:on("update")` setting the event mask;
   - draw calls reaching the canvas.

   Step 4 needs the scripts folder to come from the host: `ScriptHost::ScriptFolder()`, or a parameter of
   `SetScriptHost`.

Steps 1 and 3 are mostly moves. Step 2 is the real work, because the engine and `script_api21.hpp` use a large part of
`game::`.
