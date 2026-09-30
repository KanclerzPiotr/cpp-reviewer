#pragma once

#include "Review.hpp"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace cr {

// A run of changed rows in a file diff.
struct Hunk {
    int rowBegin = 0, rowEnd = 0;     // [begin, end) into FileDiff::rows
    int oldBegin = -1, oldEnd = -1;   // 0-based line range on each side, [begin, end); -1 if none
    int newBegin = -1, newEnd = -1;
    uint64_t key = 0;                 // identifies the hunk by its path and content
};

std::vector<Hunk> diffHunks(const FileDiff& fd);

// Identifies a change by its title and the code it covers, so that it keeps its identity
// when the same change is reviewed again in a later revision.
uint64_t changeKey(const ReviewResult& result, const SemanticChange& change);

// Keys of hunks and changes marked as reviewed, persisted per project.
class ReviewedStore {
public:
    ReviewedStore() = default;
    explicit ReviewedStore(const std::string& projectRoot);

    bool has(uint64_t key) const { return keys_.count(key) != 0; }
    void set(uint64_t key, bool reviewed);

private:
    void save() const;

    std::string path_;
    std::unordered_set<uint64_t> keys_;
};

} // namespace cr
