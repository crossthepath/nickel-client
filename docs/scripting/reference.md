# Nickel Luau scripting API — reference

Verified against the C++ source: `native/script_engine.hpp` (registration in
`RegisterApi`), `native/script_api21.hpp`, `native/script_draw.hpp`, `native/script_canvas.hpp`,
`native/script_events.hpp`, `native/script_modules.hpp`, `native/script_values.hpp`, `native/script_keys.hpp`,
`native/script_limits.hpp`. Type definitions: `nickel.d.luau` (same folder). Points marked **UNSURE** could not be
settled from the code.

Notation: `T?` means the value can be `nil`; a function "returns nothing" when it pushes no value (reads as `nil`).
"Raises" means a Lua error (counts toward the error limit unless caught with `pcall`).

---

## 1. Runtime model

- One Luau VM per `%APPDATA%\Nickel\scripts\<name>.luau`. The folder is scanned about once a second: a new or
  modified file is (re)loaded, a removed file is unloaded. `*.d.luau` files are never run. Any change under `lib/`
  reloads every script.
- Loading = running the file's top level once. **Declarations** (`nickel.module`, `nickel.command`, module settings,
  `mod:hud`, `require`) are only allowed during that run; later calls raise "... can only be declared while the
  script loads".
- Sandbox: Luau standard libraries only (`math`, `string`, `table`, `coroutine`, `os`, `utf8`, `bit32`, `buffer`,
  `vector`, `debug`, base functions), `luaL_sandbox` (read-only globals). No file, network or process access.
  `print` is Luau's own and is not shown in the game (**UNSURE** where its output goes); use `chat.print`.
- Limits (`script_limits.hpp`, `ScriptAlloc`):
  - 50 ms per call into the script (1 s for the load). Once exceeded the error is raised again on every check until
    the outermost call ends, so `pcall` cannot swallow it.
  - 64 MB memory per script; 256 timers; 64 event handlers per module; 128 settings per module; module and setting
    names 48 characters (module: error; setting: truncated).
  - 5 errors within 10 s: the script is stopped ("Stopped after errors") and its modules are disabled until the file
    is saved again.
- Threads: everything runs under one lock (`scriptGuard`), so handlers never run concurrently.
  - Render thread (every frame): `update`, `render2d`, `render3d`, `key`, `screen`, `dimension`, timers,
    `onEnable`/`onDisable`, and `death`/`respawn` from the frame watcher.
  - Game thread: `tick`, `packetSend`, `packetReceive`, `chat`, `containerOpen`, `containerClose`, `teleport`,
    `attack`, `move`, `jump`, `hurt`, `pathEnd`, `death`/`respawn` from the tick watcher, command `run` functions
    (the outgoing-chat hook), `useItem`'s `done` callback (**UNSURE** which thread the action queue completes on).
- **Event handlers run only while their module is enabled.** Timers and commands run while the script runs,
  whatever the module state.

## 2. Values

### Vectors
- Every position/velocity the API returns is a native Luau `vector` (`v.x`, `v.y`, `v.z`; `+ - * /` work;
  `vector.create(x, y, z)`, `vector.magnitude`, `vector.normalize`, `vector.dot`, `vector.cross`, `vector.floor`, ...
  from the standard `vector` library).
- Functions taking a position (`VectorLike`) accept a `vector`, a table `{x = , y = , z = }` or an array `{x, y, z}`
  (named field first, then index). With the lenient reader a missing component reads as 0.
- Strict readers (`player.setVelocity`, `player.addVelocity`, `player.teleport`) require three real numbers (not
  numeric strings), finite, within ±10 000 (velocity) or ±30 000 000 (teleport); otherwise they raise.
- Block positions are returned as vectors of whole numbers. Functions that take a block position floor each component.
- `storage.set` refuses a vector value (raises); store `{x = v.x, y = v.y, z = v.z}`.

### Colours
- A colour argument is a table of numbers 0..1: array `{r, g, b, a}` and/or named `{r = , g = , b = , a = }`
  (named fields override indices). Missing parts default to 1; values are clamped. **Anything that is not a table
  (nil, a string such as "#ff0000", a number) is opaque white.**
- A colour setting's `get()` returns an array `{r, g, b, a}` (indices 1..4 only, no `.r` fields).

