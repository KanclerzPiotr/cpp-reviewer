#include "Reviewed.hpp"

#include "Diff.hpp"
#include "Snapshot.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace cr {

std::vector<Hunk> diffHunks(const FileDiff& fd)
{
    std::vector<Hunk> out;
    const int n = static_cast<int>(fd.rows.size());
    for (int r = 0; r < n;) {
        if (fd.rows[static_cast<size_t>(r)].kind == RowKind::Equal) {
            ++r;
            continue;
        }
        Hunk h;
        h.rowBegin = r;
        uint64_t key = hashString(fd.path());
        for (; r < n && fd.rows[static_cast<size_t>(r)].kind != RowKind::Equal; ++r) {
            const auto& row = fd.rows[static_cast<size_t>(r)];
            key = hashCombine(key, static_cast<uint64_t>(row.kind));
            if (row.oldLine >= 0) {
                if (h.oldBegin < 0)
                    h.oldBegin = row.oldLine;
                h.oldEnd = row.oldLine + 1;
                key = hashCombine(key, hashString(fd.oldLines[static_cast<size_t>(row.oldLine)]));
            }
            if (row.newLine >= 0) {
                if (h.newBegin < 0)
                    h.newBegin = row.newLine;
                h.newEnd = row.newLine + 1;
                key = hashCombine(key, hashString(fd.newLines[static_cast<size_t>(row.newLine)]));
            }
        }
        h.rowEnd = r;
        h.key = key;
        out.push_back(h);
    }
    return out;
}

namespace {

uint64_t hashRange(const ReviewResult& result, Side side, const Location& loc, uint64_t h)
{
    if (!loc.valid())
        return hashCombine(h, 0);
    h = hashCombine(h, hashString(loc.file));
    const int f = result.fileIndex(side, loc.file);
    if (f < 0)
        return h;
    const auto& fd = result.files[static_cast<size_t>(f)];
    const auto& lines = side == Side::Old ? fd.oldLines : fd.newLines;
    for (int l = loc.line; l <= std::max(loc.line, loc.endLine); ++l)
        if (l >= 1 && static_cast<size_t>(l) <= lines.size())
            h = hashCombine(h, hashString(lines[static_cast<size_t>(l - 1)]));
    return h;
}

} // namespace

uint64_t changeKey(const ReviewResult& result, const SemanticChange& change)
{
    uint64_t h = hashString("change");
    h = hashCombine(h, static_cast<uint64_t>(change.kind));
    h = hashCombine(h, hashString(change.title));
    h = hashRange(result, Side::Old, change.oldLoc, h);
    return hashRange(result, Side::New, change.newLoc, h);
}

ReviewedStore::ReviewedStore(const std::string& projectRoot)
{
    auto dir = fs::path(cacheDirectory()) / "reviewed";
    std::error_code ec;
    fs::create_directories(dir, ec);
    char name[32];
    std::snprintf(name, sizeof name, "%016llx.txt",
                  static_cast<unsigned long long>(hashString(fs::path(projectRoot).lexically_normal().string())));
    path_ = (dir / name).string();
    std::ifstream in(path_);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        char* end = nullptr;
        const auto k = std::strtoull(line.c_str(), &end, 16);
        if (end != line.c_str())
            keys_.insert(k);
    }
}

void ReviewedStore::set(uint64_t key, bool reviewed)
{
    const bool changed = reviewed ? keys_.insert(key).second : keys_.erase(key) > 0;
    if (changed)
        save();
}

void ReviewedStore::save() const
{
    if (path_.empty())
        return;
    const auto tmp = path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << "# C++ Reviewer: hunks and changes marked as reviewed\n";
        char buf[24];
        for (auto k : keys_) {
            std::snprintf(buf, sizeof buf, "%016llx\n", static_cast<unsigned long long>(k));
            out << buf;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path_, ec);
}

} // namespace cr
