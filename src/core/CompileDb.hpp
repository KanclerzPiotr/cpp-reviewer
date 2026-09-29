#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace cr {

// Compile flags for libclang, taken from compile_commands.json when available.
class CompileDatabase {
public:
    // Looks for compile_commands.json in the usual places under `projectRoot`.
    static std::string find(const std::string& projectRoot);

    bool load(const std::string& jsonPath, std::string* error = nullptr);
    bool empty() const { return entries_.empty(); }
    const std::string& path() const { return path_; }

    // Arguments for parsing `relPath` of a snapshot rooted at `snapshotRoot`.
    // `projectRoot` is the directory the database was generated for; paths inside it are
    // remapped into the snapshot so that each revision is parsed against its own headers.
    std::vector<std::string> argsFor(const std::string& projectRoot, const std::string& snapshotRoot,
                                     const std::string& relPath) const;

    // Flags used when no database entry fits.
    static std::vector<std::string> fallbackArgs(const std::string& snapshotRoot, const std::string& relPath);

private:
    struct Entry {
        std::vector<std::string> args; // filtered, with absolute include paths
    };
    const Entry* lookup(const std::string& absFile) const;

    std::string path_;
    std::unordered_map<std::string, Entry> entries_;       // absolute source path -> entry
    std::unordered_map<std::string, std::string> byDir_;   // directory -> a source file in it
};

} // namespace cr