### Keys
Names (case-insensitive when read): `A`–`Z`, `D0`–`D9` (digit keys), `F1`–`F12`, `Numpad0`–`Numpad9`, `Space`,
`Enter`, `Escape`, `Tab`, `Shift`, `LeftShift`, `RightShift`, `LeftControl`, `RightControl`, `LeftAlt`, `RightAlt`,
`Up`, `Down`, `Left`, `Right`, `MouseLeft`, `MouseRight`, `MouseMiddle`, `MouseX1`, `MouseX2`. Names the API returns
use this spelling; other keys come back as `"Key<code>"`, no key as `"None"`. An unknown name means "no key".

### Faces
`"Down"`, `"Up"`, `"North"`, `"South"`, `"West"`, `"East"`.

---

## 3. `nickel`

### `nickel.module(options) -> Module` (loading only)
`options`: `name` (string, required, ≤ 48 chars, unique within the script), `category` (required, exactly one of
`"Combat"`, `"Movement"`, `"Player"`, `"Visual"`, `"Utility"`, `"HUD"`; **`"Visual"` puts it in the Visuals tab;
`"Visuals"` raises**), `description` (string), `key` (default key name, used only when no key is saved),
`activation` (`"Toggle"` default, `"Hold"`, `"OneShot"`; anything else raises).
- `Hold`: the module is enabled exactly while its bound key is down and hotkeys are allowed (menu closed, game focused).
- `OneShot`: accepted, but the engine gives it no special behaviour — it acts like `Toggle` (**UNSURE** whether the
  menu treats it differently).
- The module id is `script.<slug(script file name)>.<slug(name)>`; its enabled state, key and settings are saved and
  restored on reload.

### `nickel.command(spec)` (loading only) — returns nothing
`spec`: `name` (letters, digits, `_`, `-`), `description` (≤ 80 chars), `args`, `subcommands`, `run`.
- Typed in chat as `.name args` (the message is not sent). Words are split on spaces; `"double quotes"` group words.
  Nickel's own Schematics and Path Finder chat commands are checked first.
- `args`: array of `{ "argName", "type", {choices}?, optional = bool?, min = n?, max = n? }`. Types: `"string"`
  (default; one word), `"text"` (the rest of the line), `"number"`, `"integer"` (whole number), `"boolean"`
  (`true`/`false`), `"choice"` (third element lists the allowed words). `min`/`max` apply to numbers.
- `subcommands`: `{ [word] = spec }` (same fields minus `name`, nested at most 4 levels). If present, the next word
  must be a subcommand; the parent's `args`/`run` are not used.
- `run(...)` receives the parsed values positionally (numbers, booleans, strings). A missing optional argument stops
  parsing: it and the following arguments are `nil`.
- Errors (missing, wrong type, too many arguments) are printed locally with a usage line; `run` is not called.

### `nickel.modules.list() -> {string}`
Names of every module shown in the menu (built-in and script modules), in menu order.

### `nickel.modules.get(name) -> BuiltinModule?`
First visible module whose display name equals `name` exactly (case-sensitive), e.g. `"Reach Range"`.

`BuiltinModule` methods:
| Method | Returns / effect |
| --- | --- |
| `:name()` | display name |
| `:category()` | menu category; `"Visual"` for the Visuals tab, other names as the menu has them |
| `:isEnabled()` | boolean |
| `:setEnabled(on)` | sets the state (truthiness of `on`); saved when it changes |
| `:toggle()` | flips and saves |
| `:settings()` | `{string}` names of its non-hidden settings |
| `:setting(nameOrId)` | `Setting?` by display name or id |

### `require(name) -> any` (loading only)
Loads `scripts/lib/<name>.luau` once per script (cached) and returns its value. Names: letters, digits, `_`, `-`, `/`.
Raises on a missing file, a compile error or a require loop.

---

## 4. Module object (`nickel.module` result)

### Settings (loading only). Each returns a setting object.
| Declaration | Notes |
| --- | --- |
| `mod:boolean(name, default?)` | default `false` |
| `mod:number(name, default, min, max, step?)` | step default 0.1; min/max swapped if reversed; default clamped |
| `mod:integer(name, default, min, max, step?)` | step default 1, rounded, ≥ 1 |
| `mod:choice(name, options)` | options = `{string}` (at least one); the first is the default. No default argument. |
| `mod:color(name, default?)` | default colour table; nil = white |
| `mod:text(name, default?)` | default `""` |
| `mod:key(name, default?)` | key name; unknown = none |

