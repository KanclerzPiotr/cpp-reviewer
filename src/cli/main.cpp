// Command-line front end: prints the semantic review of two revisions.
#include "core/Session.hpp"

#include <cstring>
#include <iostream>

using namespace cr;

namespace {

void usage()
{
    std::cerr << "usage: cppreview-cli [options] [BASE [TARGET]]\n"
                 "       cppreview-cli --dirs OLD_DIR NEW_DIR\n"
                 "\n"
                 "Revisions: any git revision, WORKTREE or INDEX. Defaults: HEAD vs WORKTREE.\n"
                 "Options:\n"
                 "  -C DIR             repository (default: current directory)\n"
                 "  --pr N             review GitHub pull request N (fetched from --remote)\n"
                 "  --remote NAME      remote for --pr (default: origin)\n"
                 "  --base BRANCH      base branch for --pr (default: remote HEAD)\n"
                 "  -p PATH            compile_commands.json to use\n"
                 "  -w                 ignore whitespace\n"
                 "  --no-semantic      text diff only\n"
                 "  --diff             also print the line diff of each file\n"
                 "  --at old|new:FILE:LINE:COL  print the symbol at a position (definition/declaration)\n";
}

const char* color(LineTag t)
{
    switch (t) {
    case LineTag::Deleted: return "\033[31m";
    case LineTag::Inserted: return "\033[32m";
    case LineTag::Modified: return "\033[33m";
    case LineTag::Moved: return "\033[36m";
    case LineTag::RenameOnly: return "\033[35m";
    case LineTag::None: return "";
    }
    return "";
}

std::string where(const Location& l)
{
    if (!l.valid())
        return {};
    return l.file + ":" + std::to_string(l.line) + (l.endLine > l.line ? "-" + std::to_string(l.endLine) : "");
}

} // namespace

