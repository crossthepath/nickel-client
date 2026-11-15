# Refactoring without regressions

Large files such as `native/menu.cpp` and `native/overlay.cpp` are split step by step. A step that only moves or
reformats code must be shown not to change the program, and no single check shows that on its own. This page gives
the checks, what each one proves, what it does not, and the method that combines them.

## One kind of change per pull request

A refactoring pull request is exactly one of:

- **Move**: code goes to another file (a new header included at the same place, a block moved between files)
  with no edit to the moved text.
- **Format**: whitespace, line breaks and comments only (or braces added by clang-format, see
  `docs/contributing/formatting.md`).
- **Fix**: a change in behaviour, with its own test or reasoning.

Never mix them: a fix inside a move cannot be reviewed, because the diff no longer shows what changed. If a move needs
an edit to compile (an include, a forward declaration, a namespace), make that edit in a separate commit or pull
request, explain it, and keep the move itself mechanical.

A move is not yet an architectural improvement: a header that still reads `menu.cpp`'s globals and helpers is the same
component in two files. Pull requests that make a component independent (its own interface, its state passed in
rather than shared) change code on purpose; they are **fix**-type changes with their own tests, and come after the
move that isolated the code.

## Boundaries between components

`tools/check_boundaries.py` (CTest `boundary_checks`) counts, outside comments and strings, the names one component
uses from another, as listed in `tools/boundaries.json`: for example `menu.cpp` reaching into the Luau engine's script
list, or the engine reading the menu's globals. A count may never go up; when a pull request lowers one, it records the
new baseline with `python tools/check_boundaries.py --update`, so the improvement is visible in the diff and cannot be
undone silently. A pull request that makes a component more independent says which counts it lowered.

## The checks

### 1. Tokens: `tools/same_tokens.py`

```sh
python tools/same_tokens.py <rev-a> <rev-b> [translation units...] [--keep]
```

It exports both revisions with `git archive`, runs MSVC's preprocessor (`cl /E`) on each translation unit with the
overlay's flags, includes and defines, tokenizes the output (whitespace, line breaks, comments and `#line` markers are
ignored) and compares the token streams. The default units are the overlay's own sources: `native/overlay.cpp`,
`native/menu.cpp` and `native/media.cpp`. It needs Visual Studio or the Build Tools with the x64 C++ tools (found
through `vswhere`) and the submodules checked out; it does not need a build. Exit code 0 means every unit is the
same, 1 that a unit differs or is missing at one revision, 2 a usage or tool error. On a difference it prints the
first differing token with its source file and line at both revisions:

```text
different tokens: native/menu.cpp at token 1459085 (1637994 / 1637994 tokens)
  origin/master (3266c51ca4) native/menu.cpp:407
    ( ( LONG ) lparam , name , >>> 64 <<< ) ) swprintf ( name , 64 ,
  my-branch (c12948352f) native/menu.cpp:407
    ( ( LONG ) lparam , name , >>> 65 <<< ) ) swprintf ( name , 64 ,
```

**What identical tokens prove**: the compiler parses the same program text, with every macro already expanded at
its new place. A mistake in the move (a lost line, a changed include order, a macro that expands differently) shows
up here, quickly, without a build.

**What they do not prove**:

- **The same binary.** The compiler also sees file names and line numbers, which tokens ignore. MSVC names each
  lambda's closure type `<lambda_HASH>` with a hash that depends on the source file: when the waypoint manager moved
  from `menu.cpp` to `menu_waypoints.hpp` (pull request #24), the tokens were identical, but seven lambda types got new
  names, the linker laid the functions out in another order, and `.text`, `.rdata` and `.pdata` of the DLL differed.
  The program was the same; the bytes were not. `__FILE__`, `__LINE__`, `std::source_location`, `assert` messages and
  debug information depend on file and line in the same way.
- **Complete equivalence of behaviour.** Tokens say nothing about code outside the listed units (pass every `.cpp`
  that includes a changed header; a new `.cpp` cannot be compared with a revision where it does not exist), about
  build settings (CMake, compiler flags, link order), about generated files and resources, or about submodule updates
  (the tool refuses revisions whose submodule pointers differ). A different layout of the DLL also moves every
  internal address, which matters to anything that depends on addresses inside Nickel itself.
- Joining or splitting string literals (`"a" "b"` versus `"ab"`) compiles the same but is reported as a difference;
  so is code that expands `__LINE__` or `__COUNTER__` after it moves.
- The launcher (C#), scripts and data files: they are not C++ and have their own tests.

### 2. Macros and dependencies (review)

Before running anything, read the moved text and its new place:

- the moved code defines or tests no macro (`#define`, `#undef`, `#if`), and uses no `__FILE__`, `__LINE__`,
  `__COUNTER__`, `__DATE__`, `__TIME__`, `assert` or `std::source_location`; if it does, say what changes;
- the new header is included at the same point as the code was (same namespace, same declarations before it), and
  only by the units that had the code: `git grep -n '<header name>'`;
- the include order of every unit is unchanged; nothing that comes after the include point is used by the moved code;
- `#pragma` lines (`once`, `pack`, `warning`) are not added around code whose layout or warnings they would change.

### 3. Compiled output: `tools/compare_build.py`

```sh
python tools/compare_build.py <rev-a> <rev-b> [--work DIR]
```

It builds the overlay DLL at rev-a, rev-b and rev-a again with `/Brepro` (reproducible output) in one worktree and one
build folder (default `build/compare`; never under `%TEMP%`, where MSBuild does not track headers), then:

- **control**: the two rev-a builds must be byte-identical outside the debug data, otherwise nothing can be
  concluded on this machine;
- **objects** (`tools/compare_objects.py`): every section except `.debug$*` and `.chks64` (size, characteristics,
  COMDAT selection, bytes and relocations resolved to symbol names) and the whole symbol table. Lambda types renamed
  by a move are paired by signature and accepted (EQUIVALENT); any other difference is not;
- **DLL** (`tools/compare_images.py`): byte-identical sections outside time stamps and debug data (IDENTICAL), or
  else the same functions and data in another layout (SAME FUNCTIONS): same base relocations and `.data`, every
  function's body equal once addresses are normalized (needs `pip install capstone`), every remaining `.rdata`
  difference a code address of the new layout.

The verdict is SAME PROGRAM or DIFFERENT. The objects carry the proof that every call and every data reference still
reaches the same symbol; the DLL check shows that the linker produced nothing else.

The two comparison tools also run on their own, for example on objects or DLLs kept from earlier builds:

```sh
python tools/compare_objects.py a/menu.obj b/menu.obj
python tools/compare_images.py a/Nickel.Overlay.dll b/Nickel.Overlay.dll
```

### 4. Tests and the game

- All offline tests (`ctest --test-dir build -C Release`), the launcher checks and the launcher's `--self-test`, as in
  CI.
- When the DLL is not IDENTICAL, its internal addresses moved: nothing in Nickel should depend on them (hooks target
  Minecraft, not Nickel), but a code path that does would show up only in the game. A move of code that runs in hooks
  or on the render thread is loaded once in Minecraft 1.26.62 and 1.26.63, in instances and data folders used only for
  testing, before merging.

## Method for a move or format pull request

1. Commit the move (or the format change) alone.
2. `python tools/same_tokens.py origin/master HEAD`: every unit `same tokens`.
3. Review macros and dependencies (section 2) and write the result in the pull request.
4. `python tools/compare_build.py origin/master HEAD`: SAME PROGRAM; paste the summary.
5. Offline tests and launcher checks green; game check when section 4 asks for it.
6. Before merging, if master moved since step 4, repeat steps 2 and 4 against the new master.

Paste the outputs in the pull request under Tests. A step that does not apply (for example a format pull request
whose tokens differ only by braces added by clang-format, where step 2 reports a difference and step 4 is the proof)
is named and explained, never skipped silently.
