# Rendering

How the overlay draws the menu, the HUD and world-space visuals.

## Swap chain

`native/overlay.cpp` hooks DXGI `Present` and `ResizeBuffers1`. On the first frame it detects whether the game runs
DX11 or DX12 and creates the matching Dear ImGui backend; on resize it releases its render targets first so the game's
resize succeeds. Each frame it starts an ImGui frame, draws the menu, the HUD and world overlays, then presents.

## Menu and HUD

- `native/menu.cpp` draws the in-game menu (Right Shift), the settings panels, Settings, Configs, Scripting and
  Schematics screens, and the HUD elements with their layout editor.
- `native/theme.hpp` holds the six themes, the animated backgrounds and the line icons; `desktop/Theme.cs` mirrors the
  same table for the launcher.
- Fonts are Satoshi, embedded as resources (`native/resources.rc.in`).

## World drawing

World-space features (ESP, nametags, trajectories, block outlines, breadcrumbs…) read the camera captured during the
game's level render preparation and project points with `include/nickel/visual_projection.hpp`. Depth-tested drawing
and highlight effects use the game's own passes (`game_highlight.hpp`, `game_materials.hpp`) rather than a second
renderer.

## Stream Hide

With Stream Hide on, the menu is drawn in a separate window excluded from screen capture, so recordings show the game
without the overlay.
