# Architecture overview

How the launcher, the overlay and the game fit together, and what each directory owns.

```text
Nickel.exe (desktop/, WPF)
   └─ checks the game build, then loads ──▶ Nickel.Overlay.dll inside Minecraft.Windows.exe
                                               ├─ DXGI Present / ResizeBuffers ── ImGui on DX11 or DX12
                                               ├─ game hooks (MinHook) ────────── ticks, packets, input, rendering
                                               └─ Luau runtime ───────────────── user scripts
```

## Directories

| Directory | Owns |
| --- | --- |
| `desktop/` | The launcher: game detection, loading the overlay, settings, themes, updates, Stream Hide, start with Windows. |
| `native/` | The overlay: renderer hooks, the in-game menu and HUD, every module (`game_*.hpp`), scripting and schematics. |
| `include/nickel/`, `src/` | Game-independent rules shared with the tests: projection, entity and filter rules, camera math. |
| `tests/` | Unit checks for the shared rules and renderer probes (DX11 and DX12). |
| `ui/catalog.js` | Module list for the launcher, written from `native/generated_catalog.hpp` by `tools/sync_catalog.py`. |
| `branding/` | Logos and the [brand guide](../../branding/nickel/README.md). |
| `tools/` | Build scripts. |

## Loading

The launcher only loads into the supported game version and refuses when another client is already loaded. Once
loaded, the overlay hooks the swap chain to draw, then installs game hooks lazily: a hook is placed the first time a
module that needs it is enabled, so an unused feature never touches the game.

## Threads

- **Render thread** (DXGI `Present`): menu, HUD and world drawing; reads settings and hands snapshots to features.
- **Game thread** (game update and tick hooks): features that read or change game state, send packets or rotate the
  player.
- **Background threads**: work that may block, such as the Discord presence pipe and lyric lookups.

See [hooks](hooks.md), [modules](modules.md) and [rendering](rendering.md) for the details.

## Game builds

Nickel supports the game builds listed in `native/game_build.hpp` and `desktop/GameBuilds.cs` (kept identical by
`tools/check_game_builds.py`). A build is recognised by its version, entry point, load-time directories and the
SHA-256 of its code, constant, data, unwind and relocation sections; the launcher also hashes the code loaded in the
game process, and the overlay hashes it again before it installs any hook.

The code is written with the addresses of the reference build, 1.26.62.1. Other builds of the same game code get a
translation profile produced by `tools/port_build.py <reference exe> <new exe> <version>`: it pairs the functions of
both builds, checks that every function holding an address Nickel uses is identical once relative fields are masked,
maps data addresses through the instructions that reference them, and writes `native/generated_build_<version>.hpp`.
At run time `game::Rva()` translates an address to the running build and `game::CodeIs()` checks expected code bytes
(exactly in the reference build, with RIP-relative and rel32 fields masked in a translated one). Every game address in
`native/` must be written as `base+Rva(0x...)`; ctest fails when one is not, or when a translated build does not
cover it. A build whose code differs in a function Nickel uses cannot be translated and needs a real port.
