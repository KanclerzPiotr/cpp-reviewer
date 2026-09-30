#include "PrCompare.hpp"

#include "Diff.hpp"
#include "Process.hpp"
#include "Snapshot.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace cr {

namespace {

std::vector<std::string> components(const std::string& path)
{
    std::vector<std::string> out;
    size_t b = 0;
    while (b <= path.size()) {
        auto e = path.find('/', b);
        if (e == std::string::npos)
            e = path.size();
        out.push_back(path.substr(b, e - b));
        b = e + 1;
    }
    return out;
}

// Number of trailing path components two paths share.
int commonSuffix(const std::string& a, const std::string& b)
{
    auto ca = components(a), cb = components(b);
    int k = 0;
    while (k < static_cast<int>(std::min(ca.size(), cb.size())) && ca[ca.size() - 1 - k] == cb[cb.size() - 1 - k])
        ++k;
    return k;
}

// `path` without its last `k` components, keeping the trailing slash ("" if nothing is left).
std::string dropSuffix(const std::string& path, int k)
{
    auto c = components(path);
    std::string out;
    for (size_t i = 0; i + static_cast<size_t>(k) < c.size(); ++i)
        out += c[i] + "/";
    return out;
}

} // namespace

std::optional<ResolvedPullRequest> resolvePullRequest(const GitRepo& repo, bool cached, const std::string& base,
                                                      const std::string& head, std::string title, std::string label)
{
    if (repo.resolve(base).empty() || repo.resolve(head).empty())
        return std::nullopt;
    ResolvedPullRequest pr;
    pr.repo = repo;
    pr.cached = cached;
    pr.base = base;
    pr.head = head;
    pr.title = std::move(title);
    pr.label = std::move(label);
    pr.files = repo.diffCommits(base, head);
    return pr;
}

std::optional<ResolvedPullRequest> resolvePullRequest(const PullRequestUrl& url, const GitRepo* local,
                                                      std::string* error)
{
    const auto label = url.slug + "#" + std::to_string(url.number);
    auto fail = [&](const std::string& msg) -> std::optional<ResolvedPullRequest> {
        if (error)
            *error = label + ": " + msg;
        return std::nullopt;
    };

    // A local clone of that repository.
    if (local) {
        if (auto remote = local->remoteFor(url.host, url.slug); !remote.empty()) {
            auto f = local->fetchPullRequestForReview(remote, url.number, "");
            if (!f.error.empty())
                return fail(f.error);
            return resolvePullRequest(*local, false, f.base, f.head, f.title, label);
        }
    }

    // The download cache: one bare repository per GitHub repository.
    std::string err;
    const auto dir = (fs::path(cacheDirectory()) / "repos" / url.host / (url.slug + ".git")).string();
    auto cache = GitRepo::openBare(dir, &err);
    if (!cache)
        return fail("cannot create " + dir + ": " + err);
    const auto fetchUrl = url.fetchUrl();

    // Shallow: ask GitHub for the head and the merge base, fetch just those two commits.
    if (auto meta = cache->pullRequestMeta(fetchUrl, url.number); meta && !meta->headSha.empty()) {
        std::vector<std::string> api{"gh", "api"};
        if (url.host != "github.com") {
            api.push_back("--hostname");
            api.push_back(url.host);
        }
        api.push_back("repos/" + url.slug + "/compare/" + meta->baseSha + "..." + meta->headSha);
        api.push_back("--jq");
        api.push_back(".merge_base_commit.sha");
        auto mb = runProcess(api);
        std::string mergeBase = mb.out;
        while (!mergeBase.empty() && std::isspace(static_cast<unsigned char>(mergeBase.back())))
            mergeBase.pop_back();
        if (mb.ok() && mergeBase.size() == 40) {
            auto r = runProcess({"git", "-C", dir, "fetch", "--quiet", "--no-tags", "--depth=1", fetchUrl, meta->headSha, mergeBase});
            if (r.ok())
                return resolvePullRequest(*cache, true, mergeBase, meta->headSha, meta->title, label);
            err = r.err;
        }
    }
    // Without the GitHub CLI: a full fetch of the PR and its target branch. Earlier shallow fetches
    // would hide the merge base, so complete the history first.
    if (auto sh = runProcess({"git", "-C", dir, "rev-parse", "--is-shallow-repository"}); sh.ok() && sh.out.rfind("true", 0) == 0)
        runProcess({"git", "-C", dir, "fetch", "--quiet", "--no-tags", "--unshallow", fetchUrl});
    auto f = cache->fetchPullRequestForReview(fetchUrl, url.number, "");
    if (!f.error.empty())
        return fail(f.error + (err.empty() ? "" : " (shallow fetch: " + err + ")"));
    return resolvePullRequest(*cache, true, f.base, f.head, f.title, label);
}

