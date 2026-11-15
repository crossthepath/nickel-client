"""Proves that a change to Nickel's C++ sources is token-identical once preprocessed.

python tools/same_tokens.py <rev-a> <rev-b> [translation units...] [--keep]
  default units: native/overlay.cpp native/menu.cpp native/media.cpp (the overlay DLL's own sources)
  --keep  keep the temporary folder (exported trees are removed, preprocessed outputs stay) and print its path

Used for pull requests that only move code to another file or only reformat it: if every translation unit gives the
same tokens at both revisions, the compiler parses the same program text. That is the first check, not the whole
proof: file names and line numbers are not tokens, and MSVC derives lambda type names from the file, so a move can
change the compiled DLL (see docs/contributing/refactoring.md, then run tools/compare_build.py).

1. Resolves both revisions and refuses to compare them when a submodule (vendor/imgui, vendor/minhook,
   third_party/luau) points at different commits: git archive does not export submodules, so both revisions are
   preprocessed against the working copy's submodule folders, which only proves something when the pointers agree.
2. Exports each revision with git archive into the same temporary folder, one after the other, so absolute paths
   seen by the preprocessor are the same for both. The working copy's submodule folders are linked into the exported
   tree with directory junctions (sources include some submodule files by relative path); the junctions are unlinked
   before the tree is deleted.
3. Runs MSVC's preprocessor on each unit with the nickel_overlay target's flags (CMakeLists.txt): /std:c++17, the
   defines NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE plus CMake's Release defaults (WIN32 _WINDOWS NDEBUG), /EHa,
   /MT and /GR (they set _CPPUNWIND, _MT and _CPPRTTI), and the include folders native, vendor/imgui,
   vendor/minhook/include and the Luau VM, Compiler and Common include folders. No generated header from the build
   folder is included by these units (it only holds the resource script), so no build is needed.
   cl.exe is found through vswhere and vcvars64.bat: the vcvars64 environment is captured once with
   cmd /c call vcvars64.bat && set, then cl runs in it with its output written to a file.
   /E is used instead of /EP: it is the same output plus #line directives, which are dropped from the comparison
   but give the source file and line of a difference.
4. Tokenizes both outputs as C++ (identifiers, pp-numbers, string and character literals with escapes and prefixes,
   raw strings, punctuators); whitespace, line breaks, #line directives and #pragma once are ignored (a header that
   gets #pragma once when code moves into it compiles the same); other #pragma lines are compared.
5. Prints "same tokens: <unit>" or the first differing token with context and source location on both sides.
Exit code: 0 when every unit is the same, 1 when one differs or is missing at a revision, 2 on a usage or tool error.

Limits: a change that adds a new .cpp file or touches only files outside the listed units proves nothing here (list
the extra units); joining or splitting string literals ("a" "b" versus "ab") is reported as a difference although it
compiles the same; __LINE__ and __COUNTER__ are expanded, so code using them may differ after a move. Python 3
standard library only.
"""
import argparse
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from array import array
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_UNITS = ['native/overlay.cpp', 'native/menu.cpp', 'native/media.cpp']
SUBMODULES = ['vendor/imgui', 'vendor/minhook', 'third_party/luau']
DEFINES = ['WIN32', '_WINDOWS', 'NDEBUG', 'NOMINMAX', 'WIN32_LEAN_AND_MEAN', 'UNICODE', '_UNICODE']
FLAGS = ['/nologo', '/E', '/std:c++17', '/EHa', '/MT', '/GR']
TREE_INCLUDES = ['native']  # from the exported revision
SUBMODULE_INCLUDES = ['vendor/imgui', 'vendor/minhook/include', 'third_party/luau/VM/include',
                      'third_party/luau/Compiler/include', 'third_party/luau/Common/include']  # junctions to the working copy
CONTEXT = 8  # tokens shown on each side of a difference

TOKEN = re.compile(r'''
    (?P<line>^[ \t]*\#[ \t]*line[ \t]+(?P<lnum>\d+)(?:[ \t]+"(?P<lfile>(?:[^"\\\n]|\\.)*)")?[^\n]*)
  | (?P<once>^[ \t]*\#[ \t]*pragma[ \t]+once\b[^\n]*)
  | (?P<ws>\s+)
  | (?P<raw>(?:u8|u|U|L)?R"(?P<delim>[^\s()\\]{0,16})\([\s\S]*?\)(?P=delim)"(?:[A-Za-z_]\w*)?)
  | (?P<str>(?:u8|u|U|L)?"(?:[^"\\\n]|\\.)*"(?:[A-Za-z_]\w*)?)
  | (?P<chr>(?:u8|u|U|L)?'(?:[^'\\\n]|\\.)*'(?:[A-Za-z_]\w*)?)
  | (?P<num>\.?\d(?:[eEpP][+-]|'(?=\w)|[\w.])*)
  | (?P<id>[A-Za-z_$][\w$]*)
  | (?P<punct>%:%:|\.\.\.|<=>|->\*|<<=|>>=|::|->|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||[-+*/%&|^]=|\#\#|\.\*|<:|:>|<%|%>|%:)
  | (?P<other>.)
''', re.M | re.X)


