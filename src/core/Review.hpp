#pragma once

#include "ClangProject.hpp"
#include "Git.hpp"
#include "Model.hpp"
#include "Snapshot.hpp"

#include <atomic>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace cr {

enum class RowKind : uint8_t { Equal, Deleted, Inserted, Modified };

// One row of a side-by-side diff. Line indices are 0-based, -1 when the side has no line.
struct DiffRow {
    RowKind kind = RowKind::Equal;
    int oldLine = -1;
    int newLine = -1;
};

enum class LineTag : uint8_t { None, Deleted, Inserted, Modified, Moved, RenameOnly };

struct LineInfo {
    LineTag tag = LineTag::None;
    int change = -1; // index into ReviewResult::changes, the most specific change touching this line
};

struct FileDiff {
    ChangedFile change;
    std::string oldPath, newPath; // empty when the side doesn't exist
    bool cpp = false;
    bool binary = false;
    std::vector<std::string> oldLines, newLines;
    std::vector<DiffRow> rows;
    std::vector<LineInfo> oldInfo, newInfo;
    std::vector<int> changes; // semantic changes touching this file
    int added = 0, removed = 0;
    int oldErrors = 0, newErrors = 0; // libclang errors (missing includes etc.)
    // Generated text rather than a file of a revision (e.g. an interdiff): no navigation, and
    // the gutter shows the display line numbers (0 = none) instead of the document's own.
    bool synthetic = false;
    std::vector<int> oldDisplay, newDisplay;
    // Lines outside [begin, end) are surrounding context, shown dimmed (e.g. around a moved entity);
    // -1: every line is content.
    int oldContentBegin = -1, oldContentEnd = -1, newContentBegin = -1, newContentEnd = -1;

    const std::string& path() const { return newPath.empty() ? oldPath : newPath; }
};

struct ReviewResult {
    std::vector<FileDiff> files;
    std::vector<SemanticChange> changes;
    std::map<std::string, std::string> renames;                 // old -> new, where unambiguous
    std::map<std::string, std::set<std::string>> renameTargets; // old -> every new name

    // Index of the file with `path` on the given side, or -1.
    int fileIndex(Side side, const std::string& path) const;
};

struct ReviewOptions {
    bool ignoreWhitespace = false;
    bool semantic = true;
};

using ProgressFn = std::function<void(const std::string& stage, int done, int total)>;

ReviewResult computeReview(const std::vector<ChangedFile>& files, const Snapshot& base, const Snapshot& target,
                           ClangProject* oldProject, ClangProject* newProject, const ReviewOptions& options,
                           const ProgressFn& progress = {}, const std::atomic<bool>* cancel = nullptr);

// Fills rows and line tags of `fd` from its oldLines/newLines.
void diffLines(FileDiff& fd, bool ignoreWhitespace);

// Applies `renames` to one identifier: the whole word, or renamed names that are complete parts of
// it (delimited by '_' or camelCase, e.g. MYLIB_OldName_SOURCES). `used` gets the first name applied.
std::string renameInWord(const std::string& word, const std::map<std::string, std::string>& renames,
                         std::string* used = nullptr);

// True when identifier `b` is `a` with one of the renames applied, to the whole word or to every
// complete part of it naming the old identifier. `from`/`to` get the rename used.
bool isRenamedWord(const std::string& a, const std::string& b,
                   const std::map<std::string, std::set<std::string>>& targets, std::string* from = nullptr,
                   std::string* to = nullptr);

// Replaces identifiers (and renamed parts of identifiers) according to `renames`.
std::string applyRenames(const std::string& line, const std::map<std::string, std::string>& renames);

struct Span {
    int begin = 0, end = 0; // byte offsets into the line
};

// Changed character spans of two versions of a line (word-level diff).
void intralineDiff(const std::string& a, const std::string& b, std::vector<Span>& aSpans, std::vector<Span>& bSpans);

} // namespace cr
