#pragma once

#include "Model.hpp"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace cr {

struct EntityRef {
    const ParsedFile* file = nullptr;
    const Entity* e = nullptr;
};

struct SemanticInput {
    std::vector<EntityRef> oldEntities; // entities of all changed files, old revision
    std::vector<EntityRef> newEntities; // entities of all changed files, new revision
    std::map<std::string, std::string> fileRenames; // old path -> new path
    // Whether a (1-based) line of a file is part of the textual diff on that side.
    std::function<bool(Side, const std::string& file, int line)> lineChanged;
};

struct SemanticOutput {
    std::vector<SemanticChange> changes;
    std::map<std::string, std::string> renames; // identifier renames: old -> new
};

// Matches entities between revisions and classifies how they changed: renames, moves,
// signature changes, extracted/inlined functions, statements moved between functions and
// consistent identifier renames.
SemanticOutput analyzeEntities(const SemanticInput& input);

} // namespace cr
