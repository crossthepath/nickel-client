"""The validated game builds of the launcher (desktop/GameBuilds.cs) and of the overlay (native/game_build.hpp) must
list the same builds: version, entry point, data directories, section addresses and sizes, and the SHA-256 of the
code section. Run by ctest (game_builds_in_step)."""
import re
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
cs = (root / 'desktop' / 'GameBuilds.cs').read_text(encoding='utf-8')
hpp = (root / 'native' / 'game_build.hpp').read_text(encoding='utf-8')
NL = chr(10)


def num(text):
    return int(text.rstrip('u'), 0)


block = cs[cs.index('Validated ='):cs.index(' ];', cs.index('Validated ='))]
launcher = []
for m in re.finditer(r'new\("[^"]+",\s*(\d+),\s*(\d+),\s*(\d+),\s*(0x[0-9a-fA-F]+),\s*\[(.*?)\],\s*\[(.*?)\]\)', block, re.S):
    dirs = tuple((int(i), num(a), num(s)) for i, a, s in re.findall(r'new\((\d+),\s*(0x[0-9a-fA-F]+|0),\s*(0x[0-9a-fA-F]+|0)\)', m.group(5)))
    secs = re.findall(r'new\("(\.[a-z]+)",\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),\s*"([0-9a-fA-F]{64})"\)', m.group(6))
    text = next((h for n, _, _, h in secs if n == '.text'), '').lower()
    launcher.append(((int(m.group(1)), int(m.group(2)), int(m.group(3))), num(m.group(4)), dirs,
                     tuple((n, num(a), num(s)) for n, a, s, _ in secs), text))

start = hpp.index('kGameBuilds[]={')
table = hpp[start:hpp.index(NL + '};', start)]
overlay = []
for entry in table.split(NL + ' {"')[1:]:   # one profile per entry
    head = re.match(r'[^"]+",\{(\d+),(\d+),(\d+)\},(0x[0-9a-fA-F]+u)', entry)
    dirs = tuple((int(i), num(a), num(s)) for i, a, s in re.findall(r'\{(\d+),(0x[0-9a-fA-F]+u),(0x[0-9a-fA-F]+u)\}', entry))
    secs = tuple((n, num(a), num(s)) for n, a, s in re.findall(r'\{"(\.[a-z]+)",(0x[0-9a-fA-F]+u),(0x[0-9a-fA-F]+u)\}', entry))
    digest = re.search(r'\{((?:0x[0-9a-fA-F]{2},?){32})\}', entry)
    text = ''.join(f'{int(b, 16):02x}' for b in re.findall(r'0x([0-9a-fA-F]{2})', digest.group(1))) if digest else ''
    overlay.append(((int(head.group(1)), int(head.group(2)), int(head.group(3))), num(head.group(4)), dirs, secs, text))

ok = bool(launcher) and launcher == overlay and all(len(b[3]) == 4 and len(b[4]) == 64 for b in launcher)
for name, builds in (('launcher', launcher), ('overlay ', overlay)):
    for b in builds:
        print(name, b[0], hex(b[1]), len(b[2]), 'directories', [s[0] for s in b[3]], b[4][:16])
print('game builds in step' if ok else 'game builds DIFFER between desktop/GameBuilds.cs and native/game_build.hpp')
sys.exit(0 if ok else 1)
