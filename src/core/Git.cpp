#include "Git.hpp"

#include "Json.hpp"
#include "Process.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace cr {

namespace {

std::string trim(std::string s)
{
    auto space = [](char c) { return c == '\n' || c == '\r' || c == ' ' || c == '\t'; };
    while (!s.empty() && space(s.back()))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && space(s[i]))
        ++i;
    return s.substr(i);
}

std::vector<std::string> splitOn(const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

std::vector<std::string> gitArgs(const std::string& root, std::initializer_list<std::string> rest)
{
    std::vector<std::string> args{"git", "-C", root, "-c", "core.quotepath=off"};
    args.insert(args.end(), rest);
    return args;
}

} // namespace

std::optional<GitRepo> GitRepo::open(const std::string& path)
{
    auto r = runProcess({"git", "-C", path, "rev-parse", "--show-toplevel"});
    if (!r.ok())
        return std::nullopt;
    return GitRepo(trim(r.out));
}

std::optional<GitRepo> GitRepo::openBare(const std::string& dir, std::string* error)
{
    std::error_code ec;
    if (!fs::exists(fs::path(dir) / "HEAD", ec)) {
        fs::create_directories(dir, ec);
        auto r = runProcess({"git", "init", "--quiet", "--bare", dir});
        if (!r.ok()) {
            if (error)
                *error = r.err;
            return std::nullopt;
        }
    }
    return GitRepo(dir);
}

std::optional<std::string> GitRepo::fileAt(const std::string& sha, const std::string& path) const
{
    auto r = runProcess(gitArgs(root_, {"cat-file", "blob", sha + ":" + path}));
    if (!r.ok())
        return std::nullopt;
    return std::move(r.out);
}

std::vector<std::string> GitRepo::listTree(const std::string& sha) const
{
    return lines({"ls-tree", "-r", "--name-only", sha});
}

std::vector<std::string> GitRepo::lines(const std::vector<std::string>& rest) const
{
    std::vector<std::string> args{"git", "-C", root_, "-c", "core.quotepath=off"};
    args.insert(args.end(), rest.begin(), rest.end());
    auto r = runProcess(args);
    if (!r.ok())
        return {};
    std::vector<std::string> out;
    for (auto& l : splitOn(r.out, '\n')) {
        auto t = trim(l);
        if (!t.empty())
            out.push_back(t);
    }
    return out;
}

std::vector<std::string> GitRepo::localBranches() const
{
    return lines({"for-each-ref", "--sort=-committerdate", "--format=%(refname:short)", "refs/heads"});
}

std::vector<std::string> GitRepo::remoteBranches() const
{
    std::vector<std::string> out;
    for (auto& b : lines({"for-each-ref", "--sort=-committerdate", "--format=%(refname:short)", "refs/remotes"}))
        if (b.size() < 5 || b.compare(b.size() - 5, 5, "/HEAD") != 0)
            out.push_back(b);
    return out;
}

std::vector<std::string> GitRepo::tags() const
{
    return lines({"for-each-ref", "--sort=-creatordate", "--format=%(refname:short)", "refs/tags"});
}

std::vector<std::string> GitRepo::remotes() const
{
    return lines({"remote"});
}

std::string GitRepo::remoteUrl(const std::string& remote) const
{
    auto l = lines({"remote", "get-url", remote});
    return l.empty() ? std::string{} : l.front();
}

std::string GitRepo::currentBranch() const
{
    auto l = lines({"rev-parse", "--abbrev-ref", "HEAD"});
    return l.empty() ? std::string{} : l.front();
}

std::string GitRepo::remoteDefaultBranch(const std::string& remote) const
{
    auto l = lines({"symbolic-ref", "--short", "refs/remotes/" + remote + "/HEAD"});
    if (l.empty())
        return {};
    auto s = l.front();
    auto prefix = remote + "/";
    if (s.rfind(prefix, 0) == 0)
        s = s.substr(prefix.size());
    return s;
}

