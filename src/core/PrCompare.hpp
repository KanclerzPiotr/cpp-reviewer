#pragma once

#include "Git.hpp"
#include "Review.hpp"

#include <optional>
#include <string>
#include <vector>

namespace cr {

// A pull request resolved to commits in some repository.
struct ResolvedPullRequest {
    std::optional<GitRepo> repo; // a local clone, or the bare download cache
    bool cached = false;         // repo is the download cache (no working tree, no compile database)
    std::string head, base;      // PR head and its merge base with the target branch
    std::string title;
    std::string label;           // "owner/repo#123"
    std::vector<ChangedFile> files; // changed by the pull request

    std::string path(const ChangedFile& f) const { return f.newPath.empty() ? f.oldPath : f.newPath; }
};

// Fetches the pull request of `url`: into `local` when one of its remotes points to that
// repository, otherwise into a bare cache repository under the cache directory. The cache fetch
// is shallow (just the two commits) when the GitHub CLI can tell them, so no history is downloaded.
std::optional<ResolvedPullRequest> resolvePullRequest(const PullRequestUrl& url, const GitRepo* local,
                                                      std::string* error);

// The same pull request from commits already fetched (e.g. when resuming a session).
std::optional<ResolvedPullRequest> resolvePullRequest(const GitRepo& repo, bool cached, const std::string& base,
                                                      const std::string& head, std::string title, std::string label);

// Maps paths of repository A to repository B by replacing a prefix, e.g. "src/lib/" -> "lib/".
struct PathMapping {
    std::string from, to;

    std::string toB(const std::string& aPath) const;
    std::string toA(const std::string& bPath) const;
    std::string text() const { return from + "=" + to; } // "src/lib/=lib/"
    static std::optional<PathMapping> parse(const std::string& text);
};

// Guesses the mapping from files both pull requests changed (longest common path suffix),
// falling back to the files of B's tree when they share none.
PathMapping guessPathMapping(const ResolvedPullRequest& a, const ResolvedPullRequest& b);

// A file changed by either pull request, paired across the repositories.
struct FilePair {
    std::string pathA, pathB;
    const ChangedFile* a = nullptr; // how A changed it (null: A didn't)
    const ChangedFile* b = nullptr;
};
std::vector<FilePair> pairFiles(const ResolvedPullRequest& a, const ResolvedPullRequest& b, const PathMapping& m);

// Interdiff: for every pair, A's patch (as unified-diff-like text) next to B's patch. Identical
// changes produce equal rows; a hunk that differs or is missing on one side shows up as a change.
// Files are marked '=' (same change), 'M' (different), 'D' (only A) or 'A' (only B).
ReviewResult computeInterdiff(const ResolvedPullRequest& a, const ResolvedPullRequest& b,
                              const std::vector<FilePair>& pairs, bool ignoreWhitespace);

// The pairs as files of a review between A's head and B's head ("final files" view).
std::vector<ChangedFile> finalFilePairs(const ResolvedPullRequest& a, const ResolvedPullRequest& b,
                                        const std::vector<FilePair>& pairs);

} // namespace cr
