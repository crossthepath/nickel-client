"""Keep ui/catalog.js in step with native/generated_catalog.hpp.

native/generated_catalog.hpp is the module catalog the overlay runs on (ids, names, descriptions, categories, settings
and defaults). ui/catalog.js is written from it for the launcher and other tools.

  python tools/sync_catalog.py          rewrite ui/catalog.js from the header
  python tools/sync_catalog.py --check  exit 1 when ui/catalog.js differs from the header (used by ctest)
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "native" / "generated_catalog.hpp"
OUTPUT = ROOT / "ui" / "catalog.js"
CATEGORIES = ["Combat", "Movement", "Player", "Visuals", "Utility", "HUD", "Other"]
SETTING = ["id", "name", "description", "kind", "minimum", "maximum", "step", "number", "text", "options", "unit",
           "dependsOn", "dependsEqual", "dependsValue"]


def tokens(text):
    """Tokens of a C++ brace initializer: '{', '}', strings, numbers, true/false (commas are separators)."""
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in " \t\r\n,":
            i += 1
        elif c in "{}":
            yield c
            i += 1
        elif c == '"':
            raw = bytearray()
            i += 1
            while text[i] != '"':
                if text[i] == "\\":
                    nxt = text[i + 1]
                    if nxt in "01234567":
                        j = i + 1
                        while j < i + 4 and text[j] in "01234567":
                            j += 1
                        raw.append(int(text[i + 1:j], 8))
                        i = j
                        continue
                    raw += {"n": b"\n", "t": b"\t", '"': b'"', "\\": b"\\"}.get(nxt, nxt.encode())
                    i += 2
                    continue
                raw += text[i].encode("utf-8")
                i += 1
            i += 1
            yield ("str", raw.decode("utf-8"))
        else:
            j = i
            while j < n and text[j] not in " \t\r\n,{}":
                j += 1
            word = text[i:j]
            i = j
            yield ("bool", word == "true") if word in ("true", "false") else ("num", float(word))


def parse(tok):
    """Nested lists from tokens starting after an opening '{'."""
    out = []
    for t in tok:
        if t == "{":
            out.append(parse(tok))
        elif t == "}":
            return out
        else:
            out.append(t[1])
    return out


def read_header():
    text = HEADER.read_text(encoding="utf-8")
    start = text.index("modules = {") + len("modules = {")
    end = text.index("};", start)
    entries = parse(tokens(text[start:end] + "}"))
    modules = []
    for entry in entries:
        mid, name, description, category, settings = entry
        rows = []
        for s in settings:
            row = dict(zip(SETTING, s))
            for key in ("minimum", "maximum", "step", "number"):
                row[key] = row[key] if row[key] != int(row[key]) else int(row[key])
            for key in ("dependsOn", "dependsValue"):
                row[key] = int(row[key])
            rows.append(row)
        modules.append({"id": mid, "name": name, "description": description, "category": category, "settings": rows})
    return modules


def render(modules):
    used = [c for c in CATEGORIES if any(m["category"] == c for m in modules)]
    body = json.dumps({"version": 2, "source": "native/generated_catalog.hpp", "categories": used, "modules": modules},
                      ensure_ascii=False, indent=1)
    return "// Generated from native/generated_catalog.hpp by tools/sync_catalog.py. Do not edit by hand.\n" \
           f"window.NICKEL_CATALOG = {body};\n"


def main():
    modules = read_header()
    ids = [m["id"] for m in modules]
    if len(ids) != len(set(ids)):
        sys.exit("duplicate module id in native/generated_catalog.hpp")
    wanted = render(modules)
    current = OUTPUT.read_text(encoding="utf-8") if OUTPUT.exists() else ""
    if "--check" in sys.argv:
        if current != wanted:
            print("ui/catalog.js differs from native/generated_catalog.hpp: run python tools/sync_catalog.py")
            return 1
        counts = re.search(r"ExpectedModules=(\d+),ExpectedSettings=(\d+)", (ROOT / "desktop" / "Models.cs").read_text(encoding="utf-8"))
        settings = sum(len(m["settings"]) for m in modules)
        if not counts or (int(counts[1]), int(counts[2])) != (len(modules), settings):
            print(f"desktop/Models.cs: set ExpectedModules={len(modules)},ExpectedSettings={settings} (launcher self-test)")
            return 1
        print(f"catalogs in step: {len(modules)} modules, {settings} settings")
        return 0
    OUTPUT.write_text(wanted, encoding="utf-8", newline="\n")
    print(f"ui/catalog.js written: {len(modules)} modules, {sum(len(m['settings']) for m in modules)} settings")
    return 0


if __name__ == "__main__":
    sys.exit(main())