std::vector<CommitInfo> GitRepo::log(const std::string& ref, int maxCount, int skip,
                                     const std::string& pathFilter) const
{
    std::vector<std::string> args{"git", "-C", root_, "log",
                                  "--format=%H%x1f%h%x1f%an%x1f%ad%x1f%s%x1f%D%x1e",
                                  "--date=format:%Y-%m-%d %H:%M",
                                  "-n", std::to_string(maxCount),
                                  "--skip=" + std::to_string(skip)};
    if (ref.empty())
        args.push_back("--all");
    else
        args.push_back(ref);
    if (!pathFilter.empty()) {
        args.push_back("--");
        args.push_back(pathFilter);
    }
    auto r = runProcess(args);
    std::vector<CommitInfo> out;
    if (!r.ok())
        return out;
    for (auto& rec : splitOn(r.out, '\x1e')) {
        auto f = splitOn(trim(rec), '\x1f');
        if (f.size() < 5)
            continue;
        CommitInfo c;
        c.sha = trim(f[0]);
        c.shortSha = f[1];
        c.author = f[2];
        c.date = f[3];
        c.subject = f[4];
        if (f.size() > 5)
            c.refs = f[5];
        out.push_back(std::move(c));
    }
    return out;
}

std::optional<CommitInfo> GitRepo::commitInfo(const std::string& rev) const
{
    auto sha = resolve(rev);
    if (sha.empty())
        return std::nullopt;
    auto l = log(sha, 1);
    if (l.empty())
        return std::nullopt;
    return l.front();
}

std::string GitRepo::resolve(const std::string& rev) const
{
    auto l = lines({"rev-parse", "--verify", "--quiet", rev + "^{commit}"});
    return l.empty() ? std::string{} : l.front();
}

std::string GitRepo::mergeBase(const std::string& a, const std::string& b) const
{
    auto l = lines({"merge-base", a, b});
    return l.empty() ? std::string{} : l.front();
}