int main(int argc, char** argv)
{
    std::string repoDir = ".";
    std::string dbPath, remote = "origin", prBase;
    std::vector<std::string> positional;
    int pr = 0;
    bool dirs = false, printDiff = false;
    std::string at;
    ReviewOptions options;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a == "-C") {
            repoDir = next();
        } else if (a == "-p") {
            dbPath = next();
        } else if (a == "-w") {
            options.ignoreWhitespace = true;
        } else if (a == "--no-semantic") {
            options.semantic = false;
        } else if (a == "--diff") {
            printDiff = true;
        } else if (a == "--at") {
            at = next();
        } else if (a == "--dirs") {
            dirs = true;
        } else if (a == "--pr") {
            pr = std::atoi(next().c_str());
        } else if (a == "--remote") {
            remote = next();
        } else if (a == "--base") {
            prBase = next();
        } else {
            positional.push_back(a);
        }
    }

    std::optional<GitRepo> repo;
    Revision base, target;
    if (dirs) {
        if (positional.size() != 2) {
            usage();
            return 2;
        }
        base = Revision::directory(positional[0]);
        target = Revision::directory(positional[1]);
    } else {
        repo = GitRepo::open(repoDir);
        if (!repo) {
            std::cerr << "not a git repository: " << repoDir << "\n";
            return 1;
        }
        if (pr > 0) {
            std::string err;
            auto head = repo->fetchPullRequest(remote, pr, &err);
            if (head.empty()) {
                std::cerr << "cannot fetch PR #" << pr << ": " << err << "\n";
                return 1;
            }
            if (prBase.empty())
                prBase = repo->remoteDefaultBranch(remote);
            if (prBase.empty())
                prBase = "main";
            auto baseHead = repo->fetchBranch(remote, prBase, &err);
            auto mb = baseHead.empty() ? std::string{} : repo->mergeBase(baseHead, head);
            if (mb.empty()) {
                std::cerr << "cannot determine merge base with " << prBase << ": " << err << "\n";
                return 1;
            }
            base = Revision::commit(prBase, mb, "merge-base with " + remote + "/" + prBase);
            target = Revision::commit("PR #" + std::to_string(pr), head, "PR #" + std::to_string(pr));
        } else {
            std::string b = positional.size() > 0 ? positional[0] : "HEAD";
            std::string t = positional.size() > 1 ? positional[1] : "WORKTREE";
            auto rb = parseRevisionSpec(*repo, b);
            auto rt = parseRevisionSpec(*repo, t);
            if (!rb || !rt) {
                std::cerr << "unknown revision: " << (!rb ? b : t) << "\n";
                return 1;
            }
            base = *rb;
            target = *rt;
        }
    }

    ReviewSession session(repo, base, target);
    if (!dbPath.empty())
        session.setCompileDatabasePath(dbPath);
    std::string err;
    auto progress = [](const std::string& stage, int done, int total) {
        std::cerr << "\r\033[K" << stage << " " << done << "/" << total << std::flush;
    };
    if (!session.prepare(&err, progress)) {
        std::cerr << "\nerror: " << err << "\n";
        return 1;
    }
    if (!at.empty()) {
        // old|new:file:line:col
        auto p1 = at.find(':');
        auto p3 = at.rfind(':');
        auto p2 = at.rfind(':', p3 - 1);
        Side side = at.substr(0, p1) == "old" ? Side::Old : Side::New;
        auto file = at.substr(p1 + 1, p2 - p1 - 1);
        int line = std::atoi(at.substr(p2 + 1, p3 - p2 - 1).c_str());
        int col = std::atoi(at.substr(p3 + 1).c_str());
        auto info = session.project(side)->symbolAt(file, line, col);
        std::cerr << "\r\033[K";
        if (!info) {
            std::cout << "no symbol\n";
            return 1;
        }
        auto loc = [](const std::optional<Location>& l) {
            return l ? l->file + ":" + std::to_string(l->line) + ":" + std::to_string(l->col) : std::string("-");
        };
        std::cout << info->kind << " " << info->qualifiedName << "\n  type: " << info->type << "\n  usr: " << info->usr
                  << "\n  definition: " << loc(info->definition) << "\n  declaration: " << loc(info->declaration)
                  << "\n  comment: " << info->comment << "\n";
        return 0;
    }
    auto result = session.run(options, progress);
    std::cerr << "\r\033[K";

    std::cout << "\033[1mReview: " << base.display() << " → " << target.display() << "\033[0m\n";
    if (!session.compileDatabasePath().empty())
        std::cout << "compile database: " << session.compileDatabasePath() << "\n";
    std::cout << "\n\033[1mFiles\033[0m\n";
    for (const auto& f : result.files) {
        std::cout << "  " << f.change.status << "  " << f.path();
        if (f.change.status == 'R')
            std::cout << " (from " << f.oldPath << ")";
        std::cout << "  \033[32m+" << f.added << "\033[0m \033[31m-" << f.removed << "\033[0m";
        if (f.oldErrors || f.newErrors)
            std::cout << "  (parse errors: " << f.oldErrors << "/" << f.newErrors << ")";
        std::cout << "\n";
    }

    std::cout << "\n\033[1mSemantic changes\033[0m\n";
    for (const auto& c : result.changes) {
        std::cout << "  [" << changeKindName(c.kind) << "] " << c.title << "\n";
        if (c.oldLoc.valid() || c.newLoc.valid())
            std::cout << "      " << (c.oldLoc.valid() ? where(c.oldLoc) : "-") << "  →  "
                      << (c.newLoc.valid() ? where(c.newLoc) : "-") << "\n";
        if (!c.detail.empty())
            std::cout << "      " << c.detail << "\n";
        for (const auto& it : c.related)
            std::cout << "        " << (it.warning ? "⚠ " : "") << it.label << ": "
                      << (it.oldLoc.valid() ? where(it.oldLoc) : "-") << " → "
                      << (it.newLoc.valid() ? where(it.newLoc) : "-") << "\n";
    }
    if (!result.renames.empty()) {
        std::cout << "\n\033[1mRename map\033[0m\n";
        for (auto& [a, b] : result.renames)
            std::cout << "  " << a << " → " << b << "\n";
    }

    if (printDiff) {
        for (const auto& f : result.files) {
            std::cout << "\n\033[1m=== " << f.path() << "\033[0m\n";
            for (const auto& row : f.rows) {
                if (row.kind == RowKind::Equal)
                    continue;
                if (row.oldLine >= 0) {
                    auto& info = f.oldInfo[static_cast<size_t>(row.oldLine)];
                    std::cout << color(info.tag) << "-" << (row.oldLine + 1) << "\t" << f.oldLines[static_cast<size_t>(row.oldLine)]
                              << "\033[0m\n";
                }
                if (row.newLine >= 0) {
                    auto& info = f.newInfo[static_cast<size_t>(row.newLine)];
                    std::cout << color(info.tag) << "+" << (row.newLine + 1) << "\t" << f.newLines[static_cast<size_t>(row.newLine)]
                              << "\033[0m\n";
                }
            }
        }
    }
    return 0;
}
