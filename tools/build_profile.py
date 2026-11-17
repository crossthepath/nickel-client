"""Print the build profile entries of a Minecraft.Windows.exe for native/game_build.hpp (kGameBuilds) and
desktop/GameBuilds.cs (Validated): version, entry point, the ten load-time directories, the five checked sections
and the SHA-256 of each section's bytes in the file.

python tools/build_profile.py <Minecraft.Windows.exe> [ident]

ident is the suffix of the generated translation tables (for example 1_26_65_1); without it the entry has no tables
(the reference build). tools/check_game_builds.py checks that both files list the same builds.
"""
import hashlib
import struct
import sys

DIRECTORIES = (0, 1, 3, 5, 9, 10, 11, 12, 13, 14)
SECTIONS = ('.text', '.rdata', '.data', '.pdata', '.reloc')


def read(path):
    raw = open(path, 'rb').read()
    e = struct.unpack_from('<I', raw, 0x3c)[0]; opt = e + 24
    count = struct.unpack_from('<H', raw, e + 6)[0]; size = struct.unpack_from('<H', raw, e + 20)[0]
    entry = struct.unpack_from('<I', raw, opt + 16)[0]
    dirs = {i: struct.unpack_from('<II', raw, opt + 112 + 8 * i) for i in DIRECTORIES}
    secs = {}
    for i in range(count):
        s = opt + size + 40 * i; name = raw[s:s + 8].rstrip(b'\0').decode()
        vsize, va, rsize, rptr = struct.unpack_from('<IIII', raw, s + 8)
        secs[name] = (va, vsize, hashlib.sha256(raw[rptr:rptr + min(vsize, rsize)]).hexdigest())
    return raw, entry, dirs, secs


def version(raw):
    at = raw.find(struct.pack('<I', 0xFEEF04BD))
    ms, ls = struct.unpack_from('<II', raw, at + 8)
    return ms >> 16, ms & 0xffff, ls >> 16, ls & 0xffff


def main():
    raw, entry, dirs, secs = read(sys.argv[1])
    ident = sys.argv[2] if len(sys.argv) > 2 else None
    v = version(raw); name = f'Minecraft {v[0]}.{v[1]}.{v[2]}.{v[3]}'
    text = secs['.text'][2]
    tables = f'kRanges_{ident},CountOf(kRanges_{ident}),kData_{ident},CountOf(kData_{ident})' if ident else 'nullptr,0,nullptr,0'
    print('// native/game_build.hpp, kGameBuilds')
    print(f' {{"{name}",{{{v[0]},{v[1]},{v[2]}}},0x{entry:x}u,')
    print('  {' + ','.join(f'{{{i},0x{a:x}u,0x{s:x}u}}' for i, (a, s) in dirs.items()) + '},')
    print('  {' + ','.join(f'{{"{n}",0x{secs[n][0]:x}u,0x{secs[n][1]:x}u}}' for n in SECTIONS) + '},')
    print('  {' + ','.join(f'0x{text[k:k + 2]}' for k in range(0, 64, 2)) + '},')
    print(f'  {tables}}},')
    print('// desktop/GameBuilds.cs, Validated')
    print(f'  new("{name}", {v[0]}, {v[1]}, {v[2]}, 0x{entry:x},')
    print('  [')
    items = [f'new({i}, {f"0x{a:x}" if a else "0"}, {f"0x{s:x}" if s else "0"})' for i, (a, s) in dirs.items()]
    print('   ' + ', '.join(items[:4]) + ',')
    print('   ' + ', '.join(items[4:]) + ',')
    print('  ],')
    print('  [')
    for n in SECTIONS:
        print(f'   new("{n}", 0x{secs[n][0]:x}, 0x{secs[n][1]:x}, "{secs[n][2]}"),')
    print('  ]),')


if __name__ == '__main__':
    main()
