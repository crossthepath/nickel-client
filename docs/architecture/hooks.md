# Game hooks

How the overlay attaches to game functions safely.

## Rules

- Every address is an RVA into `Minecraft.Windows.exe` of the supported version. `Supported()` checks the image before
  any hook is considered.
- Each `Install…Hook` function verifies the target's prologue bytes, creates the MinHook entry and enables it. On
  unexpected bytes it logs the reason and returns `-1`; the feature stays off instead of patching the wrong code.
- `Install…` functions are idempotent: they return the cached state (`1` installed, `-1` refused) on later calls.
- One function, one hook. Features attach callbacks to a shared detour, for example the game-update detour in
  `game_free_look.hpp` calls the rotation manager, Combat and Discord callbacks in a fixed order.
- Detour bodies call feature code through `__try`/`__except`. A fault disables that feature and is logged once; the
  original function always runs unless the feature explicitly cancels it.

## Main hook points

| Header | Hook | Used by |
| --- | --- | --- |
| `game_free_look.hpp` | Game update | Free Look, rotation manager, Combat, Discord Presence |
| `game_render.hpp` | Screen view render (one call per UI layer) | HUD-layer features, Auto Clicker, Trigger Bot |
| `game_packet_hooks.hpp` | Packet send and the per-type receive dispatchers | Freecam, Utility packet features, Kill Aura teleport |
| `game_lag.hpp` | Serialized packet send | Blink, Fake Lag |
| `game_world.hpp`, `game_map.hpp` | Level render preparation, level tick, block changes | World drawing, MiniMap, Base Finder |
| `game_highlight.hpp`, `game_materials.hpp` | Highlight pass, material loading | Chams, Walls, Outline ESP, Hand Chams, Custom Sky |
| `game_view.hpp` | FOV, bobbing, hurt camera, light, time | View modules |
| `game_filter.hpp` | Actor, particle, weather, fog and foliage rendering | Render filters, Xray |
| `game_player.hpp` | Item use delay | Fast Use, Right Clicker |

## Adding a hook

1. Find the function for the supported version and record its RVA and first bytes.
2. Add an `Install…Hook` next to the feature, verifying those bytes, or reuse an existing detour if the function is
   already hooked.
3. Call it from the module's settings block in `native/menu.cpp` only when the module is enabled.
4. Guard the detour body and keep it cheap: it runs on the game's hot path.
