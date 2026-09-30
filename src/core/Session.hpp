#pragma once

#include "ClangProject.hpp"
#include "CompileDb.hpp"
#include "Git.hpp"
#include "Review.hpp"
#include "Snapshot.hpp"

#include <memory>
#include <optional>
#include <string>

namespace cr {

// Everything needed to review one pair of revisions: repository, materialized snapshots,
// compile flags and a libclang project per side.
class ReviewSession {
public:
    // `repo` may be empty when comparing two plain directories.
    ReviewSession(std::optional<GitRepo> repo, Revision base, Revision target);
    // Each revision from its own repository (e.g. the same change in two repositories).
    ReviewSession(std::optional<GitRepo> baseRepo, Revision base, std::optional<GitRepo> targetRepo, Revision target);

    // Overrides the compile_commands.json lookup (for both sides, or one).
    void setCompileDatabasePath(std::string path) { compileDbPath_ = std::move(path); }
    void setCompileDatabasePath(Side side, std::string path) { sideDbPath_[side == Side::Old ? 0 : 1] = std::move(path); }
    // Compares these files (with their paths on each side) instead of what git reports.
    void setFiles(std::vector<ChangedFile> files) { files_ = std::move(files); }

    // Materializes both revisions and loads compile flags.
    bool prepare(std::string* error, const ProgressFn& progress = {});

    ReviewResult run(const ReviewOptions& options, const ProgressFn& progress = {},
                     const std::atomic<bool>* cancel = nullptr);

    const GitRepo* repo() const { return repo_ ? &*repo_ : nullptr; }
    const GitRepo* repo(Side s) const
    {
        const auto& r = s == Side::Old ? baseRepo_ : repo_;
        return r ? &*r : nullptr;
    }
    const Revision& revision(Side s) const { return s == Side::Old ? base_ : target_; }
    const Snapshot& snapshot(Side s) const { return s == Side::Old ? *baseSnap_ : *targetSnap_; }
    ClangProject* project(Side s) const { return s == Side::Old ? oldProject_.get() : newProject_.get(); }
    const std::string& compileDatabasePath() const { return compileDbPath_; }

private:
    std::shared_ptr<CompileDatabase> loadDatabase(const std::string& path, const std::string& projectRoot);

    std::optional<GitRepo> repo_;     // the target's (and, with one repository, the base's)
    std::optional<GitRepo> baseRepo_; // the base's
    std::optional<std::vector<ChangedFile>> files_;
    std::string sideDbPath_[2];
    Revision base_, target_;
    std::optional<Snapshot> baseSnap_, targetSnap_;
    std::string compileDbPath_;
    std::shared_ptr<CompileDatabase> db_;
    std::unique_ptr<ClangProject> oldProject_, newProject_;
};

} // namespace cr
