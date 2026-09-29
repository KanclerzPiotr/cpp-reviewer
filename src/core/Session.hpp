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

    // Overrides the compile_commands.json lookup.
    void setCompileDatabasePath(std::string path) { compileDbPath_ = std::move(path); }

    // Materializes both revisions and loads compile flags.
    bool prepare(std::string* error, const ProgressFn& progress = {});

    ReviewResult run(const ReviewOptions& options, const ProgressFn& progress = {},
                     const std::atomic<bool>* cancel = nullptr);

    const GitRepo* repo() const { return repo_ ? &*repo_ : nullptr; }
    const Revision& revision(Side s) const { return s == Side::Old ? base_ : target_; }
    const Snapshot& snapshot(Side s) const { return s == Side::Old ? *baseSnap_ : *targetSnap_; }
    ClangProject* project(Side s) const { return s == Side::Old ? oldProject_.get() : newProject_.get(); }
    const std::string& compileDatabasePath() const { return compileDbPath_; }

private:
    std::optional<GitRepo> repo_;
    Revision base_, target_;
    std::optional<Snapshot> baseSnap_, targetSnap_;
    std::string compileDbPath_;
    std::shared_ptr<CompileDatabase> db_;
    std::unique_ptr<ClangProject> oldProject_, newProject_;
};

} // namespace cr
