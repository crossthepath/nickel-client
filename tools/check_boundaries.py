"""Counts the names one component uses from another, so that coupling can only go down.

python tools/check_boundaries.py            check every rule of tools/boundaries.json (CTest: boundary_checks)
python tools/check_boundaries.py --update   record lower counts in tools/boundaries.json
python tools/check_boundaries.py --update --accept-increase
                                            also record higher counts (only for a dependency the pull request
                                            explains, for example a new identifier added to a rule)

Each rule names files and identifiers that those files should not use (for example the Luau engine reading the menu's
globals). The count of each identifier, outside comments and string literals, must equal the baseline stored in the
rule:
- higher: a new dependency crept in; the check fails. Use the component's interface instead.
- lower: a dependency was removed; the check also fails until the baseline is lowered with --update, so that the
  improvement is recorded and cannot be undone silently by a later change.
--update never raises a baseline unless --accept-increase is also given, so the command that records an improvement
cannot also hide a new dependency.
Counts are of names, not of uses that resolve to the other component: a local variable with the same name counts too.
Python 3 standard library only.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RULES = ROOT / "tools" / "boundaries.json"

TOKEN = re.compile(
    r'//[^\n]*'                                   # line comment
    r'|/\*.*?\*/'                                 # block comment
    r'|(?:u8|u|U|L)?R"(?P<delim>[^(\s]{0,16})\(.*?\)(?P=delim)"'   # raw string literal
    r'|(?:u8|u|U|L)?"(?:\\.|[^"\\\n])*"'          # string literal
    r"|(?:u8|u|U|L)?'(?:\\.|[^'\\\n])*'"          # character literal
    r'|(?P<ident>[A-Za-z_][A-Za-z_0-9]*)'         # identifier
    r'|\d[\w.\']*',                               # number (so digits are not read as identifiers)
    re.S,
)


def identifiers(text):
    for match in TOKEN.finditer(text):
        name = match.group("ident")
        if name:
            yield name


def count(rule):
    wanted = set(rule["identifiers"])
    counts = dict.fromkeys(rule["identifiers"], 0)
    for relative in rule["files"]:
        text = (ROOT / relative).read_text(encoding="utf-8-sig")
        for name in identifiers(text):
            if name in wanted:
                counts[name] += 1
    return counts


def main(argv):
    update = "--update" in argv[1:]
    accept_increase = "--accept-increase" in argv[1:]
    data = json.loads(RULES.read_text(encoding="utf-8"))
    failed = False
    raised = False
    for rule in data["rules"]:
        current = count(rule)
        baseline = rule.get("baseline", {})
        total_now, total_before = sum(current.values()), sum(baseline.get(k, 0) for k in current)
        print(f"{rule['name']}: {total_now} (baseline {total_before})")
        for name, value in current.items():
            before = baseline.get(name, 0)
            if value > before:
                print(f"  {name}: {value}, baseline {before} -- new uses; go through the interface instead")
                failed = True
                raised = True
            elif value < before:
                print(f"  {name}: {value}, baseline {before} -- lower: record it with --update")
                failed = True
        rule["baseline"] = current if accept_increase else {k: min(v, baseline.get(k, v)) for k, v in current.items()}
    if update:
        if raised and not accept_increase:
            print("not written: a count went up (see above); --update only records lower counts")
            return 1
        RULES.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8", newline="\n")
        print(f"baselines written to {RULES.relative_to(ROOT).as_posix()}")
        return 0
    print("boundary checks FAILED" if failed else "boundary checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
