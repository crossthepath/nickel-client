"""Compares two x64 PE images (Nickel.Overlay.dll) built with /Brepro from two revisions.

python tools/compare_images.py <a.dll> <b.dll>

1. Section by section, ignoring only what a rebuild of the same code changes: the COFF and export time stamps, the
   checksum, the debug directory entries and the data they point to (CodeView record with the PDB GUID and path,
   /Brepro hash, POGO). Equal sections: IDENTICAL.
2. Otherwise, checks whether the images hold the same functions and data laid out in another order (what happens
   when the linker receives the same objects with renamed symbols, see compare_objects.py):
   - same base relocations and the same .data;
   - every function listed in .pdata disassembled (needs the capstone package), branch and call targets outside the
     function and RIP-relative displacements replaced by a placeholder, and the multisets of bodies compared;
   - every differing .rdata byte outside absolute pointers explained: debug data, export time stamp, or a 32-bit
     field whose value is a code address in both images (unwind, exception and control-flow-guard tables).
   All true: SAME FUNCTIONS (the layout differs). This alone does not show that each call still reaches the same
   function; compare the objects for that.
Anything else: DIFFERENT.

Exit code: 0 IDENTICAL or SAME FUNCTIONS, 1 DIFFERENT, 2 usage or file error (or capstone missing when needed).
"""
import collections
import hashlib
import re
import struct
import sys


