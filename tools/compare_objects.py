"""Compares two MSVC object files (COFF, including /bigobj) compiled from the same code at two revisions.

python tools/compare_objects.py <a.obj> <b.obj>

Every section except the debug sections is compared on its size, characteristics, COMDAT selection and associated
section, raw bytes, and relocations resolved to symbol names; the symbol tables are compared on name, value, section,
type and storage class. Sections are matched by name and COMDAT leader symbol, not by position.

Excluded, because a move or a reformat changes them without changing the program:
  .debug$S, .debug$T, ...   line tables, file names and type records
  .chks64                   per-section checksums, which also cover the debug sections

Names that MSVC makes up itself may differ: a lambda's closure type is named <lambda_HASH>, and the hash depends on
the source file, so a lambda moved to another file gets a new name. Differing names are paired by signature (the
name with every lambda hash blanked out); if the pairing is one-to-one and every section and symbol is then equal,
the objects are reported EQUIVALENT (same code and data under renamed lambda types). Any other difference is
DIFFERENT.

Exit code: 0 IDENTICAL or EQUIVALENT, 1 DIFFERENT, 2 usage or file error. Python 3 standard library only.
"""
import collections
import re
import struct
import sys

LAMBDA = re.compile(r"<lambda_([0-9a-f]{32})>")


def load(path):
    """Returns (symbols, sections, COMDAT leader per section, (selection, associated section) per section)."""
    data = open(path, "rb").read()
    sig1, sig2 = struct.unpack_from("<HH", data, 0)
    if sig1 == 0 and sig2 == 0xFFFF:   # ANON_OBJECT_HEADER_BIGOBJ
        nsec, symptr, nsym = struct.unpack_from("<III", data, 44)
        sec_off, symsize, big = 56, 20, True
    else:
        nsec, = struct.unpack_from("<H", data, 2)
        symptr, nsym = struct.unpack_from("<II", data, 8)
        optional, = struct.unpack_from("<H", data, 16)
        sec_off, symsize, big = 20 + optional, 18, False
    strtab = symptr + nsym * symsize

    def string_at(offset):
        end = data.index(b"\0", strtab + offset)
        return data[strtab + offset:end].decode("latin1")

    def short_name(raw):
        if raw[:4] == b"\0\0\0\0":
            return string_at(struct.unpack_from("<I", raw, 4)[0])
        return raw.rstrip(b"\0").decode("latin1")

    symbols, aux = [], {}
    index = 0
    while index < nsym:
        at = symptr + index * symsize
        layout = "<IiHBB" if big else "<IhHBB"
        value, section, kind, storage, naux = struct.unpack_from(layout, data, at + 8)
        symbols.append((short_name(data[at:at + 8]), value, section, storage, naux, kind))
        if storage == 3 and naux and section > 0:   # section symbol: its aux record holds the COMDAT selection
            number, selection = struct.unpack_from("<HB", data, at + symsize + 12)
            high = struct.unpack_from("<H", data, at + symsize + 16)[0] if big else 0
            aux[section] = (selection, number | (high << 16))
        symbols.extend([None] * naux)
        index += 1 + naux

    sections = []
    for k in range(nsec):
        at = sec_off + 40 * k
        name = data[at:at + 8].rstrip(b"\0").decode("latin1")
        if name.startswith("/"):
            name = string_at(int(name[1:]))
        size, pointer, relocs_at, _, nrelocs, _, flags = struct.unpack_from("<IIIIHHI", data, at + 16)
        raw = data[pointer:pointer + size] if pointer else b""
        relocs = [struct.unpack_from("<IIH", data, relocs_at + 10 * r) for r in range(nrelocs)]
        sections.append((name, raw, relocs, size, flags))

    leader = {}
    for symbol in symbols:
        if symbol is None:
            continue
        name, _, section, storage, naux, _ = symbol
        if section > 0 and section not in leader and not (storage == 3 and naux > 0):
            leader[section] = name
    return symbols, sections, leader, aux


def excluded(name):
    return name.startswith(".debug$") or name == ".chks64"


