# Formatting

How source files are laid out, how new code should read, and how formatting changes reach the repository.

## The rules

Three files at the repository root hold the conventions:

| File | Covers |
| --- | --- |
| `.editorconfig` | Every editor: UTF-8, LF line endings (CRLF for `*.ps1`, `*.cmd`, `*.sln`, `*.csproj`, `*.rc`, as `.gitattributes` says), a final newline, no trailing whitespace except in Markdown. Indentation: 4 for Python; 2 for YAML, CMake and `.csproj`; none set for C, C++, C# and XAML (see below). Signed fixtures under `tests/fixtures/` are never rewritten on save. |
| `.clang-format` | C and C++ (clang-format 19): LLVM base, 4-space indent, 8-space continuation, 120 columns, one statement per line, braces on every `if`/`for`/`while` body, a blank line between definitions, one argument per line when a call does not fit, `T* name`, `if (`, includes never reordered, comments never rewrapped. |
| `.clang-format-ignore` | What clang-format must never touch: `native/generated_*.hpp`, `ui/catalog.js`, `vendor/`, `third_party/`. |

Include order is significant: some headers are included inside `menu.cpp`'s anonymous namespace and depend on what comes
before them. That is why `SortIncludes` is off; do not reorder includes by hand either.

Generated files are regenerated with their tool under `tools/` (for example `python tools/sync_catalog.py`), never edited
or formatted. `vendor/` and `third_party/` are upstream submodules and stay as published.

## Why this style

Most of today's native code is written densely: one-space indentation, several statements per line, bodies on the
same line as their `if`, lines up to 160 columns and more. It is compact, but hard to read, to review and to debug: a
diff of one statement shows a whole line of others, a breakpoint cannot stop inside a one-line `if`, and nesting is
hard to see. The target style trades length for readability:

```cpp
bool Parse(const std::string& entry, Entry* out, int limit) {
    if (entry.empty()) {
        return false;
    }
    for (size_t i = 0; i < entry.size(); ++i) {
        if (entry[i] == ' ') {
            ++limit;
        }
    }
    if (limit > 3 && out != nullptr && entry.size() > 10 && entry.front() != '#' && entry.back() != '#'
        && entry.find(',') == std::string::npos) {
        return true;
    }
    std::sort(v.begin(), v.end(), [](int a, int b) { return a < b; });
    return false;
}
```

What clang-format cannot check, reviewers do. New code should also have:

- **Functions that do one thing.** Aim for a function that fits on a screen (about 50 lines). Split a long one into
  named steps rather than adding comments between its parts.
- **Early returns** for guards, so the main path is not nested.
- **Names that say what a value is** (`targetDistance`, not `d2`); short names only for small loop counters.
- **No new globals** shared between unrelated components: pass what a function needs, or keep state in the component
  that owns it.
- **Comments that explain why**, not what the next line does; keep game addresses and the evidence for them.

## Existing files are not reformatted in bulk

The new style changes almost every line of an existing file: on `native/game_read.hpp`, clang-format 19.1.5 with this
configuration rewrites 124 of its 190 lines (the file grows to 644 lines). A tree-wide run would touch nearly every line,
bury real changes and break `git blame`, so it is not done.

- **New files** follow `.clang-format` entirely: run `clang-format -i <file>` before committing.
- **New code in an existing dense file** keeps the file consistent with itself: write new functions in the new style
  as a block (your editor's "format selection" on the lines you wrote), but do not reformat the lines around them.
- **A file is reformatted only in its own pull request**, one file (or one small group of related files) per pull
  request, with nothing else in it, preferably right before that file is refactored.
- **A formatting-only pull request is proven not to change the program**, as `docs/contributing/refactoring.md`
  describes: `tools/same_tokens.py` when only whitespace and line breaks change; because `InsertBraces` adds braces
  (new tokens), a reformat that adds braces is proven by comparing the compiled objects instead
  (`tools/compare_objects.py`).

Where clang-format is installed: Visual Studio 2022 and its Build Tools ship it
(`VC\Tools\Llvm\bin\clang-format.exe`, with the "C++ Clang tools for Windows" component).

C# (the launcher, `desktop/`) is not covered by clang-format. New C# code uses four spaces and the usual .NET layout;
existing files keep theirs until they are reformatted on their own.

## Never mix move, format and fix

A pull request does one of these, never two:

- **move**: code moves between files or functions, unchanged;
- **format**: layout changes only, proven as above;
- **fix** (or feature): behavior changes.

Mixed, a reviewer cannot tell which lines changed behavior. Kept apart, a move or format pull request is checked
mechanically and a fix pull request stays small.