A saved value replaces the default when it is valid. Setting ids are `slug(name)`: two names with the same slug are one
setting.

Setting object methods (`BooleanSetting`, `NumberSetting`, `ChoiceSetting`, `ColorSetting`, `TextSetting`,
`KeySetting` in the definitions):
- `:get()` — boolean / number / string (choice, text) / `{r, g, b, a}` array (colour) / key name or `"None"` (key).
- `:set(value)` — boolean: truthiness; number: must be a number (raises), rounded when step ≥ 1, clamped; choice:
  must be one of the options (raises); colour: colour table; key: key name; text: string. Saved immediately.
- `:name()` — display name. `:type()` — `"boolean"`, `"number"`, `"integer"`, `"choice"`, `"color"`, `"text"`, `"key"`.
- Settings of other modules (`BuiltinModule:setting`) report the internal kind in `:type()`: `"bool"`, `"number"`,
  `"range"`, `"enum"`, `"color"`, `"gradient"`, `"list"`, `"pattern"`, `"text"`, `"key"`. For kinds other than
  bool/number/color/key, `get()` returns the raw stored text and `set()` stores a string as is. `"range"`:
  `set(low, high?)` sets the low value (number) and, when given, the high one; **UNSURE** what `get()` returns for a
  range (it returns the stored text, apparently the high bound).

### Behaviour
| Method | Returns / effect |
| --- | --- |
| `mod:onEnable(fn)`, `mod:onDisable(fn)` | called (render thread) when the state changes, including on load if saved enabled; replaces a previous function |
| `mod:on(event, fn)` | adds a handler (several allowed, ≤ 64); unknown names raise; names are case-sensitive |
| `mod:isEnabled()` | boolean |
| `mod:setEnabled(on)` | sets and saves |
| `mod:hud(width, height)` | loading only; makes the module a HUD element (1..4000 each), placed in the HUD editor (default at 20, 200) |
| `mod:hudFrame()` | `x, y, scale, width, height` of this frame (width/height already scaled), or five `nil` when the module is not a drawn HUD element this frame |
| `mod:rotate(yaw, pitch, options?)` | not while loading; submits a rotation to Nickel's rotation manager; pitch clamped ±90 |
| `mod:aimAt(target, options?)` | `boolean`; target = position or entity runtime id; false when the target or the player cannot be read |
| `mod:useItem(options?)` | not while loading; queues a use of the held item (see below); returns nothing |

Rotation `options`: `mode` = `"camera"` / `"visible"` / `"silent"` (default) / `"packet"`; `priority` = `"ambient"` /
`"aim"` (default) / `"combat"` / `"action"` / `"view"` (both case-insensitive, unknown raises); `smooth` = number
(speed). The request applies while the module is enabled. **UNSURE**: how long one request stays active and whether
it must be resubmitted every frame/tick.
`aimAt` with an entity id aims at the point of its bounding box closest to the eye (height kept between 20 % and
90 % of the box); `options.predict` (0..1) shifts the box by that fraction of the entity's velocity. Same rotation
options as `rotate`.

`useItem` options: `slot` (1..9, switch to that hotbar slot first, then back), `ghost` (silent switch), `disable`
(turn the module off when done), `waitForAim` (wait up to 400 ms for the module's rotation within `aimTolerance`
degrees, default 6), `switchDelay` / `restoreDelay` (ms, 0..5000), `done(ok)` callback.

---

## 5. Events (`mod:on(name, handler)`)

Plain events call the handler with no argument; others pass one table. Every table has `cancel` — call
`event:cancel()` (or set `event.cancelled = true`). **Returning `true` does nothing.** Cancelling only has an effect
where the table says so.

