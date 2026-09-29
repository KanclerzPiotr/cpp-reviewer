#pragma once

#include "Git.hpp"

#include <optional>
#include <string>
#include <vector>

namespace cr {

// One side of a comparison.
struct Revision {
    enum class Kind { Commit, WorkingTree, Index, Directory };

    Kind kind = Kind::Commit;
    std::string ref;   // what the user picked: branch, tag, "HEAD~1", sha... or a directory path
    std::string sha;   // resolved commit for Kind::Commit
    std::string label; // human readable description

    static Revision commit(std::string ref, std::string sha, std::string label = {});
    static Revision workingTree();
    static Revision index();
    static Revision directory(std::string path);

    std::string display() const;
    bool isGit() const { return kind != Kind::Directory; }
};

// A revision materialized on disk so it can be read and parsed with its includes.
struct Snapshot {
    Revision rev;
    std::string root; // absolute directory

    std::string absPath(const std::string& rel) const;
    bool exists(const std::string& rel) const;
    std::optional<std::string> read(const std::string& rel) const;
    // Converts an absolute path inside the snapshot to a relative one; empty if outside.
    std::string relPath(const std::string& abs) const;
};

// Exports commits into a cache directory (reused between runs); working tree maps to the repo itself.
std::optional<Snapshot> materialize(const GitRepo* repo, const Revision& rev, std::string* error);

std::string cacheDirectory();

// Resolves user input (e.g. "HEAD~2", "main", "WORKTREE", "INDEX", a sha) into a revision.
std::optional<Revision> parseRevisionSpec(const GitRepo& repo, const std::string& spec);

// Changed files between two snapshots. Uses git when possible, otherwise walks both directories.
std::vector<ChangedFile> changedFiles(const GitRepo* repo, const Snapshot& base, const Snapshot& target);

bool isCppFile(const std::string& path);
bool isHeaderFile(const std::string& path);

// Every C/C++ source or header file in the snapshot (relative paths), skipping VCS/build dirs.
std::vector<std::string> listCppFiles(const Snapshot& snap);

} // namespace cr