std::vector<ChangedFile> GitRepo::parseNameStatus(const std::string& out) const
{
    // Format with -z: STATUS\0path\0 or Rxxx\0old\0new\0
    std::vector<ChangedFile> files;
    std::vector<std::string> parts;
    std::string cur;
    for (char c : out) {
        if (c == '\0') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    for (size_t i = 0; i < parts.size();) {
        const std::string& st = parts[i++];
        if (st.empty())
            continue;
        ChangedFile f;
        f.status = st[0];
        if (st.size() > 1)
            f.similarity = std::atoi(st.c_str() + 1);
        if (f.status == 'R' || f.status == 'C') {
            if (i + 1 >= parts.size())
                break;
            f.oldPath = parts[i++];
            f.newPath = parts[i++];
        } else if (i < parts.size()) {
            const std::string& p = parts[i++];
            if (f.status == 'A')
                f.newPath = p;
            else if (f.status == 'D')
                f.oldPath = p;
            else
                f.oldPath = f.newPath = p;
        }
        files.push_back(std::move(f));
    }
    return files;
}

std::vector<ChangedFile> GitRepo::diffCommits(const std::string& a, const std::string& b) const
{
    auto r = runProcess(gitArgs(root_, {"diff", "--name-status", "-z", "-M", "--no-ext-diff", a, b}));
    return r.ok() ? parseNameStatus(r.out) : std::vector<ChangedFile>{};
}

std::vector<ChangedFile> GitRepo::diffWorkingTree(const std::string& a) const
{
    auto r = runProcess(gitArgs(root_, {"diff", "--name-status", "-z", "-M", "--no-ext-diff", a}));
    auto files = r.ok() ? parseNameStatus(r.out) : std::vector<ChangedFile>{};
    for (auto& p : lines({"ls-files", "--others", "--exclude-standard"})) {
        ChangedFile f;
        f.status = 'A';
        f.newPath = p;
        files.push_back(std::move(f));
    }
    return files;
}

std::vector<ChangedFile> GitRepo::diffIndex(const std::string& a) const
{
    auto r = runProcess(gitArgs(root_, {"diff", "--cached", "--name-status", "-z", "-M", "--no-ext-diff", a}));
    return r.ok() ? parseNameStatus(r.out) : std::vector<ChangedFile>{};
}

bool GitRepo::exportCommit(const std::string& sha, const std::string& dir, std::string* error) const
{
    std::error_code ec;
    fs::create_directories(dir, ec);
    auto tarPath = dir + ".tar";
    auto r = runProcess(gitArgs(root_, {"archive", "--format=tar", "-o", tarPath, sha}));
    if (!r.ok()) {
        if (error)
            *error = r.err;
        return false;
    }
    auto t = runProcess({"tar", "-xf", tarPath, "-C", dir});
    fs::remove(tarPath, ec);
    if (!t.ok()) {
        if (error)
            *error = t.err;
        return false;
    }
    return true;
}

bool GitRepo::exportIndex(const std::string& dir, std::string* error) const
{
    std::error_code ec;
    fs::create_directories(dir, ec);
    auto prefix = fs::absolute(dir).string();
    if (prefix.back() != '/')
        prefix += '/';
    auto r = runProcess(gitArgs(root_, {"checkout-index", "-a", "-f", "--prefix=" + prefix}));
    if (!r.ok() && error)
        *error = r.err;
    return r.ok();
}

std::string GitRepo::fetchPullRequest(const std::string& remote, int number, std::string* error) const
{
    return fetchBranch(remote, "pull/" + std::to_string(number) + "/head", error);
}

std::string GitRepo::fetchBranch(const std::string& remote, const std::string& branch, std::string* error) const
{
    auto r = runProcess(gitArgs(root_, {"fetch", "--no-tags", remote, branch}));
    if (!r.ok()) {
        if (error)
            *error = r.err;
        return {};
    }
    return resolve("FETCH_HEAD");
}

std::string GitRepo::remoteFor(const std::string& host, const std::string& slug) const
{
    auto lower = [](std::string x) {
        for (auto& c : x)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return x;
    };
    for (const auto& r : remotes()) {
        std::string h, sl;
        if (hostAndSlugFromUrl(remoteUrl(r), h, sl) && lower(h) == lower(host) && lower(sl) == lower(slug))
            return r;
    }
    return {};
}

std::optional<GitRepo::PullRequestMeta> GitRepo::pullRequestMeta(const std::string& remote, int number) const
{
    std::string host, slug;
    if (!hostAndSlugFromUrl(remote.find("://") != std::string::npos ? remote : remoteUrl(remote), host, slug))
        return std::nullopt;
    auto r = runProcess({"gh", "pr", "view", std::to_string(number), "--repo", host + "/" + slug, "--json",
                         "title,state,baseRefName,baseRefOid,headRefOid"},
                        root_);
    JsonValue v;
    if (!r.ok() || !parseJson(r.out, v) || !v.isObject())
        return std::nullopt;
    PullRequestMeta m;
    m.title = v["title"].str();
    m.state = v["state"].str();
    m.baseRef = v["baseRefName"].str();
    m.baseSha = v["baseRefOid"].str();
    m.headSha = v["headRefOid"].str();
    if (m.baseRef.empty())
        return std::nullopt;
    return m;
}

GitRepo::FetchedPullRequest GitRepo::fetchPullRequestForReview(const std::string& remote, int number,
                                                               const std::string& baseRef) const
{
    FetchedPullRequest out;
    const auto pr = std::to_string(number);
    out.head = fetchPullRequest(remote, number, &out.error);
    if (out.head.empty()) {
        out.error = "cannot fetch pull/" + pr + "/head from " + remote + ": " + out.error;
        return out;
    }
    std::string baseTip, baseSha, ignored;
    std::string ref = baseRef;
    if (auto meta = pullRequestMeta(remote, number)) {
        out.title = meta->title;
        if (ref.empty())
            ref = meta->baseRef;
        if (ref == meta->baseRef)
            baseSha = meta->baseSha;
    }
    if (ref.empty()) {
        // Open, mergeable PRs have a test merge whose first parent is the target branch.
        if (!fetchBranch(remote, "pull/" + pr + "/merge", &ignored).empty())
            baseTip = resolve("FETCH_HEAD^1");
        if (!baseTip.empty()) {
            out.baseLabel = "merge-base with the target of PR #" + pr;
        } else {
            auto l = lines({"ls-remote", "--symref", remote, "HEAD"});
            if (!l.empty() && l.front().rfind("ref: refs/heads/", 0) == 0)
                ref = l.front().substr(16, l.front().find('\t') - 16);
            if (ref.empty())
                ref = "main";
        }
    }
    if (baseTip.empty()) {
        baseTip = fetchBranch(remote, ref, &out.error);
        if (baseTip.empty()) {
            out.error = "cannot fetch base branch " + ref + ": " + out.error;
            out.head.clear();
            return out;
        }
        out.baseLabel = "merge-base with " + (remote.find("://") == std::string::npos ? remote + "/" : std::string()) + ref;
        // Compare against the target as GitHub recorded it; for a merged PR the branch already
        // contains the PR.
        if (!baseSha.empty() && resolve(baseSha).empty())
            fetchBranch(remote, baseSha, &ignored);
        if (!baseSha.empty() && !resolve(baseSha).empty())
            baseTip = baseSha;
    }
    out.base = mergeBase(baseTip, out.head);
    if (out.base.empty()) {
        out.error = "no merge base between the pull request and its target branch";
        out.head.clear();
    } else if (out.base == out.head) {
        out.error = "pull request #" + pr + " is already merged into its target branch, so there is nothing to "
                    "compare against it; choose the base revision explicitly";
        out.head.clear();
    }
    return out;
}

bool hostAndSlugFromUrl(const std::string& url, std::string& host, std::string& slug)
{
    std::string rest;
    if (auto p = url.find("://"); p != std::string::npos) {
        rest = url.substr(p + 3);
        if (auto at = rest.find('@'); at != std::string::npos && at < rest.find('/'))
            rest = rest.substr(at + 1); // user@
        auto slash = rest.find('/');
        if (slash == std::string::npos)
            return false;
        host = rest.substr(0, slash);
        if (auto colon = host.find(':'); colon != std::string::npos)
            host.resize(colon); // port
        rest = rest.substr(slash + 1);
    } else if (auto colon = url.find(':'); colon != std::string::npos && url.find('@') < colon) {
        host = url.substr(url.find('@') + 1, colon - url.find('@') - 1); // git@host:owner/repo
        rest = url.substr(colon + 1);
    } else {
        return false;
    }
    while (!rest.empty() && rest.back() == '/')
        rest.pop_back();
    if (rest.size() > 4 && rest.compare(rest.size() - 4, 4, ".git") == 0)
        rest.resize(rest.size() - 4);
    if (host.empty() || std::count(rest.begin(), rest.end(), '/') != 1)
        return false;
    slug = rest;
    return true;
}

std::optional<PullRequestUrl> parsePullRequestUrl(const std::string& text)
{
    // [https://]host/owner/repo/pull/123[/files|/commits...][?query][#anchor]
    std::string s = text;
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    if (auto p = s.find("://"); p != std::string::npos)
        s = s.substr(p + 3);
    std::vector<std::string> parts;
    for (size_t b = 0; b <= s.size();) {
        size_t e = s.find_first_of("/?#", b);
        if (e == std::string::npos)
            e = s.size();
        parts.push_back(s.substr(b, e - b));
        if (e == s.size() || s[e] != '/')
            break;
        b = e + 1;
    }
    if (parts.size() < 5 || parts[0].find('.') == std::string::npos || parts[1].empty() || parts[2].empty() ||
        (parts[3] != "pull" && parts[3] != "pulls"))
        return std::nullopt;
    PullRequestUrl u;
    u.host = parts[0];
    u.slug = parts[1] + "/" + parts[2];
    try {
        size_t used = 0;
        u.number = std::stoi(parts[4], &used);
        if (used != parts[4].size() || u.number <= 0)
            return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
    return u;
}

std::string gitHubSlugFromUrl(const std::string& url)
{
    std::string rest;
    for (const char* prefix : {"git@github.com:", "ssh://git@github.com/", "https://github.com/",
                               "http://github.com/", "git://github.com/"}) {
        std::string p(prefix);
        if (url.rfind(p, 0) == 0) {
            rest = url.substr(p.size());
            break;
        }
    }
    if (rest.empty())
        return {};
    if (rest.size() > 4 && rest.compare(rest.size() - 4, 4, ".git") == 0)
        rest.resize(rest.size() - 4);
    while (!rest.empty() && rest.back() == '/')
        rest.pop_back();
    return rest;
}

} // namespace cr