std::string PathMapping::toB(const std::string& aPath) const
{
    if (aPath.rfind(from, 0) == 0)
        return to + aPath.substr(from.size());
    return aPath;
}

std::string PathMapping::toA(const std::string& bPath) const
{
    if (bPath.rfind(to, 0) == 0)
        return from + bPath.substr(to.size());
    return bPath;
}

std::optional<PathMapping> PathMapping::parse(const std::string& text)
{
    auto eq = text.find('=');
    if (eq == std::string::npos)
        return std::nullopt;
    auto trim = [](std::string s) {
        s.erase(0, s.find_first_not_of(" \t"));
        s.erase(s.find_last_not_of(" \t") + 1);
        if (!s.empty() && s.back() != '/')
            s += '/';
        return s;
    };
    return PathMapping{trim(text.substr(0, eq)), trim(text.substr(eq + 1))};
}

PathMapping guessPathMapping(const ResolvedPullRequest& a, const ResolvedPullRequest& b)
{
    std::map<std::pair<std::string, std::string>, int> votes;
    auto vote = [&](const std::string& pa, const std::string& pb) {
        const int k = commonSuffix(pa, pb);
        if (k > 0)
            votes[{dropSuffix(pa, k), dropSuffix(pb, k)}] += k;
    };
    // Files both changed; each A file votes for its best match only.
    for (const auto& fa : a.files) {
        int best = 0;
        std::string bestB;
        for (const auto& fb : b.files)
            if (int k = commonSuffix(a.path(fa), b.path(fb)); k > best) {
                best = k;
                bestB = b.path(fb);
            }
        if (best > 0)
            vote(a.path(fa), bestB);
    }
    if (votes.empty() && b.repo) {
        // Nothing in common: look A's files up in B's whole tree by file name.
        std::unordered_map<std::string, std::vector<std::string>> byName;
        for (auto& p : b.repo->listTree(b.head))
            byName[p.substr(p.rfind('/') + 1)].push_back(p);
        for (const auto& fa : a.files) {
            const auto pa = a.path(fa);
            int best = 0;
            std::string bestB;
            for (const auto& pb : byName[pa.substr(pa.rfind('/') + 1)])
                if (int k = commonSuffix(pa, pb); k > best) {
                    best = k;
                    bestB = pb;
                }
            if (best > 0)
                vote(pa, bestB);
        }
    }
    PathMapping m;
    int bestVotes = 0;
    for (const auto& [prefixes, n] : votes)
        if (n > bestVotes) {
            bestVotes = n;
            m.from = prefixes.first;
            m.to = prefixes.second;
        }
    return m;
}

std::vector<FilePair> pairFiles(const ResolvedPullRequest& a, const ResolvedPullRequest& b, const PathMapping& m)
{
    std::vector<FilePair> out;
    std::unordered_map<std::string, const ChangedFile*> bByPath;
    for (const auto& fb : b.files)
        bByPath[b.path(fb)] = &fb;
    std::unordered_set<const ChangedFile*> usedB;
    for (const auto& fa : a.files) {
        FilePair p;
        p.a = &fa;
        p.pathA = a.path(fa);
        p.pathB = m.toB(p.pathA);
        if (auto it = bByPath.find(p.pathB); it != bByPath.end()) {
            p.b = it->second;
            usedB.insert(it->second);
        }
        out.push_back(p);
    }
    for (const auto& fb : b.files) {
        if (usedB.count(&fb))
            continue;
        FilePair p;
        p.b = &fb;
        p.pathB = b.path(fb);
        p.pathA = m.toA(p.pathB);
        out.push_back(p);
    }
    std::sort(out.begin(), out.end(), [](const FilePair& x, const FilePair& y) { return x.pathB < y.pathB; });
    return out;
}

