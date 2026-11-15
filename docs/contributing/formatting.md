# Formatting

How source files are laid out, and how formatting changes reach the repository.

## The rules

Three files at the repository root hold the conventions:

| File | Covers |
| --- | --- |
| `.editorconfig` | Every editor: UTF-8, LF line endings (CRLF for `*.ps1`, `*.cmd`, `*.sln`, `*.csproj`, `*.rc`, as `.gitattributes` says), a final newline, no trailing whitespace except in Markdown. Indentation: 1 space for C, C++, C# and XAML; 4 for Python; 2 for YAML, CMake and `.csproj`. |
| `.clang-format` | C and C++: LLVM base, 1-space indent, 2-space continuation, 160 columns, braces attached, `T* name`, `if(`, short bodies kept on one line, includes never reordered, comments never rewrapped. |
| `.clang-format-ignore` | What clang-format must never touch: `native/generated_*.hpp`, `ui/catalog.js`, `vendor/`, `third_party/` (read by clang-format 18 and later). |

Include order is significant: some headers are included inside `menu.cpp`'s anonymous namespace and depend on what comes
before them. That is why `SortIncludes` is off; do not reorder includes by hand either.

Generated files are regenerated with their tool under `tools/` (for example `python tools/sync_catalog.py`), never edited
or formatted. `vendor/` and `third_party/` are upstream submodules and stay as published.

## Existing files are not reformatted in bulk

Most native code is written densely, several statements per line with no spaces around operators. clang-format cannot
keep that layout: it puts one statement per line and spaces operators. On `native/game_read.hpp`, clang-format 19.1.5 with
this configuration would rewrite 117 of the 190 lines (the file would grow to 444 lines). A tree-wide run would therefore
touch nearly every line, bury real changes and break `git blame`, so it is not done.

- New files and new code follow `.clang-format`. Run it only on the lines or files you wrote
  (`clang-format -i <file>` on a new file, or your editor's "format selection").
- A file is reformatted only in its own pull request, one file per pull request, with nothing else in it.
- A formatting-only change must be token-identical: the compiler must see the same tokens before and after. A tool that
  checks this is coming in a separate pull request; until then, say how you verified it in the pull request.

## Never mix move, format and fix

A pull request does one of these, never two:

- **move**: code moves between files or functions, unchanged;
- **format**: layout changes only, token-identical;
- **fix** (or feature): behavior changes.

Mixed, a reviewer cannot tell which lines changed behavior. Kept apart, a move or format pull request is checked
mechanically and a fix pull request stays small.
