#include "Session.hpp"

namespace cr {

ReviewSession::ReviewSession(std::optional<GitRepo> repo, Revision base, Revision target)
    : repo_(std::move(repo)), base_(std::move(base)), target_(std::move(target))
{
}

bool ReviewSession::prepare(std::string* error, const ProgressFn& progress)
{
    const GitRepo* r = repo();
    if (progress)
        progress("Exporting " + base_.display(), 0, 2);
    baseSnap_ = materialize(r, base_, error);
    if (!baseSnap_)
        return false;
    if (progress)
        progress("Exporting " + target_.display(), 1, 2);
    targetSnap_ = materialize(r, target_, error);
    if (!targetSnap_)
        return false;

    // The compile database belongs to the checked-out project (or the target directory).
    const std::string projectRoot = r ? r->root() : targetSnap_->root;
    db_ = std::make_shared<CompileDatabase>();
    if (compileDbPath_.empty())
        compileDbPath_ = CompileDatabase::find(projectRoot);
    if (!compileDbPath_.empty())
        db_->load(compileDbPath_, nullptr);

    // Paths in the database point into `projectRoot`; each side remaps them into its own snapshot.
    oldProject_ = std::make_unique<ClangProject>(*baseSnap_, db_, projectRoot);
    newProject_ = std::make_unique<ClangProject>(*targetSnap_, db_, projectRoot);
    if (progress)
        progress("Ready", 2, 2);
    return true;
}

ReviewResult ReviewSession::run(const ReviewOptions& options, const ProgressFn& progress,
                                const std::atomic<bool>* cancel)
{
    if (!baseSnap_ || !targetSnap_)
        return {};
    auto files = changedFiles(repo(), *baseSnap_, *targetSnap_);
    return computeReview(files, *baseSnap_, *targetSnap_, oldProject_.get(), newProject_.get(), options, progress,
                         cancel);
}

} // namespace cr