class Image:
    def __init__(self, path):
        self.data = data = open(path, "rb").read()
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise ValueError(f"{path} is not a PE image")
        nsec, = struct.unpack_from("<H", data, pe + 6)
        optional_size, = struct.unpack_from("<H", data, pe + 20)
        self.opt = opt = pe + 24
        if struct.unpack_from("<H", data, opt)[0] != 0x20B:
            raise ValueError(f"{path} is not a PE32+ image")
        self.sections = []
        for i in range(nsec):
            at = opt + optional_size + 40 * i
            name = data[at:at + 8].rstrip(b"\0").decode()
            vsize, rva, rsize, raw = struct.unpack_from("<IIII", data, at + 8)
            self.sections.append((name, rva, vsize, raw, rsize))
        self.masks = [(pe + 8, 4), (opt + 64, 4)]   # COFF TimeDateStamp, CheckSum
        self.debug_ranges = []
        debug_rva, debug_size = self.directory(6)
        for k in range(debug_size // 28):
            entry = self.offset(debug_rva) + 28 * k
            _, _, _, _, _, size, address, pointer = struct.unpack_from("<IIHHIIII", data, entry)
            self.masks.append((entry, 28))
            self.debug_ranges.append((debug_rva + 28 * k, debug_rva + 28 * k + 28))
            if pointer and size:
                self.masks.append((pointer, size))
                self.debug_ranges.append((address, address + size))
        export_rva, _ = self.directory(0)
        if export_rva:
            self.masks.append((self.offset(export_rva) + 4, 4))
            self.debug_ranges.append((export_rva + 4, export_rva + 8))

    def directory(self, index):
        return struct.unpack_from("<II", self.data, self.opt + 112 + 8 * index)

    def offset(self, rva):
        for _, start, vsize, raw, rsize in self.sections:
            if start <= rva < start + max(vsize, rsize):
                return raw + rva - start
        raise ValueError(f"RVA {rva:#x} outside every section")

    def read(self, rva, size):
        at = self.offset(rva)
        return self.data[at:at + size]

    def section(self, name):
        return next(s for s in self.sections if s[0] == name)

    def masked(self):
        data = bytearray(self.data)
        for at, size in self.masks:
            data[at:at + size] = b"\0" * size
        return data

    def functions(self):
        rva, size = self.directory(3)
        table = self.read(rva, size)
        return [struct.unpack_from("<II", table, 12 * i) for i in range(size // 12)]

    def relocations(self):
        rva, size = self.directory(5)
        block, out, at = self.read(rva, size), set(), 0
        while at + 8 <= len(block):
            page, length = struct.unpack_from("<II", block, at)
            if length == 0:
                break
            for k in range((length - 8) // 2):
                entry, = struct.unpack_from("<H", block, at + 8 + 2 * k)
                if entry >> 12:
                    out.add(page + (entry & 0xFFF))
            at += length
        return out


def sections_equal(a, b, out):
    ma, mb = a.masked(), b.masked()
    same = len(ma) == len(mb) and [s[:3] for s in a.sections] == [s[:3] for s in b.sections]
    for (name, _, _, raw_a, size_a), (_, _, _, raw_b, size_b) in zip(a.sections, b.sections):
        equal = ma[raw_a:raw_a + size_a] == mb[raw_b:raw_b + size_b]
        out(f"  {name:8} {size_a:>9} bytes  {'identical' if equal else 'differs'}")
        same &= equal
    header = a.sections[0][3]
    return same and ma[:header] == mb[:header]


def normalized_bodies(image, disassembler):
    bodies = collections.Counter()
    for start, end in image.functions():
        code = image.read(start, end - start)
        lines, covered = [], 0
        for ins in disassembler.disasm(code, start):
            ops = ins.op_str
            if ins.mnemonic.startswith(("j", "call", "loop")) and re.fullmatch(r"0x[0-9a-f]+", ops):
                target = int(ops, 16)
                ops = f"L+{target - start:#x}" if start <= target < end else "T"
            ops = re.sub(r"rip ?[+-] ?0x[0-9a-f]+", "rip+D", ops)
            lines.append(f"{ins.mnemonic} {ops}")
            covered += ins.size
        bodies[hashlib.sha256(("\n".join(lines) + f"|{end - start}|{covered}").encode()).hexdigest()] += 1
    return bodies


def same_functions(a, b, out):
    try:
        import capstone
    except ImportError:
        out("  the images differ; install capstone (pip install capstone) to check whether only the layout changed")
        raise SystemExit(2)
    ok = True
    relocs = a.relocations()
    if relocs != b.relocations():
        out("  base relocations differ")
        ok = False
    for name in (".data", ".rdata"):
        sa, sb = a.section(name), b.section(name)
        if sa[1:3] != sb[1:3]:
            out(f"  {name} moved or changed size")
            ok = False
            continue
        da, db = bytearray(a.read(sa[1], sa[4])), bytearray(b.read(sb[1], sb[4]))
        for r in relocs:   # absolute pointers move with the layout
            if sa[1] <= r <= sa[1] + len(da) - 8:
                da[r - sa[1]:r - sa[1] + 8] = db[r - sa[1]:r - sa[1] + 8] = b"\0" * 8
        text = a.section(".text")
        code = lambda value: text[1] <= value < text[1] + text[2]
        unexplained = 0
        for i in (i for i in range(len(da)) if da[i] != db[i]):
            rva = sa[1] + i
            if any(lo <= rva < hi for lo, hi in a.debug_ranges + b.debug_ranges):
                continue
            if any(code(struct.unpack_from("<I", da, s)[0]) and code(struct.unpack_from("<I", db, s)[0])
                   for s in range(max(0, i - 3), min(i, len(da) - 4) + 1)):
                continue
            unexplained += 1
        out(f"  {name}: {unexplained} differing bytes not explained by the layout")
        ok &= unexplained == 0
    disassembler = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    fa, fb = normalized_bodies(a, disassembler), normalized_bodies(b, disassembler)
    only_a, only_b = sum((fa - fb).values()), sum((fb - fa).values())
    out(f"  functions: {sum(fa.values())} / {sum(fb.values())}, bodies only in A: {only_a}, only in B: {only_b}")
    return ok and only_a == 0 and only_b == 0


def compare(path_a, path_b, out=print):
    a, b = Image(path_a), Image(path_b)
    if sections_equal(a, b, out):
        return "IDENTICAL"
    return "SAME FUNCTIONS" if same_functions(a, b, out) else "DIFFERENT"


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    try:
        verdict = compare(argv[1], argv[2])
    except (OSError, ValueError, struct.error, StopIteration) as error:
        print(f"error: {error}")
        return 2
    print(verdict)
    return 0 if verdict in ("IDENTICAL", "SAME FUNCTIONS") else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
