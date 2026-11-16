"""The validated game builds of the launcher (desktop/GameBuilds.cs) and of the overlay (native/game_build.hpp) must
list the same builds: version, entry point, data directories, section addresses and sizes, and the SHA-256 of the
code section. Run by ctest (game_builds_in_step); tools/check_game_builds_test.py tests the address scan."""
import re
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
NL = chr(10)
BS = chr(92)

# A number written in native/ code is treated as a game address when it has 6 to 8 hex digits, is at least 0x100000
# and falls in a section of the reference build. A constant that only looks like one (a Unicode code point, a mask...)
# is exempted on its own line, with a reason: "// not a game address: <reason>". The exemption never covers
# "base + 0x...", which is always an address.
NOT_ADDRESS = re.compile(r'//\s*not a game address:\s*\S')
LITERAL = re.compile(r'0x([0-9a-fA-F]{6,8})' + BS + 'b')
BASE_PLUS = re.compile(BS + r'bbase' + BS + r's*' + BS + r'+' + BS + r's*0x([0-9a-fA-F]+)')


def num(text):
    return int(text.rstrip('u'), 0)


def code_part(line):
    q = None; i = 0
    while i < len(line):
        c = line[i]
        if q:
            if c == BS: i += 2; continue
            if c == q: q = None
        elif c in '"' + "'": q = c
        elif line.startswith('//', i): return line[:i]
        i += 1
    return line


def scan_line(line, sections):
    """(addresses, direct) found on one source line: addresses maps each game address to its section name; direct
    lists the "base + 0x..." offsets that fall in a section (a reference-build address used without Rva())."""
    code = code_part(line)
    addresses = {}
    if not NOT_ADDRESS.search(line[len(code):]):
        for m in LITERAL.finditer(code):
            v = int(m.group(1), 16)
            sec = next((k for k, (lo, hi) in sections.items() if lo <= v < hi and v >= 0x100000), None)
            if sec: addresses.setdefault(v, sec)
    direct = []
    for m in BASE_PLUS.finditer(code):
        v = int(m.group(1), 16)
        if any(lo <= v < hi for lo, hi in sections.values()) and v >= 0x100000: direct.append(v)
    return addresses, direct


def read_builds():
    cs = (root / 'desktop' / 'GameBuilds.cs').read_text(encoding='utf-8')
    hpp = (root / 'native' / 'game_build.hpp').read_text(encoding='utf-8')
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
    return launcher, overlay


def reference_sections(launcher):
    return {n: (a, a + z) for n, a, z in launcher[0][3] if n in ('.text', '.rdata', '.data')}


def main():
    launcher, overlay = read_builds()
    ok = bool(launcher) and launcher == overlay and all(len(b[3]) == 5 and len(b[4]) == 64 for b in launcher)

    # Every game address written in native/ code must be covered by each translated build (tools/port_build.py), and
    # reached through Rva(): a reference-build address used directly would be wrong in any other build.
    sections = reference_sections(launcher)
    used = {}; direct = []
    for f in sorted((root / 'native').glob('*.[hc]pp')):
        if f.name.startswith('generated_build_') or f.name == 'game_build.hpp': continue
        for n, line in enumerate(f.read_text(encoding='utf-8', errors='replace').splitlines(), 1):
            addresses, offsets = scan_line(line, sections)
            for v, sec in addresses.items(): used.setdefault(v, (sec, f.name))
            direct += [f'{f.name}:{n} base+0x{v:x}' for v in offsets]
    missing = []
    for g in sorted((root / 'native').glob('generated_build_*.hpp')):
        text = g.read_text(encoding='utf-8')
        points = {int(a, 16) for a in re.findall(r'\{0x([0-9a-f]+)u,0x[0-9a-f]+u\}', text.split('kPoints_')[1])}
        data = {int(a, 16) for a in re.findall(r'\{0x([0-9a-f]+)u,0x[0-9a-f]+u\}', text.split('kData_')[1].split(';')[0])}
        constants = {int(c, 16) for c in re.findall(r'0x([0-9a-f]+)u', text.split('kConstants_')[1].split(';')[0])} if 'kConstants_' in text else set()
        for v, (sec, where) in used.items():
            if (sec == '.text' and v not in points) or (sec != '.text' and v not in data and v not in points and v not in constants):
                missing.append(f'{g.name}: 0x{v:x} ({where}) not verified, run tools/port_build.py')
    for line in direct: print('NOT TRANSLATED', line)
    for line in missing[:40]: print('MISSING', line)
    ok = ok and not direct and not missing
    print(f'{len(used)} game addresses in native/, all translated and verified' if not direct and not missing else 'game addresses not covered')
    for name, builds in (('launcher', launcher), ('overlay ', overlay)):
        for b in builds:
            print(name, b[0], hex(b[1]), len(b[2]), 'directories', [s[0] for s in b[3]], b[4][:16])
    print('game builds in step' if ok else 'game builds DIFFER between desktop/GameBuilds.cs and native/game_build.hpp')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
