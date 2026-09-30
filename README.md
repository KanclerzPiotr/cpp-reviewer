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

## Choosing what to compare

Started without revisions on the command line, the GUI opens the **New Comparison** dialog (also *File →
New Comparison…*, Ctrl+N). Its first tab, **Recent Sessions**, lists earlier reviews. A review is saved when
you quit or start another one, with what was compared, the current file and line, and the open file tabs.
*Resume* reopens it exactly (the same commits, no fetching). For pull requests, *Resume with Latest
Changes* fetches them again.

On the **New Comparison** tab, pick a repository and one of:

* **Two revisions:** type them (`HEAD~1`, `main`, a tag or sha, `WORKTREE` for uncommitted changes, `INDEX`
  for staged ones) or *Browse…* the log, branches and tags. Presets fill in uncommitted, staged, last commit,
  or the current branch against its merge base with the default branch. The last comparison of each
  repository is remembered.
* **A GitHub pull request:** paste its link (`https://github.com/owner/repo/pull/123`, anything after the
  number such as `/files` is ignored), or pick one from the list of open PRs. A link on the clipboard is
  filled in when the dialog opens. See [Pull requests](#pull-requests).
* **Two pull requests**, e.g. the same change in a private and a public repository. See
  [Comparing two pull requests](#comparing-two-pull-requests).
* **Two plain directories** (no git needed).

Cancelling the dialog at startup quits.

## Pull requests

A pull request is fetched from `refs/pull/N/head` of the remote pointing to its repository. If no remote
does, it's fetched from the link itself. It's compared like GitHub's *Files changed*: against the merge base
of the PR and its target branch as GitHub recorded it, so merged PRs work too. The target branch comes from:

1. the GitHub CLI (`gh pr view`), when `gh` is installed and logged in (`gh auth login`);
2. otherwise `refs/pull/N/merge`, which exists for open, mergeable PRs;
3. otherwise the remote's default branch. This can be wrong for closed PRs; pass `--base` then.

Titles and the list of open PRs come from the GitHub API (github.com or GitHub Enterprise). It authenticates
with `$GITHUB_TOKEN`, `$GH_TOKEN`, or the `gh` login, which private repositories need.

```sh
cppreviewer https://github.com/owner/repo/pull/123       # or: --pr <link>, --pr 123 [--remote origin]
cppreview-cli https://github.com/owner/repo/pull/123/files
```

Fetching is the only thing the tool does to the repository: it adds the PR's objects and updates `FETCH_HEAD`.

## Comparing two pull requests

Paste two pull request links, possibly from different repositories. Two views, switched with *Interdiff* /
*Final Files* in the toolbar:

* **Interdiff** (default): for each file, what pull request A changed next to what B changed, as patches.
  A change made the same way in both is shown as unchanged. A hunk that differs, or exists on one side
  only, is highlighted. Files are marked `=` (same change), `M` (changed differently), `D` (only A) or
  `A` (only B). Code unrelated to the two changes, like private-only code, doesn't show up.
* **Final Files**: the files as they are after A and after B, as a normal review with semantic analysis and
  navigation. Differences between the repositories show up here too.

Files are matched by the longest common path suffix, e.g. `src/lib/…` in one repository and `lib/…` in the
other. The guessed mapping is shown in the status bar and can be set in the dialog (`src/lib/=lib/`).
A pull request is fetched into the chosen repository when one of its remotes points to the PR's repository.
Otherwise it goes into a bare download cache, fetching just the two commits needed when the GitHub CLI is
available.

```sh
cppreviewer https://github.com/org/private/pull/123 https://github.com/org/public/pull/45
cppreview-cli --prs LINK_A LINK_B [--map src/lib/=lib/] [--final] [--diff]
```

## Navigating

Navigation works like an IDE, on both revisions:
**Ctrl+Click / F12** go to definition, **Ctrl+Shift+Click / Ctrl+F12** go to declaration,
hover for type/signature/doc comment, **Alt+Left/Right** (or mouse back/forward) for history.
Targets outside the changed files, including headers outside the repository, open in their own tab.
Definitions in other translation units are found through a symbol index built in the background. With a
`compile_commands.json`, the index covers the files it builds; other files still open and resolve on demand.
The index is cached per file (by its content and compile flags). Another review of the same repository
only parses the files that changed, which takes seconds instead of minutes; the status bar shows how
many files came from the cache.

**Ctrl+P** opens any file of either revision, not only the changed ones. Type parts of the path
(`lowering hpp`), optionally followed by `:line`. *Open Whole File in Tab* in a diff's context menu opens the
file under the cursor.

## Searching

* **Ctrl+F** searches the current view: Enter / F3 for the next match, Shift+Enter / Shift+F3 for the
  previous one. Options match case and whole words. Matches are highlighted on both sides of a diff.
  Hunks folded as reviewed aren't searched.
* **Ctrl+Shift+F** searches in files: the changed files, or every file of the target or base revision, as
  plain text or regex. Click a result to go there.

## Bookmarks and comments

**Ctrl+K** bookmarks the current line, **Ctrl+Shift+K** adds or edits a comment on it (both are also in the
context menu). Comments are shown at the end of their line. **F2 / Shift+F2** step through bookmarks, and
the *Bookmarks* dock lists them all. *Copy as Markdown* puts every bookmark on the clipboard with its code
line and comment, ready to paste into a PR review.

Bookmarks are saved per repository and follow their line when surrounding code moves. When the line is gone
from the revisions under review, the bookmark is kept and shown with ⚠.

## Hiding what you've already reviewed

* **Hunks:** **Ctrl+Enter** marks the hunk at the cursor as reviewed and jumps to the next one (or use
  *Mark Hunk as Reviewed* in the context menu). Reviewed hunks fold into a single `✓ reviewed` row;
  double-click it to show the code again.
* **Semantic changes:** tick the checkbox of a change to mark all of its hunks, including the lines inside
  its range such as the code extracted from a modified function. *Hide reviewed* removes ticked changes
  from the list.
* The file list shows `✓` once every hunk of a file is reviewed. *Review → Show Reviewed Hunks* unfolds
  everything.

Marks are keyed by content (path and changed text), not by commit. They survive restarts and carry over
when you review a newer revision of the same branch or PR, and a hunk reappears as soon as its code changes.

## Keyboard shortcuts

| Keys | Action |
|---|---|
| Ctrl+N | New comparison |
| F5 | Refresh the review |
| F8 / Shift+F8 | Next / previous change |
| Alt+Down / Alt+Up | Next / previous file |
| F12 / Ctrl+Click | Go to definition |
| Ctrl+F12 / Ctrl+Shift+Click | Go to declaration |
| Alt+Left / Alt+Right | Back / forward |
| Ctrl+P | Open any file |
| Ctrl+F, F3 / Shift+F3 | Find in view, next / previous match |
| Ctrl+Shift+F | Find in files |
| Ctrl+K / Ctrl+Shift+K | Toggle bookmark / comment |
| F2 / Shift+F2 | Next / previous bookmark |
| Ctrl+Enter | Mark hunk as reviewed and go to the next one |
| Esc | Clear highlights, close the find bar |

## Build

Requirements: CMake ≥ 3.20, a C++20 compiler, libclang, and Qt 6 (Widgets, Network, Concurrent).

```sh
sudo apt install cmake ninja-build qt6-base-dev libclang-dev   # e.g. libclang-22-dev
cmake -S . -B build -G Ninja
ninja -C build
```

Use `-DLIBCLANG_ROOT=/path/to/llvm` if libclang isn't found, and `-DCPPREVIEW_BUILD_GUI=OFF` to build
only the command-line tool.

To always run the latest build from `PATH`, symlink the binaries instead of copying them:

```sh
ln -sf "$PWD/build/cppreviewer" "$PWD/build/cppreview-cli" ~/bin/
```

## Usage

```sh
build/cppreviewer                          # asks what to compare
build/cppreviewer -C ~/src/proj main HEAD  # two revisions
build/cppreviewer HEAD WORKTREE            # also: INDEX
build/cppreviewer https://github.com/owner/repo/pull/123   # a pull request link
build/cppreviewer --pr 123 [--remote origin] [--base main]
build/cppreviewer --dirs old/ new/
build/cppreviewer --theme dark             # or light / system; also under View → Theme

build/cppreview-cli HEAD~1 HEAD            # the same analysis printed to the terminal
build/cppreview-cli --diff -w HEAD~1 HEAD  # plus colored line diff, ignoring whitespace
build/cppreview-cli -p build/compile_commands.json --at new:src/foo.cpp:120:14 HEAD~1 HEAD
                                           # symbol at a position: kind, type, definition, declaration
```

### compile_commands.json

For accurate results, give the tool a `compile_commands.json` (`-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`).
Without one, includes and macros from other directories don't resolve. Semantic changes can then be
missed or misclassified, and go-to-definition finds little. The status bar shows which database is in use.

It's looked for in the repository root, `build/`, `build/*/`, `out/` and `cmake-build-*/`. A database
somewhere else (e.g. `out/build/` in a larger tree) can be set in *Review → Set compile_commands.json*
(remembered per repository) or passed with `-p` to the CLI.

Each revision is parsed against its own sources: paths into the repository are remapped into the revision's
snapshot. Paths that don't exist in the snapshot keep pointing into the repository. These are the build
directory with generated headers and downloaded dependencies, which aren't part of a commit.

libclang may print `libclang: crash detected during parsing` for files it can't handle. The crash is caught:
that file is retried with fallback flags or left out of the symbol index, and the review continues.

## Files it writes

Everything lives under `~/.cache/cppreviewer/` (or `$XDG_CACHE_HOME/cppreviewer/`):

| Directory | Contents |
|---|---|
| `snapshots/<sha>` | each reviewed commit, exported so it can be parsed with its own headers |
| `reviewed/` | hunks and changes marked as reviewed, one file per repository |
| `bookmarks/` | bookmarks and comments, one file per repository |
| `symbols/` | symbol index results per file (content + flags) |
| `repos/<host>/<owner>/<repo>.git` | download cache for pull requests of repositories you don't have locally |

Everything there can be deleted at any time; it's rebuilt when needed. The repository itself is never
modified, apart from `git fetch` for pull requests. GUI settings (window layout, theme, compile database
per repository, last comparison) and the saved sessions (`sessions.json`) are kept in `~/.config/cppreviewer/`.

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
  Reviewed            hunks, content keys, store of reviewed marks
  PrCompare           pull requests from any repository, path mapping, interdiff
src/gui/    Qt 6 Widgets front end
  MainWindow          docks (files, changes, search, bookmarks), navigation, review marks
  DiffView, CodeView  side-by-side diff with folding, gutter badges, bookmarks, find
  CompareDialog, RevisionDialog, QuickOpen, FindBar, FileSearch, Bookmarks, Sessions
src/cli/    terminal front end
```
