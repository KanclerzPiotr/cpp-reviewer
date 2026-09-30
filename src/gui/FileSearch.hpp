#pragma once

#include "core/Model.hpp"

#include <QString>

#include <atomic>
#include <string>
#include <vector>

namespace gui {

struct SearchQuery {
    QString text;
    bool caseSensitive = false;
    bool wholeWord = false;
    bool regex = false;
};

struct SearchHit {
    cr::Side side = cr::Side::New;
    QString path; // relative to the snapshot root
    int line = 0; // 1-based
    QString text;
};

struct SearchTarget {
    cr::Side side;
    std::string root;
    std::vector<std::string> files; // relative paths
};

// Greps text files of the targets in parallel (binary and huge files are skipped). Stops after
// `maxHits`, setting `truncated`. Hits are ordered like the targets and their files.
std::vector<SearchHit> searchFiles(const std::vector<SearchTarget>& targets, const SearchQuery& query,
                                   const std::atomic<bool>& cancel, size_t maxHits, bool& truncated,
                                   QString* error = nullptr);

} // namespace gui
