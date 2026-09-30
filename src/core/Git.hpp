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
    // A bare repository at `dir`, created if needed (e.g. a download cache).
    static std::optional<GitRepo> openBare(const std::string& dir, std::string* error = nullptr);

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

    // Content of `path` at commit `sha`; nullopt if it doesn't exist there.
    std::optional<std::string> fileAt(const std::string& sha, const std::string& path) const;
    // Every file path of commit `sha`.
    std::vector<std::string> listTree(const std::string& sha) const;

    // Writes the tree of commit `sha` into `dir` (created if needed).
    bool exportCommit(const std::string& sha, const std::string& dir, std::string* error) const;
    // Writes the content of the index into `dir`.
    bool exportIndex(const std::string& dir, std::string* error) const;

    // Fetches refs/pull/<n>/head from `remote` and returns its sha (empty on failure).
    std::string fetchPullRequest(const std::string& remote, int number, std::string* error) const;
    // Fetches a branch from `remote` and returns the fetched sha.
    std::string fetchBranch(const std::string& remote, const std::string& branch, std::string* error) const;

    // Name of a remote whose URL points to `slug` ("owner/repo") on `host`; empty if none.
    std::string remoteFor(const std::string& host, const std::string& slug) const;

    struct PullRequestMeta {
        std::string title, state; // state: OPEN, MERGED, CLOSED
        std::string headSha;
        std::string baseRef;      // target branch
        std::string baseSha;      // target branch commit GitHub compares against (also for merged PRs)
    };
    // Details of pull request `number` of `remote` (a remote name or URL) from the GitHub CLI (`gh`),
    // if it's installed and logged in; nullopt otherwise.
    std::optional<PullRequestMeta> pullRequestMeta(const std::string& remote, int number) const;

    struct FetchedPullRequest {
        std::string head, base; // PR head and its merge base with the target branch
        std::string baseLabel;  // e.g. "merge-base with origin/main"
        std::string title;      // when known
        std::string error;
    };
    // Fetches pull request `number` from `remote` (a remote name or a URL) with its base, like
    // GitHub's "Files changed". Without `baseRef`, the target branch comes from `gh` (see
    // pullRequestMeta), else from refs/pull/N/merge (open PRs only: its first parent is the
    // target's tip), else it's the remote's default branch.
    FetchedPullRequest fetchPullRequestForReview(const std::string& remote, int number, const std::string& baseRef) const;

private:
    explicit GitRepo(std::string root) : root_(std::move(root)) {}
    std::vector<std::string> lines(const std::vector<std::string>& args) const;
    std::vector<ChangedFile> parseNameStatus(const std::string& out) const;

    std::string root_;
};

// Parses "owner/repo" from a GitHub remote URL; empty if the URL isn't GitHub.
std::string gitHubSlugFromUrl(const std::string& url);

// Host and "owner/repo" of a remote URL (https://, ssh://, git://, or scp-like git@host:owner/repo).
bool hostAndSlugFromUrl(const std::string& url, std::string& host, std::string& slug);

// A pull request link such as https://github.com/owner/repo/pull/123/files (GitHub Enterprise too).
struct PullRequestUrl {
    std::string host, slug;
    int number = 0;
    std::string fetchUrl() const { return "https://" + host + "/" + slug + ".git"; }
};
std::optional<PullRequestUrl> parsePullRequestUrl(const std::string& text);

} // namespace cr