class ToolError(Exception):
    pass


def git(*args, binary=False):
    r = subprocess.run(['git', *args], cwd=ROOT, capture_output=True)
    if r.returncode:
        raise ToolError(f'git {" ".join(args)} failed: {r.stderr.decode(errors="replace").strip()}')
    return r.stdout if binary else r.stdout.decode().strip()


def resolve(rev):
    r = subprocess.run(['git', 'rev-parse', '--verify', '--quiet', f'{rev}^{{commit}}'], cwd=ROOT,
                       capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else ''


def submodule_pointers(commit):
    """Gitlink commit of each submodule at a revision (None when the revision has no such submodule)."""
    out = {}
    for sub in SUBMODULES:
        line = git('ls-tree', commit, sub)
        out[sub] = line.split()[2] if line.startswith('160000') else None
    return out


def msvc_environment():
    base = os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')
    vswhere = Path(base) / 'Microsoft Visual Studio' / 'Installer' / 'vswhere.exe'
    if not vswhere.is_file():
        raise ToolError(f'vswhere not found at {vswhere}: install Visual Studio or its Build Tools')
    r = subprocess.run([str(vswhere), '-latest', '-products', '*', '-requires',
                        'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'],
                       capture_output=True, text=True)
    install = r.stdout.strip().splitlines()[0] if r.stdout.strip() else ''
    vcvars = Path(install) / 'VC' / 'Auxiliary' / 'Build' / 'vcvars64.bat'
    if not install or not vcvars.is_file():
        raise ToolError('no Visual Studio installation with the x64 C++ tools found by vswhere')
    # /s strips the outer quotes, leaving: call "<vcvars64.bat>" >nul && set
    r = subprocess.run(f'cmd /d /s /c "call "{vcvars}" >nul && set"', capture_output=True)
    if r.returncode:
        raise ToolError(f'{vcvars} failed: {r.stderr.decode(errors="replace").strip()}')
    env = {}
    for line in r.stdout.decode(errors='replace').splitlines():
        key, sep, value = line.partition('=')
        if sep and key:
            env[key.upper()] = value
    cl = shutil.which('cl', path=env.get('PATH', ''))
    if not cl:
        raise ToolError(f'cl.exe not on the PATH set by {vcvars}')
    return env, cl


def is_link(path):
    """True for a symbolic link or an NTFS junction (os.path.isjunction exists from Python 3.12)."""
    if os.path.islink(path):
        return True
    if hasattr(os.path, 'isjunction'):
        return os.path.isjunction(path)
    try:
        os.readlink(path)
        return True
    except OSError:
        return False


def remove_tree(tree):
    """Deletes an exported tree; the submodule junctions are unlinked first so the working copy is never touched."""
    for sub in SUBMODULES:
        link = tree / sub
        if os.path.lexists(link) and is_link(link):
            os.rmdir(link)  # removes the junction itself, not the folder it points at
    if tree.exists():
        shutil.rmtree(tree)


def export(commit, tree):
    remove_tree(tree)
    tree.mkdir(parents=True)
    with tarfile.open(fileobj=io.BytesIO(git('archive', '--format=tar', commit, binary=True))) as tar:
        try:
            tar.extractall(tree, filter='data')
        except TypeError:  # Python before 3.12 has no extraction filters
            tar.extractall(tree)
    # Sources also include submodule files by relative path (../vendor/minhook/src/hde/hde64.h): junctions make the
    # working copy's submodule folders appear at their place in the exported tree.
    for sub in SUBMODULES:
        link = tree / sub
        if link.is_dir():
            link.rmdir()  # git archive may leave an empty folder for a submodule
        link.parent.mkdir(parents=True, exist_ok=True)
        r = subprocess.run(['cmd', '/d', '/c', 'mklink', '/J', str(link), str(ROOT / sub)], capture_output=True)
        if r.returncode:
            raise ToolError(f'cannot link {sub} into the exported tree: {r.stderr.decode(errors="replace").strip()}')


def preprocess(env, cl, tree, unit, out_path):
    args = [cl, *FLAGS, *[f'/D{d}' for d in DEFINES]]
    args += [f'/I{tree / d}' for d in TREE_INCLUDES] + [f'/I{tree / d}' for d in SUBMODULE_INCLUDES]
    args.append(unit.replace('/', '\\'))
    with open(out_path, 'wb') as out:
        r = subprocess.run(args, cwd=tree, env=env, stdout=out, stderr=subprocess.PIPE)
    if r.returncode:
        text = r.stderr.decode(errors='replace').strip().splitlines()
        raise ToolError(f'cl failed on {unit} (exit {r.returncode}):\n' + '\n'.join(text[-20:]))


class Tokens:
    """Token texts of one preprocessed unit, with the source file and line of each token."""

    def __init__(self, text, tree):
        self.texts, self.file_of, self.line_of, self.files = [], array('i'), array('i'), []
        prefix = str(tree).lower().rstrip('\\') + '\\'
        index, file_idx, line = {}, -1, 1
        for m in TOKEN.finditer(text):
            kind, value = m.lastgroup, m.group()
            if kind == 'ws':
                line += value.count('\n')
                continue
            if kind == 'once':  # cl /E keeps #pragma once; it changes nothing once the header is included
                continue
            if kind == 'line':
                if m.group('lfile') is not None:
                    name = re.sub(r'\\(.)', r'\1', m.group('lfile'))
                    if name.lower().startswith(prefix):
                        name = name[len(prefix):]
                    name = name.replace('\\', '/')
                    if name not in index:
                        index[name] = len(self.files); self.files.append(name)
                    file_idx = index[name]
                line = int(m.group('lnum')) - 1  # the directive names the next line
                continue
            self.texts.append(value); self.file_of.append(file_idx); self.line_of.append(line)
            line += value.count('\n')

    def where(self, i):
        if i >= len(self.texts):
            return 'end of unit'
        f = self.file_of[i]
        return f'{self.files[f] if f >= 0 else "?"}:{self.line_of[i]}'

    def around(self, i):
        before = ' '.join(self.texts[max(0, i - CONTEXT):i])
        here = self.texts[i] if i < len(self.texts) else '<end>'
        after = ' '.join(self.texts[i + 1:i + 1 + CONTEXT])
        return f'{before} >>> {here} <<< {after}'.strip()


def first_difference(a, b):
    n = min(len(a), len(b))
    lo = 0
    step = 1 << 16  # compare large slices first, then walk the differing one
    while lo < n:
        hi = min(n, lo + step)
        if a[lo:hi] != b[lo:hi]:
            return next(i for i in range(lo, hi) if a[i] != b[i])
        lo = hi
    return n if len(a) != len(b) else None


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('rev_a')
    parser.add_argument('rev_b')
    parser.add_argument('units', nargs='*', default=DEFAULT_UNITS)
    parser.add_argument('--keep', action='store_true', help='keep the preprocessed outputs')
    args = parser.parse_args()
    try:
        commits = []
        for rev in (args.rev_a, args.rev_b):
            c = resolve(rev)
            if not c:
                raise ToolError(f'unknown revision: {rev}')
            commits.append(c)
        pointers = [submodule_pointers(c) for c in commits]
        for sub in SUBMODULES:
            if pointers[0][sub] != pointers[1][sub]:
                raise ToolError(f'{sub} points at {pointers[0][sub]} in {args.rev_a} and {pointers[1][sub]} in '
                                f'{args.rev_b}: a submodule update cannot be proven with this tool')
            if not (ROOT / sub).is_dir() or not any((ROOT / sub).iterdir()):
                raise ToolError(f'{sub} is empty: run git submodule update --init')
            head = subprocess.run(['git', '-C', str(ROOT / sub), 'rev-parse', 'HEAD'], capture_output=True, text=True)
            if pointers[0][sub] and head.stdout.strip() != pointers[0][sub]:
                print(f'note: the working copy of {sub} is not at the commit both revisions use; '
                      f'both are preprocessed against the working copy')
        units = [u.replace('\\', '/') for u in args.units]
        env, cl = msvc_environment()
        work = Path(tempfile.mkdtemp(prefix='same_tokens_'))
        tree = work / 'tree'
        outputs = [{}, {}]
        try:
            for side, commit in enumerate(commits):
                export(commit, tree)
                for unit in units:
                    if not (tree / unit).is_file():
                        outputs[side][unit] = None
                        continue
                    out_path = work / ('a', 'b')[side] / (unit.replace('/', '_') + '.i')
                    out_path.parent.mkdir(exist_ok=True)
                    preprocess(env, cl, tree, unit, out_path)
                    outputs[side][unit] = out_path
            different = 0
            for unit in units:
                missing = [rev for rev, side in zip((args.rev_a, args.rev_b), outputs) if side[unit] is None]
                if missing:
                    print(f'missing: {unit} does not exist in {" and ".join(missing)}')
                    different += 1
                    continue
                toks = [Tokens(side[unit].read_bytes().decode('utf-8', 'surrogateescape'), tree) for side in outputs]
                i = first_difference(toks[0].texts, toks[1].texts)
                if i is None:
                    print(f'same tokens: {unit} ({len(toks[0].texts)} tokens)')
                    continue
                different += 1
                print(f'different tokens: {unit} at token {i} ({len(toks[0].texts)} / {len(toks[1].texts)} tokens)')
                for rev, commit, t in zip((args.rev_a, args.rev_b), commits, toks):
                    print(f'  {rev} ({commit[:10]}) {t.where(i)}\n    {t.around(i)}')
        finally:
            remove_tree(tree)  # junctions first: the working copy's submodules are never deleted
            if args.keep:
                print(f'preprocessed outputs kept in {work}')
            else:
                shutil.rmtree(work, ignore_errors=True)
    except ToolError as e:
        print(f'error: {e}', file=sys.stderr)
        return 2
    except Exception as e:  # any other failure is a tool error (2), never "different" (1)
        print(f'error: {type(e).__name__}: {e}', file=sys.stderr)
        return 2
    return 1 if different else 0


if __name__ == '__main__':
    sys.exit(main())
