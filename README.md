# C++ Reviewer

A code-review tool for C++ that combines a side-by-side diff with **semantic analysis
(libclang)**. Beyond showing changed lines, it explains what happened to the code:

| Badge | Change | Example |
|---|---|---|
| `R` | Renamed entity | `distance()` → `euclideanDistance()` |
| `r` | Renamed identifier (consistently, across functions) | `m_points` → `m_vertices` (10 occurrences) |
| `→` | Moved entity (to another file / class / namespace) | `countLarge()` moved from `shapes.cpp` to `stats.cpp` |
| `↕` | Reordered within a file | `perimeter()` moved above `area()` |
| `X` | Extracted function | `appendPoints()` extracted from `describe()` |
| `I` | Inlined function | helper body pasted into its only caller |
| `⇢` | Statements moved between functions | |
| `≡` | Moved lines (text level, like `git --color-moved`, rename-aware) | |
| `S` | Signature changed | `(const T&)` → `(std::ostream&, const T&)` |
| `C` | Duplicated code | new function copies an existing body |

Clicking a change expands the **places related to it** and highlights the relevant identifier
(old name on the base side, new name on the target side; Esc clears it). Click a place to jump there,
double-click a range (extracted code, a hunk, moved code) to diff it against its counterpart:

| Change | Related places |
|---|---|
| Renamed entity / identifier | every occurrence of the rename (declarations, definitions, call sites, moved code) |
| Extracted function | the extracted code ⇄ new function; every call of the new function |
| Inlined function | the inlined body ⇄ where it went; every former call (base) |
| Signature changed | definition, declarations, every call — **⚠ calls on lines that weren't touched** |
| Added / Removed | uses of the new entity / former uses, **⚠ places still referencing a removed name** |
| Modified | each changed hunk inside the entity |
| Moved / reordered / moved code | before ⇄ after |

Usages are found in the changed files. For class members only `obj.name`, `ptr->name`,
`Class::name` and uses inside the class count, so unrelated functions with the same name are skipped.

Lines that differ **only by a detected rename** are tinted purple, so you can skip them. Double-clicking a
change (or clicking its gutter badge) opens a **diff of the code against where it came from**. For
example, a moved function is diffed against its original, even when it was in another file.

Navigation works like an IDE, on both revisions:
**Ctrl+Click / F12** go to definition, **Ctrl+Shift+Click / Ctrl+F12** go to declaration,
hover for type/signature/doc comment, **Alt+Left/Right** (or mouse back/forward) for history.
Definitions in other translation units are found through a symbol index built in the background.

## Inputs

* Any two git revisions (commits, branches, tags, `HEAD~3`, …) picked from a log/branch browser.
* Working tree or index (uncommitted / staged changes).
* GitHub pull requests: listed through the GitHub API (set `GITHUB_TOKEN` for private repos) and fetched
  from `refs/pull/N/head`; the base is the merge base with the PR's target branch.
* Two plain directories (no git needed).

Each commit is exported to `~/.cache/cppreviewer/snapshots/<sha>` so it can be parsed with its own headers.
The repository itself is never modified, apart from `git fetch` for pull requests.

## Build

Requirements: CMake ≥ 3.20, a C++20 compiler, libclang, and Qt 6 (Widgets, Network, Concurrent).

```sh
sudo apt install cmake ninja-build qt6-base-dev libclang-dev   # e.g. libclang-22-dev
cmake -S . -B build -G Ninja
ninja -C build
```

Use `-DLIBCLANG_ROOT=/path/to/llvm` if libclang isn't found, and `-DCPPREVIEW_BUILD_GUI=OFF` to build
only the command-line tool.

## Usage

```sh
build/cppreviewer                          # current repo: uncommitted changes, or the last commit
build/cppreviewer -C ~/src/proj main HEAD  # two revisions
build/cppreviewer HEAD WORKTREE            # also: INDEX
build/cppreviewer --pr 123 [--remote origin] [--base main]
build/cppreviewer --dirs old/ new/
build/cppreviewer --theme dark             # or light / system; also under View → Theme

build/cppreview-cli HEAD~1 HEAD            # the same analysis printed to the terminal
build/cppreview-cli --diff -w HEAD~1 HEAD  # plus colored line diff, ignoring whitespace
```

For accurate results, give the tool a `compile_commands.json` (`-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`).
It's searched in the repo root, `build/`, `build/*/`, `out/` and `cmake-build-*/`, or can be set in
*Review → Set compile_commands.json*. Paths inside it are remapped into each revision's snapshot.
Without one, the tool uses heuristic include paths (`include/`, `src/`, …).

## Architecture

```
src/core/   (no Qt)
  Git, Snapshot       revisions → changed files, materialized trees
  CompileDb           compile_commands.json → per-file libclang arguments
  ClangProject        libclang per revision: tokens, entities, statements; symbolAt(); USR index
  Diff                Myers O(ND) diff in linear space
  SemanticDiff        entity matching (identity → signature → α-renamed body → similarity →
                      member evidence), reorder (weighted LIS), extraction/inlining (statement
                      coverage), moved statements, identifier renames (token alignment votes)
  Review, Session     line diff + moved lines + rename-only lines, orchestration
src/gui/    Qt 6 Widgets front end
src/cli/    terminal front end
```
