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
| `ui/catalog.js` | Module names, categories and descriptions; `tools/build_native_catalog.py` turns it into `native/generated_catalog.hpp`. |
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