| Event | Thread | Argument | Cancel |
| --- | --- | --- | --- |
| `update` | render, every frame | none | — |
| `tick` | game, every game tick (after queued chat/player writes are applied) | none | — |
| `render2d` | render | `ScreenDraw` | — |
| `render3d` | render (before render2d) | `WorldDraw` | — |
| `attack` | game, local player attacks | `{target}` runtime id | cancels the attack |
| `packetSend` | game, outgoing packet | `{id, name}` | drops the packet |
| `packetReceive` | game, incoming packet | `{id, name}` | drops the packet |
| `chat` | game, incoming chat (TextPacket with author, non-empty) | `{message, author, type = "Chat"}` | hides the message |
| `key` | render, key pressed with no menu/chat/pause screen | `{key, pressed = true}` | no effect |
| `move` | game, movement input of the local player | `{forward, strafe, jump, sneak, sprint}` **writable** | no effect |
| `jump` | game, before a jump | `{boost = 0}` **writable** | cancels the jump |
| `hurt` | game (tick), health dropped | `{amount, health}` | — |
| `death` / `respawn` | game (tick) and render (frame watcher) | none | — |
| `dimension` | render, dimension changed (not the first read) | `{dimension}` `"Overworld"`/`"Nether"`/`"TheEnd"` | — |
| `screen` | render, screen name changed | `{name, previous}` (e.g. `"hud_screen"`, `"inventory_screen"`) | — |
| `containerOpen` | game, ContainerOpen packet | `{type = "container"}` (always that string) | — |
| `containerClose` | game, ContainerClose packet | none | — |
| `pathEnd` | game (tick), Path Finder stopped being active | none | — |
| `teleport` | game, MovePlayer mode 2 for the local player | `{position, pearl}` (`pearl`: an ender pearl was used within 5 s) | — |

- Packet names: `Text` (9), `MovePlayer` (0x13), `ActorEvent` (0x1b), `InventoryTransaction` (0x1e), `MobEquipment`
  (0x1f), `Interact` (0x21), `PlayerAction` (0x24), `SetActorMotion` (0x28), `Animate` (0x2c), `ContainerOpen`
  (0x2e), `ContainerClose` (0x2f), `CommandRequest` (0x4d), `PlayerAuthInput` (0x90); every other id is `"Unknown"`.
  **There is no packet payload and no way to build or modify packets.**
- `move`: changed fields are written back (forward/strafe clamped to −1..1, booleans replace the flags).
- `jump.boost`: added to the jump. **UNSURE** unit (a float the hook adds at `+0x1c` of its 8th argument).
- `death`/`respawn` come from two watchers (tick health tracking and the per-frame watcher): **UNSURE** — they may
  fire twice for one death.
- **`join`, `leave` and `hit` are accepted by `mod:on` but never dispatched: handlers for them never run.** (They are
  left out of `nickel.d.luau` so the checker rejects them.)
- `key`: releases are not reported. **UNSURE** whether auto-repeat produces repeated events.

---

## 6. Drawing

Coordinates are pixels of the game window, origin top-left, x right, y down; `d:size()` returns width, height.
Drawer methods raise "draw only works inside its render2d/render3d handler" outside the handler that received the
drawer. render2d draws on the HUD layer, which is **hidden outside play** (chat, pause, inventory...); render3d draws
on the background layer every frame.

### `ScreenDraw` (render2d)
| Method | Notes |
| --- | --- |
| `d:text(text, x, y, size?, color?, shadow?, options?)` | size default 16; shadow = 1 px black at 3/4 alpha; `options.font` = `"Regular"` (default) / `"SemiBold"` / `"Bold"` / `"ExtraBold"` (drawn Bold); `options.align` = `"Left"` (default) / `"Center"` / `"Right"` (x is the left edge, centre or right edge; case-sensitive). y is the top of the text. |
| `d:fill(x1, y1, x2, y2, color?, rounding?)` | filled rectangle between two corners; rounding default 0 |
| `d:rect(x1, y1, x2, y2, color?, thickness?)` | outline; thickness default 1 |
| `d:line(x1, y1, x2, y2, color?, thickness?)` | thickness default 1 |
| `d:circle(x, y, radius, color?, filled?, thickness?)` | filled when `filled` is truthy; thickness for the outline |
| `d:gradient(x1, y1, x2, y2, topLeft?, topRight?, bottomRight?, bottomLeft?)` | four corner colours |
| `d:worldToScreen(pos)` | **two numbers `x, y`**, or `nil, nil` (behind the camera, no fresh camera matrix, or `pos` not a vector/table) — never a table |
| `d:size()` | `width, height` |

### `WorldDraw` (render3d) — projected lines, always visible through walls (no depth test)
| Method | Notes |
| --- | --- |
| `d:box(min, max, color?, thickness?)` | 12 edges of the axis-aligned box; raises if min/max are not vectors/tables |
| `d:filledBox(min, max, color?)` | 6 faces (a face is skipped unless its 4 corners are in front of the camera) |
| `d:line(from, to, color?, thickness?)` | segment drawn only if both ends are in front of the camera |
| `d:entityBox(id, color?, thickness?)` | `boolean`: false when the entity or its box was not found |

