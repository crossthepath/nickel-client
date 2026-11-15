# Refactoring without regressions

Large files such as `native/menu.cpp` and `native/overlay.cpp` are split step by step. Most of these steps can be
proven not to change behaviour: if the compiler sees exactly the same tokens before and after, the program is the
same. `tools/same_tokens.py` checks that.

## One kind of change per pull request

A refactoring pull request is exactly one of:

- **Move**: code goes to another file (a new header included at the same place, a block moved between files)
  with no edit to the moved text.
- **Format**: whitespace, line breaks and comments only.
- **Fix**: a change in behaviour, with its own test or reasoning.

Never mix them. A move or a format pull request must show `same tokens` for every translation unit it touches; a fix
inside a move cannot be reviewed, because the diff no longer shows what changed. If a move needs an edit to compile
(an include, a forward declaration, a namespace), make that edit in a separate commit or pull request, explain it, and
keep the move itself token-identical.

## The checker

```sh
python tools/same_tokens.py <rev-a> <rev-b> [translation units...] [--keep]
```

It exports both revisions with `git archive`, runs MSVC's preprocessor (`cl /E`) on each translation unit with the
overlay's flags, includes and defines, tokenizes the output (whitespace, line breaks, comments and `#line` markers
are ignored) and compares the token streams. The default units are the overlay's own sources: `native/overlay.cpp`,
`native/menu.cpp` and `native/media.cpp`. It needs Visual Studio or the Build Tools with the x64 C++ tools (found
through `vswhere`) and the submodules checked out; it does not need a build.

Exit code 0 means every unit is the same, 1 means a unit differs or is missing at one revision, 2 is a usage or tool
error (unknown revision, compiler not found, submodule pointers that differ between the revisions). On a difference
it prints the first differing token with its source file and line at both revisions:

```text
different tokens: native/menu.cpp at token 1459085 (1637994 / 1637994 tokens)
  origin/master (3266c51ca4) native/menu.cpp:407
    ( ( LONG ) lparam , name , >>> 64 <<< ) ) swprintf ( name , 64 ,
  my-branch (c12948352f) native/menu.cpp:407
    ( ( LONG ) lparam , name , >>> 65 <<< ) ) swprintf ( name , 64 ,
```

## Typical use

Commit the move or the format change, then compare it with its base:

```sh
python tools/same_tokens.py origin/master HEAD
python tools/same_tokens.py HEAD~1 HEAD native/menu.cpp
```

Paste the output in the pull request under Tests. `--keep` keeps the preprocessed outputs in a temporary folder to
inspect a difference.

## What it does not prove

- A change outside the listed units: pass every `.cpp` file that includes a changed header, or add the new unit when
  a pull request adds a `.cpp` file (a new unit cannot be compared with a revision where it does not exist).
- Submodule updates: both revisions are preprocessed against the working copy's submodules, so the tool refuses to
  compare revisions whose submodule pointers differ.
- Joining or splitting string literals (`"a" "b"` versus `"ab"`) compiles the same but is reported as a difference;
  so is code that expands `__LINE__` or `__COUNTER__` after it moves.
- The launcher (C#), scripts and data files: they are not C++ and have their own tests.
