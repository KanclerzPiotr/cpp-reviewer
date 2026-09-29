#include "Snapshot.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <unistd.h>

namespace fs = std::filesystem;

namespace cr {

Revision Revision::commit(std::string ref, std::string sha, std::string label)
{
    Revision r;
    r.kind = Kind::Commit;
    r.ref = std::move(ref);
    r.sha = std::move(sha);
    r.label = std::move(label);
    return r;
}

Revision Revision::workingTree()
{
    Revision r;
    r.kind = Kind::WorkingTree;
    r.ref = "WORKTREE";
    r.label = "Working tree";
    return r;
}

Revision Revision::index()
{
    Revision r;
    r.kind = Kind::Index;
    r.ref = "INDEX";
    r.label = "Staged (index)";
    return r;
}

Revision Revision::directory(std::string path)
{
    Revision r;
    r.kind = Kind::Directory;
    r.ref = fs::absolute(path).lexically_normal().string();
    r.label = r.ref;
    return r;
}

std::string Revision::display() const
{
    if (!label.empty())
        return label;
    if (kind == Kind::Commit) {
        auto s = sha.substr(0, 10);
        return ref.empty() || ref == sha ? s : ref + " (" + s + ")";
    }
    return ref;
}

std::string Snapshot::absPath(const std::string& rel) const
{
    return (fs::path(root) / rel).string();
}

bool Snapshot::exists(const std::string& rel) const
{
    std::error_code ec;
    return fs::is_regular_file(absPath(rel), ec);
}

std::optional<std::string> Snapshot::read(const std::string& rel) const
{
    std::ifstream in(absPath(rel), std::ios::binary);
    if (!in)
        return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string Snapshot::relPath(const std::string& abs) const
{
    auto p = fs::path(abs).lexically_normal();
    auto r = p.lexically_relative(fs::path(root).lexically_normal());
    if (r.empty())
        return {};
    auto s = r.string();
    if (s.rfind("..", 0) == 0)
        return {};
    return s;
}

std::string cacheDirectory()
{
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    fs::path base;
    if (xdg && *xdg)
        base = xdg;
    else if (const char* home = std::getenv("HOME"))
        base = fs::path(home) / ".cache";
    else
        base = fs::temp_directory_path();
    auto dir = base / "cppreviewer";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir.string();
}

std::optional<Snapshot> materialize(const GitRepo* repo, const Revision& rev, std::string* error)
{
    Snapshot snap;
    snap.rev = rev;
    switch (rev.kind) {
    case Revision::Kind::Directory:
        snap.root = rev.ref;
        return snap;
    case Revision::Kind::WorkingTree:
        if (!repo)
            return std::nullopt;
        snap.root = repo->root();
        return snap;
    case Revision::Kind::Index: {
        if (!repo)
            return std::nullopt;
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        auto dir = fs::path(cacheDirectory()) / "index" / (std::to_string(getpid()) + "-" + std::to_string(stamp));
        if (!repo->exportIndex(dir.string(), error))
            return std::nullopt;
        snap.root = dir.string();
        return snap;
    }
    case Revision::Kind::Commit: {
        if (!repo || rev.sha.empty())
            return std::nullopt;
        auto dir = fs::path(cacheDirectory()) / "snapshots" / rev.sha;
        auto marker = dir / ".cppreviewer-complete";
        std::error_code ec;
        if (!fs::exists(marker, ec)) {
            fs::remove_all(dir, ec);
            if (!repo->exportCommit(rev.sha, dir.string(), error))
                return std::nullopt;
            std::ofstream(marker.string()) << rev.sha << '\n';
        }
        snap.root = dir.string();
        return snap;
    }
    }
    return std::nullopt;
}

std::optional<Revision> parseRevisionSpec(const GitRepo& repo, const std::string& spec)
{
    std::string upper = spec;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    if (upper == "WORKTREE" || upper == "WORKING" || upper == ".")
        return Revision::workingTree();
    if (upper == "INDEX" || upper == "STAGED")
        return Revision::index();
    auto sha = repo.resolve(spec);
    if (sha.empty())
        return std::nullopt;
    std::string label = spec == sha ? sha.substr(0, 10) : spec + " (" + sha.substr(0, 10) + ")";
    return Revision::commit(spec, sha, label);
}

namespace {

bool skipDir(const fs::path& p)
{
    auto name = p.filename().string();
    if (name.empty())
        return false;
    if (name[0] == '.')
        return true; // .git, .cache, .vscode...
    return name == "build" || name == "node_modules" || name.rfind("cmake-build-", 0) == 0 ||
           name == "out" || name == "_deps";
}

void walk(const fs::path& root, std::vector<std::string>& out, bool onlyCpp)
{
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    for (; it != end; it.increment(ec)) {
        if (ec)
            break;
        if (it->is_directory(ec)) {
            if (skipDir(it->path()))
                it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec))
            continue;
        auto rel = it->path().lexically_relative(root).string();
        if (!onlyCpp || isCppFile(rel))
            out.push_back(rel);
    }
    std::sort(out.begin(), out.end());
}

bool sameContent(const fs::path& a, const fs::path& b)
{
    std::error_code ec;
    if (fs::file_size(a, ec) != fs::file_size(b, ec))
        return false;
    std::ifstream ia(a, std::ios::binary), ib(b, std::ios::binary);
    std::istreambuf_iterator<char> ea, eb;
    return std::equal(std::istreambuf_iterator<char>(ia), ea, std::istreambuf_iterator<char>(ib), eb);
}

} // namespace

std::vector<ChangedFile> changedFiles(const GitRepo* repo, const Snapshot& base, const Snapshot& target)
{
    using K = Revision::Kind;
    if (repo && base.rev.kind == K::Commit) {
        switch (target.rev.kind) {
        case K::Commit: return repo->diffCommits(base.rev.sha, target.rev.sha);
        case K::WorkingTree: return repo->diffWorkingTree(base.rev.sha);
        case K::Index: return repo->diffIndex(base.rev.sha);
        case K::Directory: break;
        }
    }

    // Generic directory comparison (used for two directories, or unusual git combinations).
    std::vector<std::string> a, b;
    walk(base.root, a, false);
    walk(target.root, b, false);
    std::set<std::string> inB(b.begin(), b.end());
    std::set<std::string> inA(a.begin(), a.end());
    std::vector<ChangedFile> out;
    for (auto& p : a) {
        if (!inB.count(p)) {
            out.push_back({'D', p, {}, 0});
        } else if (!sameContent(fs::path(base.root) / p, fs::path(target.root) / p)) {
            out.push_back({'M', p, p, 0});
        }
    }
    for (auto& p : b)
        if (!inA.count(p))
            out.push_back({'A', {}, p, 0});
    std::sort(out.begin(), out.end(), [](const ChangedFile& x, const ChangedFile& y) {
        const auto& px = x.newPath.empty() ? x.oldPath : x.newPath;
        const auto& py = y.newPath.empty() ? y.oldPath : y.newPath;
        return px < py;
    });
    return out;
}

bool isHeaderFile(const std::string& path)
{
    auto ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".h" || ext == ".hh" || ext == ".hpp" || ext == ".hxx" || ext == ".h++" ||
           ext == ".inl" || ext == ".ipp" || ext == ".tpp" || ext == ".ixx";
}

bool isCppFile(const std::string& path)
{
    if (isHeaderFile(path))
        return true;
    auto ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".c++" || ext == ".cppm";
}

std::vector<std::string> listCppFiles(const Snapshot& snap)
{
    std::vector<std::string> out;
    walk(snap.root, out, true);
    return out;
}

} // namespace cr
