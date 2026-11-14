# Nickel — brand guide

A dark, quiet interface with an animated "technical drawing" identity.

## Name

Always **Nickel** (product name: "Nickel Client"). Data folder: `%APPDATA%\Nickel`.

## Mark — "the folded crystal"

A nickel crystal made of four folded-paper facets: three white ones (100 / 82 / 55 % opacity) and an orange top-left
facet (gradient `#FF9348 → #F26A1E → #E8590F`). Drawn on a 48-unit grid.

| File | Use |
| --- | --- |
| `logo/nickel-mark.svg`, `nickel-mark-1024.png` | colour mark on dark backgrounds (menu, launcher sidebar) |
| `logo/nickel-mark-mono.svg` | white mark alone (on orange or on images) |
| `logo/nickel-tile.svg`, `nickel-icon-16…1024.png`, `nickel.ico` | application icon: orange gradient tile with construction lines |
| `logo/nickel-logo-horizontal*.png` | mark + "Nickel" (Satoshi Medium): dark, transparent, white (mono) |

## Colours

Interface tokens: frame `#08080B`, background `#0D0D0F`, panels `#0F0F14`, hover `#14141B`, chips `#17171D` /
`#23232C`, rules `#1A1A21`, text `#FFFFFF` / `#9494A0` / `#787884` / `#60606A`, danger `#F87171` on `#2A1518`,
success `#34D399`.

Themes (same table in `native/theme.hpp` and `desktop/Theme.cs`):

| Theme | Pattern | Glow → fill → edges | Accent |
| --- | --- | --- | --- |
| Orange (default) | drawing | `#FF9348 → #E8590F → #B13704 → #7E2602` | `#FF8A3D` |
| Blueprint | blue grid drawing | `#35AEFF → #0A72D6 → #0753A3 → #022B5E` | `#4DABFF` |
| Cutting Mat | cutting mat | `#31C99A → #0E9A6F → #08734F → #033F2A` | `#34D399` |
| Violet | drawing | `#B07CFF → #6E5BFF → #4B3BB0 → #1B1450` | `#B47BFF` |
| Crimson | drawing | `#FF7B6B → #E23A3A → #A61E24 → #520A0E` | `#FF6363` |
| Graphite | blue grid drawing | `#8A8A96 → #45454F → #2A2A33 → #0D0D0F` | `#EBEBF0` |

## Layout

- Black left column (`#08080B`), 32–36 px entries with a 12 px radius, active entry `#14141B`, 11 px uppercase captions.
- On the right: the theme's animated background, a header on top of it (breadcrumb "Nickel / …", large title, subtitle
  with one **highlighted** word: white box, dark accent text), then a floating `#0F0F14` panel with a 20 px radius.
- Lists are rows separated by rules (no cards); modules show a chip with their initial (accent when enabled).
- Numbered sections: "01 · INTERFACE".
- Line icons drawn by Nickel (24-unit grid, 1.7 stroke).

## Motion

The glow breathes (18 s), the rays sweep a few degrees (15–17 s), arcs draw themselves on open (1.4 s), a reticle runs
along the bottom ruler, the cutting-mat reflections slide; theme changes cross-fade (≈ 0.3 s).

## Typography

Satoshi (Fontshare, free licence), weights 400 / 500 / 600 / 700 (`desktop/Assets/Nickel/Satoshi-*.ttf`).
