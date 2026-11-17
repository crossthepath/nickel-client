"""Port Nickel's game addresses from the reference build (Minecraft 1.26.62.1) to another build of the same game.

python tools/port_build.py <reference Minecraft.Windows.exe> <new Minecraft.Windows.exe> <name> [--out native/generated_build_<name>.hpp]

1. Pairs the functions of both builds (exception table .pdata, aligned on the sequence of function sizes).
2. Collects every reference-build address written in native/ (code and data sections, comments excluded).
3. Code addresses: the function holding the address must be paired and identical once relative branch targets and
   RIP-relative displacements are masked; leaf functions (no .pdata entry) are placed with the delta of their
   neighbours and compared the same way up to their first ret. The address keeps its offset in the function.
4. Data addresses (.rdata / .data): found through the RIP-relative instructions of the reference build that point at
   them; the matching instruction of the paired function in the new build gives the new address, which must be the
   same for every reference checked.
5. Writes a header with the translation ranges (code: [start, end) + delta), the data pairs and the verified points
   (every reference address and its new address), and refuses to write it when any used address fails.
Requires capstone and numpy. The header is checked by tests/game_build_checks.cpp (every point must translate).
"""
import bisect
import re
import struct
import sys
from pathlib import Path

import capstone
import numpy as np

ROOT = Path(__file__).resolve().parent.parent
MD = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
MD.detail = True


def load(path):
    raw = open(path, 'rb').read()
    e = struct.unpack_from('<I', raw, 0x3c)[0]; opt = e + 24
    nsec = struct.unpack_from('<H', raw, e + 6)[0]; optsz = struct.unpack_from('<H', raw, e + 20)[0]
    secs = {}
    for i in range(nsec):
        s = opt + optsz + 40 * i; n = raw[s:s + 8].rstrip(b'\0').decode()
        vs, va, rs, rp = struct.unpack_from('<IIII', raw, s + 8); secs[n] = (va, vs, rs, rp)

    def rd(rva, n):
        for va, vs, rs, rp in secs.values():
            if va <= rva < va + max(vs, rs):
                o = rva - va; return raw[rp + o: rp + o + n]
        return b''
    exc, excs = struct.unpack_from('<II', raw, opt + 112 + 24)
    pd = rd(exc, excs)
    funcs = sorted(set(struct.unpack_from('<II', pd, i) for i in range(0, len(pd) - 11, 12)))
    return {'raw': raw, 'secs': secs, 'rd': rd, 'funcs': [f for f in funcs if f[1] > f[0]]}


def pair(a, b):
    fa, fb = a['funcs'], b['funcs']
    sa = [e - s for s, e in fa]; sb = [e - s for s, e in fb]
    i = j = 0; pairs = {}
    while i < len(fa) and j < len(fb):
        if sa[i] == sb[j]:
            pairs[fa[i][0]] = (fb[j][0], fa[i][1], fb[j][1]); i += 1; j += 1; continue
        found = None
        for d in range(1, 400):
            for k in range(d + 1):
                if sa[i + k:i + k + 8] == sb[j + d - k:j + d - k + 8]: found = (k, d - k); break
            if found: break
        if not found: break
        i += found[0]; j += found[1]
    return pairs


def masked(rd, start, length=None, until_ret=False):
    out = []
    for ins in MD.disasm(rd(start, length or 0x400), start):
        by = bytearray(ins.bytes)
        if ins.disp_size and ins.disp_offset and any(op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP for op in ins.operands):
            for k in range(ins.disp_size): by[ins.disp_offset + k] = 0
        if ins.imm_size and ins.imm_offset and (ins.group(capstone.x86.X86_GRP_JUMP) or ins.group(capstone.x86.X86_GRP_CALL)) and ins.operands and ins.operands[0].type == capstone.x86.X86_OP_IMM:
            for k in range(ins.imm_size): by[ins.imm_offset + k] = 0
        out.append(bytes(by))
        if until_ret and (ins.mnemonic in ('ret', 'int3') or len(out) >= 64): break
    return out


def used_addresses(ref):
    ranges = {n: (ref['secs'][n][0], ref['secs'][n][0] + ref['secs'][n][1]) for n in ('.text', '.rdata', '.data')}
    found = {}
    for f in sorted((ROOT / 'native').glob('*.[hc]pp')):
        if f.name.startswith('generated_build_') or f.name == 'generated_signatures.hpp': continue
        for line in f.read_text(encoding='utf-8', errors='replace').splitlines():
            code = '' if line.lstrip().startswith('//') else line.split('//')[0]
            for m in re.finditer(r'0x([0-9a-fA-F]{6,8})\b', code):
                v = int(m.group(1), 16)
                for n, (lo, hi) in ranges.items():
                    if lo <= v < hi and v >= 0x100000: found.setdefault(v, (n, f.name))
    return found


def data_xrefs(ref, targets):
    va, vs, rs, rp = ref['secs']['.text']
    text = np.frombuffer(ref['raw'][rp:rp + min(vs, rs)], dtype=np.uint8)
    want = np.array(sorted({t - k for t in targets for k in (0, 1, 2, 4)}), dtype=np.int64)
    hits = {t: [] for t in targets}; chunk = 1 << 24
    for c0 in range(0, len(text) - 4, chunk):
        seg = text[c0:c0 + chunk + 4].astype(np.int64); n = len(seg) - 3
        disp = seg[:n] | (seg[1:n + 1] << 8) | (seg[2:n + 2] << 16) | (seg[3:n + 3] << 24)
        disp = np.where(disp >= 2 ** 31, disp - 2 ** 32, disp)
        pos = np.arange(n, dtype=np.int64) + c0 + va
        for i in np.nonzero(np.isin(pos + 4 + disp, want))[0]:
            p = int(pos[i])
            for back in range(1, 9):
                ins = next(MD.disasm(ref['rd'](p - back, 16), p - back), None)
                if ins and ins.disp_offset == back and any(op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP for op in ins.operands):
                    tgt = ins.address + ins.size + ins.disp
                    if tgt in hits and len(hits[tgt]) < 16: hits[tgt].append(p - back)
                    break
    return hits


