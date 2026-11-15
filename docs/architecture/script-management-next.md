# ScriptHost part 4: what PUBLIC and Nickel AI need from ScriptLibrary

Status: parts 4a (`Compile`) and 4b (held scripts and the write contract) are implemented on branch
`scripts/script-host-4`, on top of pull request #28 (`script-management.md`). Part 3 of section 10 (the engine's own
translation unit and the VM test) is not done yet.

Two future components will write scripts into `%APPDATA%\Nickel\scripts` without the menu:

- **PUBLIC** installs community scripts (`auth-and-security.md` §6.5, `plan.md` C3 in the PUBLIC repository).
- **Nickel AI** writes and fixes scripts for the player (`tools.md`: `validate_script`, `read_script`,
  `write_script`, `set_script_running`).

A cross-check of both designs found four gaps in #28's interface. The first is blocking for both components:

1. **No "installed, stopped" state.** Today any file in `scripts\` runs within a second, and `WriteSource` loads what
   it writes. PUBLIC requires that an installed script "must not start by itself" across restarts. The AI's
   `write_script` promises that "saving does not run it".
2. **No compile entry point.** `validate_script` needs Luau compile errors without a VM.
3. **No write contract.** There is no create versus replace, no revision check against a file the player edited
   meanwhile, no result codes, and no backup. A backup written as a `.luau` file through `WriteSource` would run.
4. **No `Remove`.** PUBLIC uninstall and revocation need one.

This page specifies the additions, the exact header change, the thread rules, the tests, the boundary impact, the
migration, the risks and how the work splits into pull requests.

## 1. Held scripts: "installed, not started"

### Where the state is stored

The engine keeps a list of **held** names in `scripts\.nickel\held.txt`: UTF-8, one script name per line, compared
without case (`SameScriptName`). A held script is never loaded, and therefore never runs, until something calls
`Start` for it.

Why this file:

- **One place for every writer.** PUBLIC, the AI and the Scripting tab all get the same rule. The engine reads no
  component's private file.
- **PUBLIC keeps only provenance.** `public\installed.json` keeps `item_id`, `version`, `sha256` and `file`. The
  `started` field proposed in §6.5 is dropped, because `ScriptInfo::held` already answers that question. Keeping both
  would let them disagree. The PUBLIC docs must be amended to match (see §8).
- **Inside the scripts folder, outside the scan.** The folder scan only lists `scripts\*.luau` and
  `scripts\lib\*.luau`, so `.nickel\` is never run. A script's state travels with the folder if the player moves it.
  The engine already keeps `.storage\` there.
- **Testable.** The VM test planned in `script-management.md` gives the engine a temporary scripts folder, and the
  list lives inside it.

The format is a plain line list rather than JSON because it holds one field. The engine has no JSON parser, and a list
of names cannot be half-understood.

### Who sets and clears it

| Event | Held after it |
| --- | --- |
| `WriteSource` / `RestoreBackup` through the library (PUBLIC install or update, AI save) | yes. The name is added and saved **before** the file is moved in, under `scriptGuard`, so the scan never sees the new file unheld. |
| `Hold(name)` (PUBLIC revocation, a future "keep stopped" button) | yes |
| `Start(name)`: the tab's Start and Reload buttons, the AI's `set_script_running` after the player confirms | no |
| `Remove(name)` through the library | no (the entry is removed with the file) |
| The user's Stop button | **unchanged.** Not persisted, as today. |
| The user creates, edits or copies a `.luau` file in Explorer or an editor | **unchanged** for unheld scripts: the file runs, and a save reloads it. A held script stays held. |
| The folder scan sees a held file disappear | the entry stays until `Start` or `Remove` |

User-made scripts keep today's behaviour exactly: nothing is added to the list unless a library writer or `Hold` puts
it there. With no `held.txt`, nothing is held.

### What ScanScripts does with it

The scan's per-file decision moves into a pure function, so the rule is tested without Windows or Lua:

```cpp
struct ScanStep { bool unload = false; bool load = false; bool compileOnly = false; };
// isNew: first time the scan sees the file; stampChanged: its time or size changed; libChanged: a lib/ file changed;
// running: Script::state == Running; held: the name is in the held list.
ScanStep DecideScan(bool isNew, bool stampChanged, bool libChanged, bool running, bool held);
```

| | not held (today) | held |
| --- | --- | --- |
| new file | load | compile only |
| file changed | load (clears the user's in-memory Stop, as today) | unload if running, then compile only |
| `lib/` changed | reload if running | nothing |
| nothing changed | nothing | unload if running (a `Hold` from another thread takes effect here, within a second) |

**Compile only** runs `luau_compile` without creating a VM. On success the state is `Stopped`; on a compile error the
state is `FailedToLoad` and the error is in `errors`. Either way the script stays held. This way PUBLIC and the AI see
syntax errors in `List()` right after a write, without running anything. A held script has no modules (it never ran
`module()`), so the tab shows 0 modules.

`Start(name)` removes the name from the list, saves the list, then calls `StartScript` as today.

## 2. Compile

```cpp
std::vector<CompileError> Compile(const std::string& source) const;   // empty: it compiles
```

- It uses the engine's options (`optimizationLevel=1`, `debugLevel=1`), so the messages are the same as at load time.
- It is `luau_compile` only: no `lua_State`, no `scriptGuard`, no host call. Any thread may call it.
- Luau reports one error as bytecode `\0:<line>: <message>`. The pure helper `ParseCompileError` turns that into
  `{line, message}`, with line 0 when no line is given.
- The returned vector has 0 or 1 entries today. It is a vector so that type-check warnings can be added later
  without changing the interface.
- `WithoutUtf8Bom` is applied first, as in `WriteSource`, so that what compiles here is what will load.

The AI's `unknown_api` scan stays on the AI side, because it needs the API list from its knowledge bundle.

## 3. The write contract

### Revisions

A **revision** is the SHA-256 of the file's bytes on disk, as 64 lowercase hex characters. The AI shows it as
`sha256:<hex>`; it adds and strips that prefix itself. `ReadSource` returns the revision together with the text, so
the two always describe the same bytes:

```cpp
struct ScriptSource { std::string text; std::string sha256; };
bool ReadSource(const std::string& name, ScriptSource& source) const;
```

The engine hashes with Windows CNG (`BCryptHash`, as `game::Sha256` in `game_build.hpp` already does). Formatting
and checking hex is pure (`Sha256Hex`, `ValidSha256Hex`).

### WriteSource

```cpp
ScriptFileResult WriteSource(const std::string& name, const std::string& source, const std::string& expectedSha256);
```

`expectedSha256` is the revision the caller last read, or empty for "the file must not exist" (the AI's
`mode: create`, a PUBLIC first install). Steps, under the library's own file mutex `scriptFiles` (not `scriptGuard`,
so the game thread never waits on disk I/O):

1. Check the name with `ValidScriptName`, or accept a name that `List()` already shows (an existing user file may
   have a name that is not valid for new files). Otherwise return `InvalidName`.
2. If the source is larger than 1 MiB after `WithoutUtf8Bom`, return `TooLarge`. Callers keep their own lower limits:
   PUBLIC 200 KB, the AI 64 KB.
3. Read the current file. If it is missing and `expectedSha256` is not empty, or it exists and its revision is not
   `expectedSha256`, return `Conflict`.
4. If the file exists, copy it to the backup (below).
5. Add the name to the held list and save the list (under `scriptGuard`).
6. Write the temporary file `.<name>.<thread>.<n>.write`, then move it over `<name>.luau` with
   `MOVEFILE_REPLACE_EXISTING`. On failure, delete the temporary file, remove the held entry again if this call added
   it, and return `Failed`. Otherwise return `Done`.

The new revision is the SHA-256 of the bytes written. The caller can compute it, or call `ReadSource`. A running
script whose file is replaced this way is unloaded at the next scan and stays held. The AI then asks the player and
calls `Start`, which matches `tools.md`: "reloaded only if the player had it running and confirms".

```cpp
enum class ScriptFileResult { Done, InvalidName, TooLarge, Conflict, NotFound, Failed };
```

`NotFound` is used by `RestoreBackup` when there is no backup.

### Backups

There is one level per script, in `scripts\.nickel\backup\<name>.luau.bak`. The scan never sees it (subfolder, and
not `*.luau`). The engine writes it at step 4 of `WriteSource` and before `Remove`.

```cpp
bool HasBackup(const std::string& name) const;                                   // any thread
ScriptFileResult RestoreBackup(const std::string& name, const std::string& expectedSha256);
```

`RestoreBackup` is `WriteSource(name, <backup bytes>, expectedSha256)`. The file being replaced therefore becomes the
new backup, so a second restore undoes the first. Like every library write, the restored script is held. The
Scripting tab can offer "Restore previous version" when `HasBackup` is true. `HasBackup` is a file-existence check:
the tab calls it only for the selected script, never inside `List()`.

## 4. Remove

```cpp
ScriptFileResult Remove(const std::string& name, const std::string& expectedSha256);
```

`expectedSha256` is required (empty gives `Conflict`), so a file the player edited locally is never deleted without
the caller reading it again. This is PUBLIC's "never overwritten without asking" rule. On a match, the engine copies
the file to the backup, deletes `<name>.luau` and removes the held entry. The script itself is unloaded by the next
scan on the render thread, exactly as when the player deletes the file in Explorer, so `Remove` is safe from any
thread. `RestoreBackup(name, "")` brings it back, held.

## 5. Header change

The change to `native/script_library.hpp` as of #28 (`13b5891`) is below. The engine side (`EngineScriptLibrary`, the
held list, `DecideScan` in `ScanScripts`) follows the same rules.

```diff
@@ struct ScriptInfo {
     std::string name;   // the file name without ".luau", UTF-8; identifies the script (compared without case)
     uint64_t serial = 0; // identity of this entry: a file that disappears and comes back (or is renamed) gets a new one
     std::wstring file;  // full path of the .luau file
     ScriptState state = ScriptState::Stopped;
+    bool held = false;   // installed or written but not started: it does not run until Start (scripts\.nickel\held.txt)
     std::vector<ScriptModuleInfo> modules;
@@ template <class EngineScript> ScriptInfo DescribeScript(...)
     info.state = ScriptStateOf(script.state);
+    info.held = script.held;
@@ before class ScriptLibrary
+// A Luau compile error. line is 0 when the compiler gave none.
+struct CompileError {
+    int line = 0;
+    std::string message;
+};
+
+// The error Luau puts after the 0 byte of failed bytecode (":12: Expected 'end' ..."), as a CompileError.
+inline CompileError ParseCompileError(const std::string& text);
+
+// The text of a script and its revision: the SHA-256 of its bytes on disk, 64 lowercase hex characters.
+struct ScriptSource {
+    std::string text;
+    std::string sha256;
+};
+
+// 32 bytes as 64 lowercase hex characters; true when `text` is 64 lowercase hex characters.
+inline std::string Sha256Hex(const uint8_t digest[32]);
+inline bool ValidSha256Hex(const std::string& text);
+
+// The result of a write, a restore or a removal.
+enum class ScriptFileResult {
+    Done,        // written, restored or removed
+    InvalidName, // not a name in List() and not ValidScriptName
+    TooLarge,    // more than kMaxScriptBytes after WithoutUtf8Bom
+    Conflict,    // the file is not at the expected revision (or exists when it must not)
+    NotFound,    // RestoreBackup: no backup
+    Failed,      // the disk refused; nothing changed
+};
+constexpr size_t kMaxScriptBytes = 1024 * 1024;
+
+// The held list (scripts\.nickel\held.txt): one name per line; blank lines, invalid names and repeats (ignoring case)
+// are dropped; '\r' is ignored.
+inline std::vector<std::string> ParseHeldList(const std::string& text);
+inline std::string FormatHeldList(const std::vector<std::string>& names);
+
+// What the folder scan does with one file (see docs/architecture/script-management-next.md, section 1).
+struct ScanStep {
+    bool unload = false;
+    bool load = false;
+    bool compileOnly = false;
+};
+inline ScanStep DecideScan(bool isNew, bool stampChanged, bool libChanged, bool running, bool held);
+
@@ class ScriptLibrary
-// The scripts and what can be done with them. List, Folder, ReadSource and WriteSource may be called from any thread
-// (the engine takes its own lock and runs no Lua for them). Start and Stop run the script's Lua (its main chunk, its
-// modules' enable and disable handlers) and change the menu's modules: call them on the render thread only, and
-// never from inside a script callback.
+// The scripts and what can be done with them. Every call except Start and Stop may be made from any thread: they take
+// the engine's locks themselves and run no Lua (a held or removed script is unloaded by the next folder scan, on the
+// render thread). Start and Stop run the script's Lua and change the menu's modules: render thread only, never from
+// inside a script callback. A file written or restored through the library is held: it does not run before Start.
 class ScriptLibrary {
 public:
     virtual std::vector<ScriptInfo> List() const = 0;
     virtual std::wstring Folder() const = 0;
-    virtual bool ReadSource(const std::string& name, std::string& source) const = 0;
+    virtual bool ReadSource(const std::string& name, ScriptSource& source) const = 0;
+    // Luau compile errors of `source` (empty: it compiles), with the engine's options; no VM is created.
+    virtual std::vector<CompileError> Compile(const std::string& source) const = 0;
+    virtual bool HasBackup(const std::string& name) const = 0;
+
+    // ---- files (any thread; written and restored scripts are held)
+    // expectedSha256: the revision last read, or empty when the file must not exist.
+    virtual ScriptFileResult WriteSource(const std::string& name, const std::string& source,
+                                         const std::string& expectedSha256) = 0;
+    // expectedSha256 is required; the file is backed up, then deleted.
+    virtual ScriptFileResult Remove(const std::string& name, const std::string& expectedSha256) = 0;
+    // The backup becomes the file, and the replaced file the backup; expectedSha256 as for WriteSource.
+    virtual ScriptFileResult RestoreBackup(const std::string& name, const std::string& expectedSha256) = 0;
+    // Keeps script `name` stopped across restarts until Start; a running script stops at the next folder scan.
+    virtual bool Hold(const std::string& name) = 0;

     // ---- running (render thread only)
-    // Loads script `name` again from its file and runs it ...
+    // Loads script `name` again from its file and runs it ...; also releases a held script.
     virtual bool Start(const std::string& name) = 0;
-    virtual bool Stop(const std::string& name) = 0;
-    virtual bool WriteSource(const std::string& name, const std::string& source) = 0;
+    virtual bool Stop(const std::string& name) = 0;   // not persisted, as the tab's Stop button today
 };
```

`ToggleScript` is unchanged. The engine's `Script` gains `bool held`, set from the list at each scan.

## 6. Threads

| Call | Thread | Locks taken | Runs Lua |
| --- | --- | --- | --- |
| `List`, `Folder`, `HasBackup` | any | `scriptGuard` briefly (`List`) | no |
| `ReadSource`, `Compile` | any | `scriptGuard` to find the file (`ReadSource`); none (`Compile`) | no |
| `WriteSource`, `RestoreBackup`, `Remove` | any | `scriptFiles` for the whole call, `scriptGuard` only to update the held list | no |
| `Hold` | any | `scriptGuard` (list update and save) | no; the scan unloads |
| `Start`, `Stop` | render thread, not from a script callback | `scriptGuard` | yes |

- The AI's worker thread and PUBLIC's download and revocation worker never call `Start` or `Stop`. They post the
  request to the render thread: the overlay's existing per-frame work queue, or a small `ScriptsLibrary` command queue
  drained at the start of `ScriptsFrame`. Either is fine. The queue is not part of this interface.
- `scriptFiles` is never taken while holding `scriptGuard`, so the two locks cannot deadlock. File I/O never runs
  under `scriptGuard`, so the game thread's packet hooks never wait on the disk. The one exception is the held list:
  a few hundred bytes, written only when it changes.

## 7. Tests

Pure tests, in `tests/script_library_checks.cpp` (no Lua, no Windows):

- `DecideScan`: every row of the table in §1, including "held and running gives unload" and "held and lib changed
  gives nothing".
- `ParseHeldList` / `FormatHeldList`:
  - a round trip;
  - CRLF;
  - blank lines;
  - repeats differing only in case;
  - invalid names dropped;
  - UTF-8 names kept.
- `ParseCompileError`: `":12: msg"`, a message without a line, an empty message, a very large line number.
- `Sha256Hex` / `ValidSha256Hex`: the FIPS 180-2 `"abc"` digest, upper case and 63-character input refused.
- A recording `ScriptLibrary` fake for the callers' logic. The AI's `AssistantTools` and PUBLIC's installer test
  against it in their own pull requests.

Engine tests, on Windows (a new target, `nickel_script_files_checks`, until the engine has its own translation unit;
see below):

- Against a temporary folder: `WriteSource` with `""` on a missing file gives `Done` and the name is held; with `""`
  on an existing file it gives `Conflict`; with a stale revision, `Conflict`.
- The backup appears and `RestoreBackup` swaps.
- `Remove` with a wrong revision leaves the file; with the right one it deletes it and keeps the backup.
- Two threads writing the same name: no lost temporary file, and the last move wins.
- A leading BOM is dropped.

These need the file half of `EngineScriptLibrary` outside `menu.cpp`. Pull request 4b (below) moves those functions
into `native/script_files.hpp`: Windows, no Lua, no menu. It takes the scripts folder as a parameter, and the engine
calls it.

The VM test (`nickel_script_engine_checks`, the last step in `script-management.md`) then covers:

- a held file compiled but not run;
- `Start` releasing it;
- `Hold` stopping a running script at the next scan;
- a compile error shown as `FailedToLoad` while held.

`Compile` gets a Luau-only test in `nickel_script_time_limit_checks`' style: it links Luau.Compiler, compiles valid
source and broken source, and checks the parsed line.

## 8. Boundaries, migration, other documents

**Boundaries.** No count goes up.

- The new engine code uses `ScriptWide`, not the menu's `Wide`/`Utf8`, so engine → menu stays at 4.
- The tab reads `ScriptInfo::held` and calls `HasBackup` and `RestoreBackup` through the interface; Scripting tab →
  engine internals stays at 0.
- Add `held`-list internals (`ScriptHeldNames`, `SaveHeldList`, `DecideScan` callers) and `scriptFiles` to the
  Scripting tab rule at 0.
- Add `native/script_files.hpp` to the engine rule's file list, so it can never use the menu's names.

**Migration.**

- With no `held.txt`, nothing is held and every existing script behaves as today.
- #28's two-argument `WriteSource(name, source)` has no caller yet. It is replaced, not kept beside the new one.
  Its "loads at the next scan" behaviour goes away with it.
- `ReadSource`'s signature changes for the same reason: there is no caller yet.
- A downgrade to a build without held scripts runs held files again. That is acceptable for local builds; release
  notes mention it once.

**Other documents to amend.**

- PUBLIC `auth-and-security.md` §6.5 and `plan.md` C3:
  - the "installed, stopped" state is `scripts\.nickel\held.txt`, owned by the engine;
  - `installed.json` drops `started`;
  - revocation calls `Hold`, then shows the reason from PUBLIC's own record;
  - uninstall calls `Remove` with the hash from `installed.json`;
  - an update calls `WriteSource` with the installed revision, and a `Conflict` is the "edited locally" prompt.
- Nickel AI `tools.md`:
  - script names have no extension and match with `SameScriptName`;
  - `revision` is `sha256:` plus `ScriptSource::sha256`;
  - `mode: create` passes `""`;
  - `write_script` maps `Conflict` to `conflict`, and `InvalidName`/`TooLarge`/`Failed` to their own error codes;
  - "Save" never runs the script;
  - `set_script_running` and `Start`/`Stop` go through the render thread;
  - `validate_script` maps to `Compile`;
  - the AI's stand-in compiler and `ScriptInfo` copy are deleted.

## 9. Risks

- **The held list can disagree with the folder.** A held file renamed in Explorer runs under its new name. A held
  entry left by a file the user deleted keeps a later file of the same name stopped until Start (the tab shows it as
  not started). Both come from explicit user actions, and the held state is a safeguard against starting by itself,
  not a security boundary against the player. PUBLIC's revocation check compares the hashes of files on disk anyway.
- **Time-of-check race with outside editors.** `scriptFiles` orders the library's own writers (PUBLIC against the
  AI). An editor saving between step 3 and step 6 of `WriteSource` is overwritten. That editor's version is lost from
  the folder but is in the backup only if it landed before step 4. The window is a few milliseconds. Locking the file
  against editors would cost more than it buys.
- **One backup level.** Two quick AI saves keep only the second-to-last version. `tools.md` already describes one
  level; the Scripting tab's Restore is the user's undo.
- **Compiling off the render thread.** `luau_compile` keeps no shared mutable state apart from read-only `FFlag`s.
  A test compiles on two threads at once to catch a regression if Luau changes.
- **Held but changed scripts compile on every save.** Compiling is milliseconds for a few hundred KB, and only on
  stamp changes, so the cost stays small.
- **A user who wants a script to stay stopped** still has no persisted Stop: the tab's Stop is in memory, as today. A
  later "Keep stopped" option can call `Hold`. It is left out so that user scripts keep today's behaviour exactly.

## 10. Pull requests, after #28

1. **4a, Compile.** `CompileError`, `ParseCompileError`, `ScriptLibrary::Compile`, and their tests. It is small and
   independent, and unblocks the AI's `validate_script`.
2. **4b, held scripts and the write contract.** The held list, `DecideScan` in `ScanScripts`, `ScriptInfo::held`,
   `Hold`, `Start` releasing, revisions (`ScriptSource`, `ReadSource`), the new `WriteSource` with result codes,
   backups, `RestoreBackup` and `Remove`. The file half moves to `native/script_files.hpp`, with its Windows tests.
   The tab shows "not started" for held scripts and gets "Restore previous version" for the selected one. These go
   together because a write is only safe for PUBLIC and the AI once it is held, and the hold is only reachable
   through a write. Splitting them would leave an interface that starts what it installs.
3. **The engine's own translation unit and the VM test** (`script-management.md`, last section). It covers held
   scripts end to end.

PUBLIC C3 and the AI's `write_script` / `set_script_running` depend on 4b. The AI's `validate_script` depends on 4a.
