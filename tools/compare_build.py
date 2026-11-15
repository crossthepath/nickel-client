"""Builds the overlay DLL at two revisions and compares what the compiler and the linker produced.

python tools/compare_build.py <rev-a> <rev-b> [--work DIR]

Used, after tools/same_tokens.py, for pull requests that only move or reformat code (docs/contributing/refactoring.md).

1. Refuses revisions whose submodule pointers differ (the submodules are taken from the working copy).
2. Checks out rev-a in one detached worktree (default build/compare/tree, reused between runs) and configures one
   build folder for it with /Brepro on the compiler and the linker, so that the same code gives the same bytes.
   The folder must not be under %TEMP%: MSBuild does not track headers read from there (warning MSB8029), so an
   incremental build could keep an object compiled from an older header.
3. Builds the nickel_overlay target at rev-a, then rev-b, then rev-a again, in the same worktree and build folder
   (same paths, so nothing differs because of where the files were). Each build keeps the DLL and its objects.
4. Control: the two rev-a builds must be IDENTICAL (tools/compare_images.py); otherwise the build is not reproducible
   here and nothing can be concluded.
5. Compares every object of rev-a with the one of rev-b (tools/compare_objects.py) and the two DLLs.

Verdict: SAME PROGRAM when every object is IDENTICAL or EQUIVALENT and the DLLs are IDENTICAL or SAME FUNCTIONS;
DIFFERENT otherwise. Exit code 0 SAME PROGRAM, 1 DIFFERENT, 2 tool or build error. Needs CMake and Visual Studio (or
its Build Tools) with the x64 C++ tools, and capstone (pip install capstone) when the DLLs are not byte-identical.
A full build of the overlay takes several minutes; the second and third builds only recompile what changed.
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import compare_images    # noqa: E402
import compare_objects   # noqa: E402

SUBMODULES = ("vendor/imgui", "vendor/minhook", "third_party/luau")


def git(*args, cwd=ROOT, check=True):
    result = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)
    if check and result.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)}: {result.stderr.strip()}")
    return result.stdout.strip()


def submodule_pointers(rev):
    pointers = {}
    for line in git("ls-tree", rev, *SUBMODULES).splitlines():
        meta, path = line.split("\t")
        pointers[path] = meta.split()[2]
    return pointers


def under_temp(path):
    for name in ("TEMP", "TMP"):
        temp = os.environ.get(name)
        if temp:
            try:
                path.resolve().relative_to(Path(temp).resolve())
                return True
            except ValueError:
                pass
    return False


def prepare(tree, rev):
    if not (tree / ".git").exists():
        tree.parent.mkdir(parents=True, exist_ok=True)
        git("worktree", "add", "--detach", str(tree), rev)
        overrides = []
        for sub in SUBMODULES:
            overrides += ["-c", f"submodule.{sub}.url={(ROOT / sub).as_posix()}"]
        git("-c", "protocol.file.allow=always", *overrides, "submodule", "update", "--init", cwd=tree)
    else:
        git("checkout", "--quiet", "--detach", rev, cwd=tree)
        git("submodule", "update", cwd=tree)


def build(tree, folder, keep):
    env = dict(os.environ, CFLAGS="/Brepro", CXXFLAGS="/Brepro", LDFLAGS="/Brepro")
    if not (folder / "CMakeCache.txt").exists():
        subprocess.run(["cmake", "-S", str(tree), "-B", str(folder)], env=env, check=True, capture_output=True)
    result = subprocess.run(["cmake", "--build", str(folder), "--config", "Release", "--target", "nickel_overlay",
                             "--parallel"], env=env, capture_output=True, text=True)
    if result.returncode != 0:
        tail = "\n".join(result.stdout.splitlines()[-20:])
        raise RuntimeError(f"build failed:\n{tail}")
    if keep.exists():
        shutil.rmtree(keep)
    keep.mkdir(parents=True)
    shutil.copy2(folder / "Release" / "Nickel.Overlay.dll", keep)
    for obj in (folder / "nickel_overlay.dir" / "Release").glob("*.obj"):
        shutil.copy2(obj, keep)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("rev_a")
    parser.add_argument("rev_b")
    parser.add_argument("--work", type=Path, default=ROOT / "build" / "compare")
    options = parser.parse_args()
    work = options.work.resolve()
    if under_temp(work):
        print(f"error: {work} is under %TEMP%, where MSBuild does not track headers; choose another --work folder")
        return 2
    try:
        a, b = git("rev-parse", options.rev_a), git("rev-parse", options.rev_b)
        if submodule_pointers(a) != submodule_pointers(b):
            print("error: the revisions point at different submodule commits")
            return 2
        tree, folder, out = work / "tree", work / "build", work / "out"
        for label, rev in (("a", a), ("b", b), ("a-again", a)):
            print(f"building {label} ({rev[:10]})", flush=True)
            prepare(tree, rev)
            build(tree, folder, out / label)
        print("control: rev-a built twice")
        control = compare_images.compare(str(out / "a" / "Nickel.Overlay.dll"),
                                         str(out / "a-again" / "Nickel.Overlay.dll"))
        print(f"  {control}")
        if control != "IDENTICAL":
            print("error: two builds of rev-a differ, so this machine does not build reproducibly")
            return 2
        same = True
        for obj in sorted((out / "a").glob("*.obj")):
            other = out / "b" / obj.name
            if not other.exists():
                print(f"{obj.name}: missing at rev-b")
                same = False
                continue
            verdict = compare_objects.compare(str(obj), str(other), out=lambda line: print(line))
            print(f"{obj.name}: {verdict}")
            same &= verdict in ("IDENTICAL", "EQUIVALENT")
        extra = {p.name for p in (out / "b").glob("*.obj")} - {p.name for p in (out / "a").glob("*.obj")}
        for name in sorted(extra):
            print(f"{name}: only at rev-b")
            same = False
        print("Nickel.Overlay.dll:")
        image = compare_images.compare(str(out / "a" / "Nickel.Overlay.dll"), str(out / "b" / "Nickel.Overlay.dll"))
        print(f"  {image}")
        same &= image in ("IDENTICAL", "SAME FUNCTIONS")
    except (RuntimeError, subprocess.CalledProcessError, OSError) as error:
        print(f"error: {error}")
        return 2
    print("SAME PROGRAM" if same else "DIFFERENT")
    return 0 if same else 1


if __name__ == "__main__":
    sys.exit(main())