namespace {

struct PatchText {
    std::vector<std::string> lines;
    std::vector<int> display; // real line number (1-based) per line, 0 for hunk headers
};

// git's default function-name rule: a line starting with a letter, '_' or '$'.
bool isFuncName(const std::string& l)
{
    return !l.empty() && (std::isalpha(static_cast<unsigned char>(l[0])) || l[0] == '_' || l[0] == '$');
}

// The change of one PR to one file, as unified-diff-like text with 3 lines of context. Hunk
// headers carry the enclosing function instead of line numbers, so that the same change in two
// repositories (where line numbers differ) compares equal.
PatchText patchOf(const ResolvedPullRequest& pr, const ChangedFile* cf, bool ignoreWhitespace)
{
    PatchText out;
    if (!cf || !pr.repo)
        return out;
    FileDiff fd;
    if (!cf->oldPath.empty())
        fd.oldLines = splitLines(pr.repo->fileAt(pr.base, cf->oldPath).value_or(""));
    if (!cf->newPath.empty())
        fd.newLines = splitLines(pr.repo->fileAt(pr.head, cf->newPath).value_or(""));
    diffLines(fd, ignoreWhitespace);

    constexpr int context = 3;
    const int n = static_cast<int>(fd.rows.size());
    std::vector<char> show(fd.rows.size(), 0);
    for (int r = 0; r < n; ++r)
        if (fd.rows[static_cast<size_t>(r)].kind != RowKind::Equal)
            for (int k = std::max(0, r - context); k <= std::min(n - 1, r + context); ++k)
                show[static_cast<size_t>(k)] = 1;
    for (int r = 0; r < n;) {
        if (!show[static_cast<size_t>(r)]) {
            ++r;
            continue;
        }
        // Header: the nearest function-like line above the hunk.
        std::string header = "@@";
        const int firstNew = fd.rows[static_cast<size_t>(r)].newLine;
        const int firstOld = fd.rows[static_cast<size_t>(r)].oldLine;
        const auto& src = firstNew >= 0 || fd.oldLines.empty() ? fd.newLines : fd.oldLines;
        for (int l = (firstNew >= 0 ? firstNew : firstOld) - 1; l >= 0; --l)
            if (isFuncName(src[static_cast<size_t>(l)])) {
                header += " " + src[static_cast<size_t>(l)];
                break;
            }
        out.lines.push_back(header);
        out.display.push_back(0);
        for (; r < n && show[static_cast<size_t>(r)];) {
            const auto& row = fd.rows[static_cast<size_t>(r)];
            if (row.kind == RowKind::Equal) {
                out.lines.push_back("  " + fd.newLines[static_cast<size_t>(row.newLine)]);
                out.display.push_back(row.newLine + 1);
                ++r;
                continue;
            }
            // A run of changes: removed lines first, then added ones, like a unified diff.
            int e = r;
            while (e < n && show[static_cast<size_t>(e)] && fd.rows[static_cast<size_t>(e)].kind != RowKind::Equal)
                ++e;
            for (int k = r; k < e; ++k)
                if (int l = fd.rows[static_cast<size_t>(k)].oldLine; l >= 0) {
                    out.lines.push_back("- " + fd.oldLines[static_cast<size_t>(l)]);
                    out.display.push_back(l + 1);
                }
            for (int k = r; k < e; ++k)
                if (int l = fd.rows[static_cast<size_t>(k)].newLine; l >= 0) {
                    out.lines.push_back("+ " + fd.newLines[static_cast<size_t>(l)]);
                    out.display.push_back(l + 1);
                }
            r = e;
        }
    }
    return out;
}

} // namespace

ReviewResult computeInterdiff(const ResolvedPullRequest& a, const ResolvedPullRequest& b,
                              const std::vector<FilePair>& pairs, bool ignoreWhitespace)
{
    ReviewResult result;
    for (const auto& p : pairs) {
        auto pa = patchOf(a, p.a, ignoreWhitespace);
        auto pb = patchOf(b, p.b, ignoreWhitespace);
        FileDiff fd;
        fd.synthetic = true;
        fd.oldPath = p.pathA;
        fd.newPath = p.pathB;
        fd.oldLines = std::move(pa.lines);
        fd.newLines = std::move(pb.lines);
        fd.oldDisplay = std::move(pa.display);
        fd.newDisplay = std::move(pb.display);
        diffLines(fd, ignoreWhitespace);
        fd.change.oldPath = fd.oldPath;
        fd.change.newPath = fd.newPath;
        fd.change.status = !p.a ? 'A' : !p.b ? 'D' : (fd.added == 0 && fd.removed == 0 ? '=' : 'M');
        result.files.push_back(std::move(fd));
    }
    return result;
}

std::vector<ChangedFile> finalFilePairs(const ResolvedPullRequest& a, const ResolvedPullRequest& b,
                                        const std::vector<FilePair>& pairs)
{
    // Whether each path exists after the pull request: deleted by it, or not there at all.
    auto existsAfter = [](const ResolvedPullRequest& pr, const ChangedFile* cf, const std::string& path) {
        if (cf)
            return !cf->newPath.empty();
        return pr.repo && pr.repo->fileAt(pr.head, path).has_value();
    };
    std::vector<ChangedFile> out;
    for (const auto& p : pairs) {
        ChangedFile cf;
        if (existsAfter(a, p.a, p.pathA))
            cf.oldPath = p.pathA;
        if (existsAfter(b, p.b, p.pathB))
            cf.newPath = p.pathB;
        if (cf.oldPath.empty() && cf.newPath.empty())
            continue;
        cf.status = cf.oldPath.empty() ? 'A' : cf.newPath.empty() ? 'D' : 'M';
        out.push_back(cf);
    }
    return out;
}

} // namespace cr
