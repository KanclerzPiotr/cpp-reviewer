#include "Session.hpp"

namespace cr {

ReviewSession::ReviewSession(std::optional<GitRepo> repo, Revision base, Revision target)
    : repo_(repo), baseRepo_(std::move(repo)), base_(std::move(base)), target_(std::move(target))
{
}

ReviewSession::ReviewSession(std::optional<GitRepo> baseRepo, Revision base, std::optional<GitRepo> targetRepo,
                             Revision target)
    : repo_(std::move(targetRepo)), baseRepo_(std::move(baseRepo)), base_(std::move(base)), target_(std::move(target))
{
}

std::shared_ptr<CompileDatabase> ReviewSession::loadDatabase(const std::string& path, const std::string& projectRoot)
{
    auto db = std::make_shared<CompileDatabase>();
    const auto p = path.empty() ? CompileDatabase::find(projectRoot) : path;
    if (!p.empty())
        db->load(p, nullptr);
    return db;
}

bool ReviewSession::prepare(std::string* error, const ProgressFn& progress)
{
    if (progress)
        progress("Exporting " + base_.display(), 0, 2);
    baseSnap_ = materialize(repo(Side::Old), base_, error);
    if (!baseSnap_)
        return false;
    if (progress)
        progress("Exporting " + target_.display(), 1, 2);
    targetSnap_ = materialize(repo(Side::New), target_, error);
    if (!targetSnap_)
        return false;

    // The compile database belongs to the checked-out project (or the target directory).
    // Paths in it point into that project; each side remaps them into its own snapshot.
    const bool oneRepo = !baseRepo_ || !repo_ || baseRepo_->root() == repo_->root();
    if (oneRepo) {
        const std::string projectRoot = repo() ? repo()->root() : targetSnap_->root;
        if (compileDbPath_.empty())
            compileDbPath_ = CompileDatabase::find(projectRoot);
        db_ = loadDatabase(compileDbPath_, projectRoot);
        oldProject_ = std::make_unique<ClangProject>(*baseSnap_, db_, projectRoot);
        newProject_ = std::make_unique<ClangProject>(*targetSnap_, db_, projectRoot);
    } else {
        const auto baseRoot = baseRepo_->root(), targetRoot = repo_->root();
        auto baseDb = loadDatabase(sideDbPath_[0], baseRoot);
        auto targetDb = loadDatabase(sideDbPath_[1], targetRoot);
        compileDbPath_ = !baseDb->empty() ? baseDb->path() : targetDb->path();
        db_ = targetDb;
        oldProject_ = std::make_unique<ClangProject>(*baseSnap_, baseDb, baseRoot);
        newProject_ = std::make_unique<ClangProject>(*targetSnap_, targetDb, targetRoot);
    }
    if (progress)
        progress("Ready", 2, 2);
    return true;
}

ReviewResult ReviewSession::run(const ReviewOptions& options, const ProgressFn& progress,
                                const std::atomic<bool>* cancel)
{
    if (!baseSnap_ || !targetSnap_)
        return {};
    auto files = files_ ? *files_ : changedFiles(repo(), *baseSnap_, *targetSnap_);
    return computeReview(files, *baseSnap_, *targetSnap_, oldProject_.get(), newProject_.get(), options, progress,
                         cancel);
}

} // namespace cr