The camera matrix must be fresh (captured < 250 ms ago); otherwise projections fail silently.

---

## 7. `player`
Reads return `nil` (or the value noted) outside a world.
| Function | Returns |
| --- | --- |
| `inWorld()` | boolean |
| `position()` | `vector?` feet position (eye minus the eye-height offset) |
| `eyePosition()` | `vector?` |
| `velocity()` | `vector?` blocks per tick |
| `rotation()` | `yaw, pitch` (degrees; `0, 0` when unreadable) |
| `anglesTo(pos)` | `yaw, pitch` from the eye to `pos`, or `nil, nil` |
| `health()` | `number?` |
| `name()` | string (`""` when unknown) |
| `runtimeId()` / `actorUniqueId()` | `number?` |
| `onGround()`, `isSprinting()`, `isSneaking()`, `inWater()`, `inLava()` | boolean |
| `fallDistance()` | number (0 when unreadable) |
| `gameMode()` | `"Survival"`, `"Creative"`, `"Adventure"`, `"Spectator"`, `"Default"` or `"Undefined"` |
| `slot()` | selected hotbar slot 1..9 (1 when unreadable) |
| `lookingAt()` | `LookTarget?` `{type = "block"/"entity"/"none", block?, position?, face?}`; block hits have `block` = `position` = block coords and `face`; entity hits have no id |

Requests (queued, applied on the next tick on the game thread; dropped outside a world or if the world changes):
| Function | Notes |
| --- | --- |
| `setSlot(n)` | 1..9 else raises; an empty slot is not selected |
| `setVelocity(v)` / `addVelocity(v)` | strict vector (±10 000); several `addVelocity` in one tick add up |
| `teleport(feet)` | `true` when queued, or `false, "not in a world"`; strict (±3e7); local move like Look TP; servers may refuse long moves |
| `jump()` | returns whether on ground; if so sets vertical velocity to at least 0.42 next tick |
| `swing()` | arm swing |

Immediate actions (run on the calling thread at call time; **UNSURE** whether calling them from render-thread
handlers is safe — `tick` is the conservative choice):
| Function | Returns |
| --- | --- |
| `attack(id)` | `boolean?` (nothing outside a world; false for the local player or unknown id) |
| `interact(id)` | `boolean?` |
| `startBreak(pos, face?)` / `continueBreak(pos, face?)` | `ok, done` booleans, or nothing outside a world; face default `"Up"`; unknown face raises |
| `stopBreak(pos)` | nothing |
| `useItemOn(pos, face?)` | nothing; uses the held item on the block face (default `"Up"`; unknown face silently `"Up"`) |
| `releaseItem()` | nothing (releases a bow/charge) |
| `simulate(input?, ticks?)` | `{SimTick}?` — Nickel's own physics model; `ticks` 1..60 (default 1); input fields `strafe`, `forward` (−1..1), `yaw`, `pitch`, `jump`, `sneak`, `sprint` default to the current input; each tick `{position, velocity, onGround, landed, collided, outcome}` with outcome `"Normal"`, `"Teleport"`, `"Unreliable"`, `"UnloadedChunk"`, `"ImmobileOrNotReady"`, `"Mounted"`, `"InvalidInput"` |

## 8. `world`
| Function | Returns |
| --- | --- |
| `entities(options?)` | `{Entity}` (≤ 512, includes the local player with `isLocal = true`). `options` = `{players = bool, radius = n, type = s}` or a boolean (players only). `radius` measured from the eye. `type`: any non-empty value other than `"player"` only **removes players**; it does not select a mob type. |
| `entity(id)` | `Entity?` by runtime id |
| `target(options?)` | nearest other entity with health ≠ 0 within `range` (default 6) of the eye; `playersOnly` default **true** |
| `dimension()` | `"Overworld"`, `"Nether"`, `"TheEnd"` or `nil` |
| `time()` | always `nil` (not read) |
| `block(pos)` | `Block?` `{name, runtimeId, isAir, solid, liquid, position, states}`; `states` = block-state table (numbers/strings) |
| `raycast(from, to, liquids?)` | `RaycastHit?`: block → `{type = "block", block, face, position}`; entity → `{type = "entity", position}` (no id); miss → `{type = "none"}`; nothing outside a world |
| `isLoaded(pos)` | `boolean?` (chunk present and y inside the world) |
| `findBlocks(name, radius?)` | `{vector}?` block positions (radius 1..12, default 8, at most 256); `name` compared lower-case to the full (`minecraft:stone`) or short name |
| `placeBlock(pos)` | `boolean?` places the held block like Auto Block Reset (reach 5, swings) |