def main():
    ref, new, name = load(sys.argv[1]), load(sys.argv[2]), sys.argv[3]
    ident = name.replace('.', '_')
    out = Path(sys.argv[sys.argv.index('--out') + 1]) if '--out' in sys.argv else ROOT / 'native' / f'generated_build_{ident}.hpp'
    pairs = pair(ref, new); starts = [f[0] for f in ref['funcs']]
    used = used_addresses(ref); failures = []; points = {}
    for v, (sec, where) in sorted(used.items()):
        if sec != '.text': continue
        i = bisect.bisect_right(starts, v) - 1
        if i >= 0 and ref['funcs'][i][0] <= v < ref['funcs'][i][1]:
            fb, fe = ref['funcs'][i]
            if fb not in pairs: failures.append((v, where, 'function not paired')); continue
            nb, _, ne = pairs[fb]
            if masked(ref['rd'], fb, fe - fb) != masked(new['rd'], nb, ne - nb): failures.append((v, where, 'function changed')); continue
            points[v] = nb + (v - fb)
        else:   # leaf function or padding: delta of a neighbour, compared up to the first ret
            near = [f for f in (ref['funcs'][i] if i >= 0 else None, ref['funcs'][i + 1] if i + 1 < len(ref['funcs']) else None) if f and f[0] in pairs]
            deltas = [pairs[f[0]][0] - f[0] for f in near]
            hit = next((d for d in dict.fromkeys(deltas) if masked(ref['rd'], v, until_ret=True) == masked(new['rd'], v + d, until_ret=True)), None)
            if hit is None: failures.append((v, where, 'leaf code differs')); continue
            points[v] = v + hit
    data = {v for v, (sec, _) in used.items() if sec != '.text'}
    data_map = {}; constants = set()
    for t, refs in data_xrefs(ref, data).items():
        got = set()
        for st in refs[:6]:
            i = bisect.bisect_right(starts, st) - 1
            if i < 0 or ref['funcs'][i][0] not in pairs: continue
            nst = pairs[ref['funcs'][i][0]][0] + (st - ref['funcs'][i][0])
            ins = next(MD.disasm(new['rd'](nst, 16), nst), None)
            if ins and ins.disp_size: got.add(ins.address + ins.size + ins.disp)
        if len(got) == 1: data_map[t] = got.pop()
        elif refs: failures.append((t, used[t][1], f'data address ambiguous {sorted(got)}'))
        else: constants.add(t)   # no instruction references it: a constant in the same range (a size, a hash), not game data
    if failures:
        for f in failures: print('FAIL', hex(f[0]), f[1], f[2])
        sys.exit(1)
    runs = []   # code ranges: consecutive paired functions with one delta, each extended to the next range
    for fb in sorted(pairs):
        d = pairs[fb][0] - fb
        if runs and runs[-1][2] == d: runs[-1][1] = pairs[fb][1]
        else: runs.append([fb, pairs[fb][1], d])
    text_lo = ref['secs']['.text'][0]; text_end = text_lo + ref['secs']['.text'][1]
    ranges = [[r[0] if k else text_lo, runs[k + 1][0] if k + 1 < len(runs) else text_end, r[2]] for k, r in enumerate(runs)]
    starts_r = [r[0] for r in ranges]

    def tr(v):
        k = bisect.bisect_right(starts_r, v) - 1
        return v + ranges[k][2] if k >= 0 and v < ranges[k][1] else None
    wrong = [(hex(v), hex(n)) for v, n in points.items() if tr(v) != n]
    if wrong: print('ranges do not reproduce', wrong[:20]); sys.exit(1)
    lines = [f'// Generated by tools/port_build.py: Nickel addresses of the reference build (1.26.62.1) -> {name}. Do not edit.',
             '// Code: ranges [start, end) of the reference build moved by delta; data: exact pairs; points: every code address',
             '// Nickel uses with its verified new address (functions compared with relative fields masked).',
             '#pragma once', 'namespace game {',
             f'inline constexpr RvaRange kRanges_{ident}[]={{' + ','.join(f'{{0x{a:x}u,0x{b:x}u,{d}}}' for a, b, d in ranges) + '};',
             f'inline constexpr RvaPair kData_{ident}[]={{' + ','.join(f'{{0x{a:x}u,0x{b:x}u}}' for a, b in sorted(data_map.items())) + '};',
             f'inline constexpr RvaPair kPoints_{ident}[]={{' + ','.join(f'{{0x{a:x}u,0x{b:x}u}}' for a, b in sorted(points.items())) + '};',
             f'inline constexpr uint32_t kConstants_{ident}[]={{' + (','.join(f'0x{c:x}u' for c in sorted(constants)) or '0u') + '};   // not addresses',
             '}', '']
    out.write_text('\n'.join(lines), encoding='utf-8')
    print(f'{out.name}: {len(ranges)} code ranges, {len(data_map)} data pairs, {len(points)} verified code points, {len(constants)} constants')


if __name__ == '__main__':
    main()
