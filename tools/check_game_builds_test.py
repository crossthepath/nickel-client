"""Tests of the game address scan of tools/check_game_builds.py (ctest game_builds_scan). A constant that only looks
like a game address is exempted only by an explicit "// not a game address: <reason>" on its line; every other
address-looking number stays checked."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_game_builds as cgb  # noqa: E402

failures = []


def check(name, condition):
    if not condition:
        failures.append(name)
    print(('ok   ' if condition else 'FAIL ') + name)


launcher, _ = cgb.read_builds()
sections = cgb.reference_sections(launcher)
in_section = next((k for k, (lo, hi) in sections.items() if lo <= 0x10FFFF < hi), None)
check('0x10ffff falls in a section of the reference build (why it was a false positive)', in_section is not None)

plain = 'if (code > 0x10FFFF) return false;'
check('an unmarked number in a section is a game address', 0x10FFFF in cgb.scan_line(plain, sections)[0])
check('the exemption needs its marker and a reason',
      0x10FFFF in cgb.scan_line(plain + ' // not a game address:', sections)[0])
check('the marker inside a string does not exempt the line',
      0x10FFFF in cgb.scan_line('Log("// not a game address: x"); v = 0x10FFFF;', sections)[0])
check('a marked line is not a game address',
      cgb.scan_line(plain + ' // not a game address: largest Unicode code point', sections) == ({}, []))
check('a number in a comment is not a game address', cgb.scan_line('x = 1; // see 0x10FFFF', sections) == ({}, []))
check('the marker never exempts base + offset',
      cgb.scan_line('auto p = base + 0x10FFFF; // not a game address: wrong', sections)[1] == [0x10FFFF])

# Regression: json_value.hpp keeps its Unicode limit exempted, and the real scan of native/ finds no address there.
json_lines = (cgb.root / 'native' / 'json_value.hpp').read_text(encoding='utf-8').splitlines()
unicode_lines = [line for line in json_lines if '0X10FFFF' in line.upper()]
check('json_value.hpp still has its Unicode limit', bool(unicode_lines))
check('json_value.hpp reports no game address', all(cgb.scan_line(line, sections) == ({}, []) for line in json_lines))

if failures:
    print(f'{len(failures)} failed')
sys.exit(1 if failures else 0)