`Entity`: `id` (runtime id), `type` (`"player"` or `"entity.<n>"`, n = numeric actor type & 0xff — **no type names**),
`name?` (players: name tag; other entities: the entity type's translated display name in the game language, e.g.
"Zombie"; absent when empty), `isPlayer`, `isLocal`, `teammate` (always false), `bot` (always false), `health?`,
`position` (feet when the box was read), `eyePosition`, `velocity`, `boxMin?`, `boxMax?`.

## 9. `chat` — queued, sent on the next tick; dropped outside a world; queue ≤ 256
- `chat.print(text)` — local message, not sent.
- `chat.send(text)` — sends a chat message (truncated to 512 bytes).
- `chat.command(text)` — sends a CommandRequest with `text` as the command. **UNSURE** whether the leading `/` is
  required (the client normally sends it with the slash; pass `"/say hi"`).

## 10. `client`
| Function | Returns |
| --- | --- |
| `notify(title, text?)` | Nickel notification (at most 16 pending) |
| `time()` | milliseconds, monotonic (not wall-clock) |
| `tick()` | game ticks counted since Nickel started counting (any script running) |
| `after(ms, fn)` / `every(ms, fn)` | timer id; checked each frame on the render thread; `every` at least 1 ms |
| `cancel(id)` | boolean (found) |
| `isKeyDown(name)` | boolean, only while Minecraft is the foreground window |
| `server()` | `{local = bool, address?, port?}` — `local` is a keyword: read it as `info["local"]` |
| `ping()` | `number?` ms |
| `transport()` | `"RakNet"`, `"NetherNet"` or `"None"` |

## 11. `storage`
Per-script file `%APPDATA%\Nickel\scripts\.storage\<slug>.txt`, read/written on every call.
- `storage.get(key)` — the stored value or `nil`.
- `storage.set(key, value)` — nil (deletes), booleans, numbers, strings and tables of those (≤ 16 levels). Functions,
  vectors (raises "vectors aren't stored...") and other types raise.

## 12. `inventory`
Raises "the inventory is not available" outside a world. Slots are 1-based over the player's inventory (1..9 = hotbar).
| Function | Returns |
| --- | --- |
| `item(slot)` | `Item?` (raises on a bad slot) |
| `size()` | number of slots |
| `held()` / `offhand()` | `Item?` |
| `armor(i)` | `Item?` (i from 1; **UNSURE** order, presumably head, chest, legs, feet) |
| `find(name)` / `hotbarFind(name)` | first slot `number?` whose id matches (full `minecraft:x`, bare `x`, case-insensitive); hotbarFind looks in 1..9 |
| `swap(a, b)` | `ok, result` — moves the whole stack of `a` to `b` (**UNSURE** behaviour when `b` is occupied) |
| `drop(slot, count?)` | `ok, result` (whole stack by default) |
| `equip(slot)` | `ok, result`; `false, "NotArmor"` for non-armour |
| `toOffhand(slot)` | `ok, result` |

`result`: `"Submitted"`, `"Unavailable"`, `"Busy"`, `"InvalidContainer"`, `"InvalidSlot"`, `"InvalidAmount"`,
`"IncompatibleItems"` (and `"NotArmor"`). `Item`: `{id, name, count, durability?, maxDurability?}` — `id` always
namespaced. No enchantments, custom names or NBT.

