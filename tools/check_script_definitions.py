"""Checks that docs/scripting/nickel.d.luau declares exactly the functions the script engine registers.

The definitions are what Nickel AI's script check and its scripting reference rely on: a function added to (or removed
from) native/script_engine.hpp RegisterApi without the definitions would make the assistant reject working scripts or
accept broken ones. Run: python tools/check_script_definitions.py (ctest: script_definitions_checks).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENGINE = ROOT / "native" / "script_engine.hpp"
DEFINITIONS = ROOT / "docs" / "scripting" / "nickel.d.luau"

# luaL_Reg table -> where the definitions declare it: an extern type (methods) or a global table.
TABLES = {
    "setting": ("type", "Setting"), "module": ("type", "Module"), "builtin": ("type", "BuiltinModule"),
    "screen": ("type", "ScreenDraw"), "world3d": ("type", "WorldDraw"), "api": ("global", "nickel"),
    "mods": ("global", "nickel.modules"),
}


def registered():
    text = ENGINE.read_text(encoding="utf-8")
    out = {}
    for table, body in re.findall(r"static const luaL_Reg (\w+)\[\]=\{(.*?)\{nullptr,nullptr\}\};", text, re.S):
        kind, name = TABLES.get(table, ("global", table))
        out[(kind, name)] = set(re.findall(r'\{"(\w+)",', body))
    return out


def declared():
    text = DEFINITIONS.read_text(encoding="utf-8")
    out = {}
    for name, body in re.findall(r"declare extern type (\w+)[^\n]* with\n(.*?)\nend", text, re.S):
        out[("type", name)] = set(re.findall(r"^\s*function (\w+)\(self", body, re.M))
    for name, body in re.findall(r"^declare (\w+): \{\n(.*?)\n\}", text, re.S | re.M):
        top = set()
        nested = {}
        depth = 0
        current = None
        for line in body.splitlines():
            stripped = line.strip()
            match = re.match(r"(\w+):", stripped)
            if depth == 0 and match:
                if stripped.endswith("{"):
                    current = match.group(1)
                    nested[current] = set()
                    depth = 1
                    continue
                top.add(match.group(1))
            elif depth == 1:
                if stripped.startswith("}"):
                    depth = 0
                    continue
                if match:
                    nested[current].add(match.group(1))
        out[("global", name)] = top | set(nested)
        for inner, names in nested.items():
            out[("global", f"{name}.{inner}")] = names
    return out


def main():
    engine, definitions = registered(), declared()
    problems = []
    for key, names in sorted(engine.items()):
        have = definitions.get(key)
        if have is None:
            problems.append(f"{key[1]}: not declared in nickel.d.luau")
            continue
        missing = sorted(names - have)
        if missing:
            problems.append(f"{key[1]}: registered but not declared: {', '.join(missing)}")
        extra = sorted(n for n in have - names if not (key == ("global", "nickel") and n == "modules"))
        if extra:
            problems.append(f"{key[1]}: declared but not registered: {', '.join(extra)}")
    if "declare function require(" not in DEFINITIONS.read_text(encoding="utf-8"):
        problems.append("require: not declared")
    for line in problems:
        print(line)
    print(f"{sum(len(v) for v in engine.values())} registered functions in {len(engine)} tables; "
          f"{len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
