#pragma once

#include "CompileDb.hpp"
#include "Model.hpp"
#include "Snapshot.hpp"

#include <atomic>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace cr {

struct SymbolInfo {
    std::string spelling;
    std::string kind;          // "function", "class", "variable", "macro", ...
    std::string qualifiedName;
    std::string type;          // type or full signature
    std::string comment;       // raw documentation comment, if any
    std::string usr;
    std::optional<Location> definition;
    std::optional<Location> declaration;
    std::optional<Location> includedFile; // for #include directives
};

// libclang access for one revision: parses files on demand, keeps a bounded cache of
// translation units for navigation and an optional project-wide symbol index.
class ClangProject {
public:
    ClangProject(Snapshot snapshot, std::shared_ptr<const CompileDatabase> db, std::string projectRoot);
    ~ClangProject();

    ClangProject(const ClangProject&) = delete;
    ClangProject& operator=(const ClangProject&) = delete;

    const Snapshot& snapshot() const { return snap_; }

    // Parses a file (relative path) and extracts tokens and entities. Thread-safe, cached.
    std::shared_ptr<const ParsedFile> parse(const std::string& relPath);

    // Symbol under a position (1-based line, 1-based byte column). `file` may be absolute
    // for files outside the snapshot.
    std::optional<SymbolInfo> symbolAt(const std::string& file, int line, int col);

    // Parses all C/C++ files (without function bodies) to map USRs to definitions and
    // declarations. Can be cancelled; safe to call from a background thread.
    void buildIndex(const std::function<void(int done, int total)>& progress, const std::atomic<bool>& cancel);
    bool indexReady() const { return indexReady_; }

    std::vector<Location> definitionsOf(const std::string& usr) const;
    std::vector<Location> declarationsOf(const std::string& usr) const;

private:
    struct Tu;
    std::shared_ptr<Tu> acquireTu(const std::string& absPath, bool skipBodies);
    std::vector<std::string> argsFor(const std::string& absPath) const;
    std::string absPath(const std::string& file) const;
    Location toLocation(const std::string& absFile, int line, int col) const;

    Snapshot snap_;
    std::shared_ptr<const CompileDatabase> db_;
    std::string projectRoot_;

    std::mutex parsedMutex_;
    std::unordered_map<std::string, std::shared_ptr<const ParsedFile>> parsed_;

    std::mutex tuMutex_;
    std::list<std::string> tuLru_;
    std::unordered_map<std::string, std::shared_ptr<Tu>> tus_;
    static constexpr size_t kMaxTus = 12;

    mutable std::mutex indexMutex_;
    std::unordered_map<std::string, std::vector<Location>> defs_;
    std::unordered_map<std::string, std::vector<Location>> decls_;
    std::atomic<bool> indexReady_{false};
};

} // namespace cr