## 13. `container` (the chest-like window currently open)
| Function | Returns |
| --- | --- |
| `isOpen()` | boolean |
| `type()` | `"None"`, `"Inventory"`, `"Container"`, `"Workbench"`, `"Furnace"`, ... (protocol container types, e.g. `"Hopper"`, `"Anvil"`, `"BrewingStand"`, `"ChestBoat"`, `"Crafter"`), `"Unknown"` |
| `size()` | slot count (0 when none open) |
| `item(slot)` / `items()` | `Item?` / array of `Item?` (has `nil` holes; iterate with a numeric `for i = 1, container.size()`) — both raise when nothing is open |
| `take(slot, count?)` | `ok, result` — whole stack into the first free inventory slot; a partial count returns `false, "InvalidAmount"`; also `"Empty"`, `"Busy"`, `"InventoryFull"` |
| `put(invSlot, count?)` | `ok, result` — whole stack from the inventory into the first empty container slot; `"ContainerFull"` |
| `close()` | nothing; sends ContainerClose |

## 14. `path` (Nickel's Path Finder; requests are queued)
`goto(pos, radius?)`, `follow(playerName, distance?)`, `mine({blockNames}, count?)` (needs at least one name),
`wander(radius?)` (1..4096), `stop()`, `pause()`, `resume()` — all return nothing. `isActive()` → boolean.
`status()` → `{state = "Idle"/"Planning"/"Moving"/"Acting"/"Paused", process = "None"/"Goto"/"Wander"/"Follow"/
"Mine"/"Tunnel"/"Chop"/"Build", detail, remaining}`. The `pathEnd` event fires when it stops being active.
**UNSURE** whether the Path Finder module must be enabled for requests to run.

## 15. `projectile`
Item names: `bow`/`arrow`, `snowball`, `egg`/`blue_egg`/`brown_egg`, `ender_pearl`, `fishing_rod`/`fishing_hook`
(optionally `minecraft:`-prefixed); anything else raises.
- `projectile.trace(item, yaw, pitch)` → `{position, ticks, entity?}?` where it lands (entity = runtime id hit).
- `projectile.aim(item, target, tolerance?)` → `yaw, pitch` that land within `tolerance` blocks (default 1.5) of
  `target`, or nothing.

---

## 16. What does NOT exist
- Events `join`, `leave`, `hit` (accepted, never fire). No `render` / `frame` / `load` events.
- Entity type ids (`type` is only `"player"` or `"entity.<n>"`; `name` gives the display name in the game language, such as "Zombie"), teammate/bot detection (always false), entity armour/held items.
- Packet contents, packet building or editing; only `id`/`name` and cancelling.
- World time (`world.time()` is nil), weather, biome, light level, block hardness, scoreboard, tab list.
- File, HTTP or socket access (Luau has no `io`; `os` has no `execute`); assigning to the read-only globals.
- A drawing API outside `render2d`/`render3d`; images/textures; depth-tested 3D.
- Colour strings (`"#ff0000"`), `Color3`, `Vector3` (use tables and `vector.create`).
- `nickel.modules.get` by id, module creation outside load, `setting:min()`/`max()`.

## 17. Type checking (`nickel.d.luau`)
- Non-strict mode accepts almost everything (it only reports unknown globals). Use `--!strict` to catch wrong calls.
- In strict mode the `mod:on` overloads cannot infer an unannotated handler parameter: annotate it with the payload
  type — `function(d: ScreenDraw)`, `function(d: WorldDraw)`, `function(e: AttackEvent)`, `PacketEvent`,
  `ChatEvent`, `KeyEvent`, `MoveEvent`, `JumpEvent`, `HurtEvent`, `DimensionEvent`, `ScreenEvent`,
  `ContainerOpenEvent`, `TeleportEvent`. Handlers of `update`, `tick`, `death`, `respawn`, `containerClose`,
  `pathEnd` take no parameter. Extern types (`Entity`, `Item`, `Block`, `Module`, settings, ...) can be used in
  annotations; type aliases of the definition file (`VectorLike`, `Color`, options) cannot.

---

## 18. Examples (each passes the checker in non-strict and strict mode)

### 1. Entity ESP label (render2d)

```luau
-- Entity ESP: a label over every entity within a radius.
local esp = nickel.module({ name = "Label ESP", category = "Visual", description = "Names over nearby entities" })
local radius = esp:number("Radius", 32, 4, 128, 1)
local playersOnly = esp:boolean("Players only", false)
local color = esp:color("Color", { 1, 1, 1, 1 })

esp:on("render2d", function(d: ScreenDraw)
  local list = world.entities({ players = playersOnly:get(), radius = radius:get() })
  for _, e in list do
    if not e.isLocal then
      local top = e.boxMax or e.eyePosition
      local x, y = d:worldToScreen(top + vector.create(0, 0.5, 0))
      if x and y then
        local label = e.name or e.type
        if e.health then
          label = label .. "  " .. math.floor(e.health)
        end
        d:text(label, x, y, 14, color:get(), true, { align = "Center" })
      end
    end
  end
end)
```

