"""The validated game builds of the launcher (desktop/GameBuilds.cs) and of the overlay (native/game_build.hpp) must
list the same builds: same version and same section addresses and sizes. Run by ctest (game_builds_in_step)."""
import re, sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
cs = (root / 'desktop' / 'GameBuilds.cs').read_text(encoding='utf-8')
hpp = (root / 'native' / 'game_build.hpp').read_text(encoding='utf-8')
validated = cs[cs.index('Validated ='):cs.index('];', cs.index('Validated ='))]
launcher = []
for m in re.finditer(r'new\("([^"]+)",\s*(\d+),\s*(\d+),\s*(\d+),\s*\[(.*?)\]\)', validated, re.S):
    sections = tuple((n, int(a, 16), int(s, 16)) for n, a, s in re.findall(r'new\("(\.[a-z]+)",\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),', m.group(5)))
    launcher.append(((int(m.group(2)), int(m.group(3)), int(m.group(4))), sections))
table = hpp[hpp.index('kGameBuilds[]={'):hpp.index('};', hpp.index('kGameBuilds[]={'))]
overlay = []
for m in re.finditer(r'\{"[^"]+",\{(\d+),(\d+),(\d+)\},\{(.*?)\}\}', table):
    sections = tuple((n, int(a, 16), int(s, 16)) for n, a, s in re.findall(r'\{"(\.[a-z]+)",(0x[0-9a-fA-F]+)u,(0x[0-9a-fA-F]+)u\}', m.group(4) + '}'))
    overlay.append(((int(m.group(1)), int(m.group(2)), int(m.group(3))), sections))
ok = bool(launcher) and launcher == overlay and all(len(s) == 3 for _, s in launcher)
print('launcher', launcher); print('overlay ', overlay)
print('game builds in step' if ok else 'game builds DIFFER between desktop/GameBuilds.cs and native/game_build.hpp')
sys.exit(0 if ok else 1)