def contents(obj, rename):
    """Everything the comparison covers, keyed so that sections match by name and COMDAT leader."""
    symbols, sections, leader, aux = obj
    r = lambda name: rename.get(name, name)

    def key(number):
        if 0 < number <= len(sections):
            return (sections[number - 1][0], r(leader.get(number, "")))
        return number

    out = collections.defaultdict(list)
    for number, (name, raw, relocs, size, flags) in enumerate(sections, 1):
        if excluded(name):
            continue
        selection, associated = aux.get(number, (0, 0))
        resolved = tuple((offset, r(symbols[s][0]) if symbols[s] else "?", kind) for offset, s, kind in relocs)
        out[key(number)].append((size, flags, selection, key(associated) if selection == 5 else 0, raw, resolved))
    out[("<symbols>", "")] = [
        (r(s[0]), s[1], str(key(s[2])), s[5], s[3])
        for s in symbols
        if s and not (0 < s[2] <= len(sections) and excluded(sections[s[2] - 1][0]))
    ]
    return {k: sorted(v) for k, v in out.items()}


def lambda_renaming(a, b):
    """Pairs the names found in only one object by signature; returns (renaming, renamed lambda types) or None."""
    names_a = collections.Counter(s[0] for s in a[0] if s)
    names_b = collections.Counter(s[0] for s in b[0] if s)
    only_a, only_b = set(names_a) - set(names_b), set(names_b) - set(names_a)
    if any(LAMBDA.search(n) is None for n in only_a | only_b):
        return None   # a name that is not a compiler-made lambda name changed
    shape = lambda name: LAMBDA.sub("<lambda_?>", name)
    groups_a, groups_b = collections.defaultdict(list), collections.defaultdict(list)
    for symbols, only, groups in ((a[0], only_a, groups_a), (b[0], only_b, groups_b)):
        for s in symbols:
            if s and s[0] in only and s[0] not in groups[shape(s[0])]:
                groups[shape(s[0])].append(s[0])
    if set(groups_a) != set(groups_b) or any(len(groups_a[k]) != len(groups_b[k]) for k in groups_a):
        return None
    forward, backward = {}, {}
    for k in groups_a:
        for x, y in zip(groups_a[k], groups_b[k]):
            for hx, hy in zip(LAMBDA.findall(x), LAMBDA.findall(y)):
                if forward.setdefault(hx, hy) != hy or backward.setdefault(hy, hx) != hx:
                    return None
    rename = {n: LAMBDA.sub(lambda m: "<lambda_" + forward.get(m.group(1), m.group(1)) + ">", n) for n in only_a}
    if set(rename.values()) != only_b:
        return None
    return rename, len(forward)


def compare(path_a, path_b, out=print):
    """Returns 'IDENTICAL', 'EQUIVALENT' or 'DIFFERENT' and prints the evidence."""
    a, b = load(path_a), load(path_b)
    pairing = lambda_renaming(a, b)
    if pairing is None:
        out("  symbol names differ beyond renamed lambda types")
        rename, renamed = {}, 0
    else:
        rename, renamed = pairing
    ca, cb = contents(a, rename), contents(b, {})
    missing, extra = set(ca) - set(cb), set(cb) - set(ca)
    differing = sorted(k for k in set(ca) & set(cb) if ca[k] != cb[k])
    for k in sorted(missing)[:5]:
        out(f"  only in A: {k}")
    for k in sorted(extra)[:5]:
        out(f"  only in B: {k}")
    for k in differing[:5]:
        out(f"  differs: {k[0]} {str(k[1])[:150]}")
    if pairing is None or missing or extra or differing:
        return "DIFFERENT"
    if renamed:
        out(f"  {renamed} lambda types renamed ({len(rename)} symbol names), everything else equal")
        return "EQUIVALENT"
    return "IDENTICAL"


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    try:
        verdict = compare(argv[1], argv[2])
    except (OSError, struct.error, ValueError) as error:
        print(f"error: {error}")
        return 2
    print(verdict)
    return 0 if verdict in ("IDENTICAL", "EQUIVALENT") else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
