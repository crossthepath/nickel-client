"""Fail when a build input is not part of the repository.

Every file embedded by native/resources.rc.in and every file the launcher project embeds must be tracked by git, so a
fresh clone builds the same thing as a developer machine (an ignored local file once broke the CI build).

  python tools/check_tracked_inputs.py
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def tracked(relative):
    result = subprocess.run(["git", "ls-files", "--error-unmatch", relative], cwd=ROOT, capture_output=True, text=True)
    return result.returncode == 0


def main():
    inputs = []
    for line in (ROOT / "native" / "resources.rc.in").read_text(encoding="utf-8").splitlines():
        match = re.search(r'"@CMAKE_SOURCE_DIR@/([^"]+)"', line)
        if match:
            inputs.append(match.group(1))
    project = (ROOT / "desktop" / "Nickel.Desktop.csproj").read_text(encoding="utf-8")
    for pattern in re.findall(r'(?:Resource|EmbeddedResource) Include="([^"]+)"', project):
        if pattern.startswith("../build/"):
            continue   # build output (the overlay DLL), produced by the native build
        base = (ROOT / "desktop" / pattern).resolve()
        if "*" in pattern:
            matches = sorted(base.parent.glob(base.name))
            if not matches:
                inputs.append(str(Path("desktop") / pattern))   # reported as missing below
            inputs += [str(m.relative_to(ROOT)).replace("\\", "/") for m in matches]
        else:
            inputs.append(str(base.relative_to(ROOT)).replace("\\", "/"))
    missing = [i for i in inputs if not (ROOT / i).is_file() or not tracked(i)]
    if missing:
        print("build inputs missing from the repository (absent or not tracked by git):")
        for m in missing:
            print("  " + m)
        return 1
    print(f"{len(inputs)} build inputs tracked by git")
    return 0


if __name__ == "__main__":
    sys.exit(main())
