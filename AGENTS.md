# Repository agent instructions

Nickel is a Minecraft: Bedrock Edition client for Windows: a C++ overlay (`native/`) loaded into the official game,
and a WPF launcher (`desktop/`). It targets one game version at a time (currently **1.26.62**). These rules apply to
human contributors and to coding agents alike.

| Read | When |
| --- | --- |
| [Architecture](docs/architecture/overview.md) | How the launcher, the overlay and the game fit together |
| [Game hooks](docs/architecture/hooks.md) | Adding or changing a hook into the game |
| [Modules](docs/architecture/modules.md) | Adding a module, its settings and its menu entry |
| [Rendering](docs/architecture/rendering.md) | Menu, HUD and world drawing |

## Behavior and architecture

- Every game address, offset and vtable slot belongs to the supported game version. Before hooking, check the
  function prologue and refuse to install on unexpected bytes; log the refusal instead of guessing.
- Game code runs on the game's threads. Wrap work called from a hook in `__try`/`__except`, stop the feature on the
  first fault and log it once; never let a fault reach the game. The overlay builds with `/EHa` so locks unwind.
- Share one hook per game function. When a second feature needs the same function, add a callback to the existing
  detour rather than a second MinHook entry.
- Modules that click, attack or turn the view pause while the Nickel menu is open.
- Read settings in the menu thread and hand them to game-thread code as a snapshot; never read ImGui state from a
  game thread.
- Keep shared values in one place: the module catalog (`native/generated_catalog.hpp`, generated from
  `ui/catalog.js`), the theme table (`native/theme.hpp` and `desktop/Theme.cs` stay in sync), the product name.
- Ship original art only. Never commit Mojang or Microsoft assets, other clients' assets, game binaries, captures,
  decompilations, credentials or recordings.

## Code style

- C++20, one feature per `native/game_<feature>.hpp` header where possible; UI code lives in `native/menu.cpp`,
  `native/theme.hpp` and the `hud_*.hpp` headers.
- Write short, clear English comments. The first line of a file says what it owns; important functions and structs get
  one sentence on their role or contract. Explain why, not history: no dates, test logs or session notes.
- Keep files focused; split a header before it grows into an unrelated second feature.

## Build and tests

```sh
git clone --recursive https://github.com/crossthepath/nickel-client.git
cmake -S . -B build -DNICKEL_ENABLE_RENDERER_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
powershell -ExecutionPolicy Bypass -File tools/build_desktop.ps1 -OutputFolder dist/Nickel
```

Reproduce bugs with a test in `tests/` when the logic can run outside the game (projection, filters, camera math).
Features that need the game are checked in a local world or on a local server before they are called done.

## Commits and pull requests

`master` is the default branch. Write commit and PR titles as short imperative English sentences describing the
resulting behavior ("Add Kill Aura teleport mode"), without type prefixes or issue numbers. Keep one change per PR and
describe how to see it in game. Report accurately what was built, tested in game, or left unfinished.
