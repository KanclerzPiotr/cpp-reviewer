#pragma once

#include <optional>
#include <string>
#include <vector>

namespace cr {

struct CommitInfo {
    std::string sha;
    std::string shortSha;
    std::string author;
    std::string date;     // ISO-like, e.g. 2026-09-29 12:00
    std::string subject;
    std::string refs;     // decorations, e.g. "HEAD -> main, origin/main"
};

struct ChangedFile {
    char status = 'M';    // A, D, M, R (rename), C (copy), T (type change), ? (untracked)
    std::string oldPath;  // empty when added
    std::string newPath;  // empty when deleted
    int similarity = 0;   // for renames/copies reported by git
};

class GitRepo {
public:
    // Opens the repository containing `path`. Returns nullopt if it's not inside a work tree.
    static std::optional<GitRepo> open(const std::string& path);

    const std::string& root() const { return root_; }

    std::vector<std::string> localBranches() const;
    std::vector<std::string> remoteBranches() const;
    std::vector<std::string> tags() const;
    std::vector<std::string> remotes() const;
    std::string remoteUrl(const std::string& remote) const;
    std::string currentBranch() const;
    // Branch the remote's HEAD points to, e.g. "main". Empty if unknown.
    std::string remoteDefaultBranch(const std::string& remote) const;

    // `ref` empty means --all.
    std::vector<CommitInfo> log(const std::string& ref, int maxCount, int skip = 0,
                                const std::string& pathFilter = {}) const;
    std::optional<CommitInfo> commitInfo(const std::string& rev) const;

    // Resolves to a full commit sha, or empty on failure.
    std::string resolve(const std::string& rev) const;
    std::string mergeBase(const std::string& a, const std::string& b) const;

    // Changed files between two commits.
    std::vector<ChangedFile> diffCommits(const std::string& a, const std::string& b) const;
    // Changed files between a commit and the working tree (including untracked files).
    std::vector<ChangedFile> diffWorkingTree(const std::string& a) const;
    // Changed files between a commit and the index.
    std::vector<ChangedFile> diffIndex(const std::string& a) const;

    // Writes the tree of commit `sha` into `dir` (created if needed).
    bool exportCommit(const std::string& sha, const std::string& dir, std::string* error) const;
    // Writes the content of the index into `dir`.
    bool exportIndex(const std::string& dir, std::string* error) const;

    // Fetches refs/pull/<n>/head from `remote` and returns its sha (empty on failure).
    std::string fetchPullRequest(const std::string& remote, int number, std::string* error) const;
    // Fetches a branch from `remote` and returns the fetched sha.
    std::string fetchBranch(const std::string& remote, const std::string& branch, std::string* error) const;

private:
    explicit GitRepo(std::string root) : root_(std::move(root)) {}
    std::vector<std::string> lines(const std::vector<std::string>& args) const;
    std::vector<ChangedFile> parseNameStatus(const std::string& out) const;

    std::string root_;
};

// Parses "owner/repo" from a GitHub remote URL; empty if the URL isn't GitHub.
std::string gitHubSlugFromUrl(const std::string& url);

} // namespace cr
