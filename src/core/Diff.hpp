#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cr {

enum class EditOp { Equal, Delete, Insert };

struct Edit {
    EditOp op;
    int aBegin, aEnd; // range in sequence A (empty for Insert)
    int bBegin, bEnd; // range in sequence B (empty for Delete)
};

// Myers O(ND) difference with linear space (divide & conquer on the middle snake).
std::vector<Edit> diffSequences(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b);

// Line diff for display: Myers, then each block of added/removed lines that could equally be
// placed a few lines up or down (e.g. a new function starting or ending with "}") is slid to where
// it reads best, using git's indent heuristic. `a`/`b` are the line hashes, `aLines`/`bLines` the
// text (for indentation and blank lines).
std::vector<Edit> diffLinesReadable(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b,
                                    const std::vector<std::string>& aLines, const std::vector<std::string>& bLines);

// Number of equal elements in the diff of a and b (length of the LCS found by Myers).
int commonLength(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b);

// Dice similarity: 2 * LCS / (|a| + |b|), in [0, 1].
double similarity(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b);

uint64_t hashString(std::string_view s, uint64_t seed = 1469598103934665603ULL);
uint64_t hashCombine(uint64_t h, uint64_t v);

// Collapses runs of whitespace and trims; used for whitespace-insensitive comparisons.
std::string normalizeWhitespace(std::string_view line);

std::vector<std::string> splitLines(const std::string& text);

} // namespace cr
