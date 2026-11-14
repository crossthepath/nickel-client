# Modules

How a module is declared, configured and run.

## Catalog

Modules and their settings are declared in `native/generated_catalog.hpp` (id, name, description, category, and each
setting's kind, bounds, default and visibility rule). This file is the catalog; `ui/catalog.js` is written from it by
`python tools/sync_catalog.py`, and the `catalog_sync_checks` test fails when the two differ. The menu builds every card and settings panel from it; settings persist in
`module-preferences.ini` next to the overlay and in named configs.

## Lifecycle

1. **Menu frame** (`native/menu.cpp`): each module has a block that reads its settings with `Find`/`Flag`, copies them
   into the feature's shared state under its mutex, resets that state when the module is toggled, and installs the
   hooks the module needs.
2. **Game thread**: the feature runs from a hook callback (game update, tick, screen render, packet send or receive)
   using its snapshot of the settings.
3. **Pause**: while the Nickel menu is open, modules that click, attack or rotate the view do nothing
   (`game::menuPause`).

## Shared building blocks

| Header | Provides |
| --- | --- |
| `game_read.hpp`, `game_events.hpp` | Safe memory reads, the local player context, entity components |
| `game_actions.hpp` | The action queue: slot changes, item use, swings, block placement |
| `game_rotation.hpp` | The rotation manager: prioritized rotation requests, silent rotations, reached callbacks |
| `game_combat.hpp` | Click scheduling, the shared target filter, attack transactions |
| `game_inventory.hpp` | Inventory transactions and item stack requests |
| `game_visual_entities.hpp` | Entity snapshots for Visuals, team and bot rules |

## Adding a module

1. Add it to `native/generated_catalog.hpp`, then run `python tools/sync_catalog.py` and update the counts it asks
   for in `desktop/Models.cs`.
2. Put the behavior in `native/game_<module>.hpp`, with its shared state behind a mutex.
3. Add the settings block in `native/menu.cpp` and install only the hooks it needs.
4. Test it in a local world or on a local server before marking it done.