### 2. HUD element

```luau
-- HUD element: player coordinates, movable in the HUD editor.
local hud = nickel.module({ name = "Coordinates", category = "HUD", description = "Shows X Y Z" })
hud:hud(160, 24)
local background = hud:color("Background", { 0, 0, 0, 0.5 })

hud:on("render2d", function(d: ScreenDraw)
  local x, y, scale, w, h = hud:hudFrame()
  if not (x and y and scale and w and h) then return end
  d:fill(x, y, x + w, y + h, background:get(), 4 * scale)
  local p = player.position()
  local text = "not in a world"
  if p then
    text = string.format("%d  %d  %d", math.floor(p.x), math.floor(p.y), math.floor(p.z))
  end
  d:text(text, x + 6 * scale, y + 4 * scale, 16 * scale, { 1, 1, 1, 1 }, true)
end)
```

### 3. Chat command

```luau
-- Chat command: ".dist to <x> <y> <z>" prints the distance to a point; ".dist here" saves the position.
nickel.command({
  name = "dist",
  description = "Distance to a point",
  subcommands = {
    to = {
      args = { { "x", "number" }, { "y", "number" }, { "z", "number" } },
      run = function(x, y, z)
        local p = player.position()
        if not p then
          chat.print("Not in a world")
          return
        end
        local d = vector.magnitude(vector.create(x, y, z) - p)
        chat.print(string.format("Distance: %.1f blocks", d))
      end,
    },
    here = {
      run = function()
        local p = player.position()
        if p then
          storage.set("saved", { x = p.x, y = p.y, z = p.z })
          chat.print("Saved")
        end
      end,
    },
  },
})
```

### 4. Packet logger (packetReceive)

```luau
-- Packet logger: counts incoming packets by name and prints a summary every 5 seconds.
local logger = nickel.module({ name = "Packet Counter", category = "Utility" })
local counts: { [string]: number } = {}
local timer: number? = nil

logger:on("packetReceive", function(e: PacketEvent)
  counts[e.name] = (counts[e.name] or 0) + 1
end)

logger:onEnable(function()
  counts = {}
  timer = client.every(5000, function()
    for name, n in counts do
      chat.print(name .. ": " .. n)
    end
    counts = {}
  end)
end)

logger:onDisable(function()
  if timer then
    client.cancel(timer)
    timer = nil
  end
end)
```

### 5. Toggle module reacting to attack

```luau
-- Toggle module reacting to attack: blocks hits on players named in a list, notifies on others.
local guard = nickel.module({ name = "Friend Guard", category = "Combat", key = "G" })
local friends = guard:text("Friends", "Steve,Alex")

guard:on("attack", function(e: AttackEvent)
  local target = world.entity(e.target)
  if target and target.isPlayer and target.name then
    for friend in string.gmatch(friends:get(), "[^,]+") do
      if friend == target.name then
        e:cancel()
        chat.print("Not hitting " .. friend)
        return
      end
    end
  end
end)

guard:onEnable(function() client.notify("Friend Guard", "On") end)
guard:onDisable(function() client.notify("Friend Guard", "Off") end)
```

### 6. Config-style module using built-in modules

```luau
-- Config-style module: turns built-in modules on with chosen settings, restores them when turned off.
local preset = nickel.module({ name = "PvP Preset", category = "Utility", description = "Applies a PvP setup" })
local names = { "Reach Range", "Nametags" }
local before: { [string]: boolean } = {}

preset:onEnable(function()
  for _, name in names do
    local m = nickel.modules.get(name)
    if m then
      before[name] = m:isEnabled()
      m:setEnabled(true)
    else
      chat.print("No module named " .. name)
    end
  end
  local nametags = nickel.modules.get("Nametags")
  if nametags then
    for _, settingName in nametags:settings() do
      local s = nametags:setting(settingName)
      if s then
        chat.print(settingName .. " (" .. s:type() .. ") = " .. tostring(s:get()))
      end
    end
  end
end)

preset:onDisable(function()
  for name, was in before do
    local m = nickel.modules.get(name)
    if m then m:setEnabled(was) end
  end
  before = {}
end)
```
