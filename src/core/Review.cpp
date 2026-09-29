#include "Review.hpp"

#include "Diff.hpp"
#include "SemanticDiff.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <tuple>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

namespace cr {

namespace {

bool isBinary(const std::string& s)
{
    return s.find('\0') < std::min<size_t>(s.size(), 8000);
}

struct Word {
    int begin, end;
    bool ident;
    bool space;
};

bool isWordChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

std::vector<Word> splitWords(const std::string& s)
{
    std::vector<Word> out;
    int n = static_cast<int>(s.size());
    for (int i = 0; i < n;) {
        int j = i + 1;
        if (isWordChar(s[static_cast<size_t>(i)])) {
            while (j < n && isWordChar(s[static_cast<size_t>(j)]))
                ++j;
            bool ident = !std::isdigit(static_cast<unsigned char>(s[static_cast<size_t>(i)]));
            out.push_back({i, j, ident, false});
        } else if (s[static_cast<size_t>(i)] == ' ' || s[static_cast<size_t>(i)] == '\t') {
            while (j < n && (s[static_cast<size_t>(j)] == ' ' || s[static_cast<size_t>(j)] == '\t'))
                ++j;
            out.push_back({i, j, false, true});
        } else {
            out.push_back({i, j, false, false});
        }
        i = j;
    }
    return out;
}

std::vector<uint64_t> wordIds(const std::string& s, const std::vector<Word>& words, bool skipSpace)
{
    std::vector<uint64_t> ids;
    for (const auto& w : words)
        if (!(skipSpace && w.space))
            ids.push_back(hashString(std::string_view(s).substr(static_cast<size_t>(w.begin),
                                                                 static_cast<size_t>(w.end - w.begin))));
    return ids;
}

double lineSimilarity(const std::string& a, const std::string& b)
{
    auto wa = splitWords(a);
    auto wb = splitWords(b);
    auto ia = wordIds(a, wa, true);
    auto ib = wordIds(b, wb, true);
    if (ia.empty() || ib.empty())
        return ia.empty() && ib.empty() ? 1.0 : 0.0;
    return similarity(ia, ib);
}

int alnumCount(const std::string& s)
{
    int n = 0;
    for (char c : s)
        if (std::isalnum(static_cast<unsigned char>(c)))
            ++n;
    return n;
}

// Pairs deleted and inserted lines of one hunk so that similar lines end up side by side.
std::vector<std::pair<int, int>> alignHunk(const std::vector<std::string>& oldLines, const std::vector<int>& dels,
                                           const std::vector<std::string>& newLines, const std::vector<int>& ins)
{
    std::vector<std::pair<int, int>> pairs;
    const size_t p = dels.size(), q = ins.size();
    if (p == 0 || q == 0)
        return pairs;
    if (p * q > 40000) {
        for (size_t k = 0; k < std::min(p, q); ++k)
            pairs.emplace_back(static_cast<int>(k), static_cast<int>(k));
        return pairs;
    }
    std::vector<std::vector<double>> sim(p, std::vector<double>(q, 0.0));
    for (size_t a = 0; a < p; ++a)
        for (size_t b = 0; b < q; ++b)
            sim[a][b] = lineSimilarity(oldLines[static_cast<size_t>(dels[a])], newLines[static_cast<size_t>(ins[b])]);
    // score[a][b]: best total similarity using dels[a..] and ins[b..]
    std::vector<std::vector<double>> score(p + 1, std::vector<double>(q + 1, 0.0));
    for (size_t a = p; a-- > 0;)
        for (size_t b = q; b-- > 0;) {
            double best = std::max(score[a + 1][b], score[a][b + 1]);
            if (sim[a][b] >= 0.35)
                best = std::max(best, score[a + 1][b + 1] + sim[a][b]);
            score[a][b] = best;
        }
    size_t a = 0, b = 0;
    while (a < p && b < q) {
        if (sim[a][b] >= 0.35 && score[a][b] == score[a + 1][b + 1] + sim[a][b]) {
            pairs.emplace_back(static_cast<int>(a), static_cast<int>(b));
            ++a;
            ++b;
        } else if (score[a][b] == score[a + 1][b]) {
            ++a;
        } else {
            ++b;
        }
    }
    return pairs;
}

} // namespace

void diffLines(FileDiff& fd, bool ignoreWhitespace)
{
    auto hashLines = [&](const std::vector<std::string>& lines) {
        std::vector<uint64_t> h;
        h.reserve(lines.size());
        for (const auto& l : lines)
            h.push_back(hashString(ignoreWhitespace ? normalizeWhitespace(l) : l));
        return h;
    };
    fd.oldInfo.assign(fd.oldLines.size(), {});
    fd.newInfo.assign(fd.newLines.size(), {});
    auto edits = diffSequences(hashLines(fd.oldLines), hashLines(fd.newLines));

    std::vector<int> dels, ins;
    auto flush = [&] {
        auto pairs = alignHunk(fd.oldLines, dels, fd.newLines, ins);
        size_t a = 0, b = 0;
        auto emitUntil = [&](size_t ea, size_t eb) {
            for (; a < ea; ++a) {
                fd.rows.push_back({RowKind::Deleted, dels[a], -1});
                fd.oldInfo[static_cast<size_t>(dels[a])].tag = LineTag::Deleted;
            }
            for (; b < eb; ++b) {
                fd.rows.push_back({RowKind::Inserted, -1, ins[b]});
                fd.newInfo[static_cast<size_t>(ins[b])].tag = LineTag::Inserted;
            }
        };
        for (auto [pa, pb] : pairs) {
            emitUntil(static_cast<size_t>(pa), static_cast<size_t>(pb));
            fd.rows.push_back({RowKind::Modified, dels[a], ins[b]});
            fd.oldInfo[static_cast<size_t>(dels[a])].tag = LineTag::Modified;
            fd.newInfo[static_cast<size_t>(ins[b])].tag = LineTag::Modified;
            ++a;
            ++b;
        }
        emitUntil(dels.size(), ins.size());
        dels.clear();
        ins.clear();
    };

    for (const auto& e : edits) {
        if (e.op == EditOp::Equal) {
            flush();
            for (int k = 0; k < e.aEnd - e.aBegin; ++k)
                fd.rows.push_back({RowKind::Equal, e.aBegin + k, e.bBegin + k});
        } else if (e.op == EditOp::Delete) {
            for (int k = e.aBegin; k < e.aEnd; ++k)
                dels.push_back(k);
        } else {
            for (int k = e.bBegin; k < e.bEnd; ++k)
                ins.push_back(k);
        }
    }
    flush();

    for (auto& i : fd.newInfo)
        if (i.tag != LineTag::None)
            ++fd.added;
    for (auto& i : fd.oldInfo)
        if (i.tag != LineTag::None)
            ++fd.removed;
}

namespace {

bool isChanged(const LineInfo& i)
{
    return i.tag != LineTag::None;
}

int priority(ChangeKind k)
{
    switch (k) {
    case ChangeKind::Modified: return 1;
    case ChangeKind::Added:
    case ChangeKind::Removed:
    case ChangeKind::SignatureChanged: return 2;
    case ChangeKind::Renamed: return 3;
    case ChangeKind::Moved:
    case ChangeKind::Reordered:
    case ChangeKind::MovedLines: return 4;
    case ChangeKind::MovedCode:
    case ChangeKind::Extracted:
    case ChangeKind::Inlined:
    case ChangeKind::CopiedCode: return 5;
    case ChangeKind::SymbolRenamed: return 3;
    }
    return 0;
}

struct MovedBlock {
    int oldFile, oldLine, newFile, newLine, length;
};

// Text-level move detection across all files, similar to git's --color-moved.
std::vector<MovedBlock> detectMovedLines(ReviewResult& r)
{
    // Old lines are compared after applying the rename map, so moved-and-renamed code matches.
    auto key = [](const std::string& l) { return normalizeWhitespace(l); };
    auto oldKey = [&r](const std::string& l) { return normalizeWhitespace(applyRenames(l, r.renames)); };
    auto trivial = [](const std::string& l) { return alnumCount(l) < 3; };

    std::vector<std::vector<int>> oldRow(r.files.size()), newRow(r.files.size());
    std::unordered_map<std::string, std::vector<std::pair<int, int>>> added;
    for (size_t f = 0; f < r.files.size(); ++f) {
        auto& fd = r.files[f];
        oldRow[f].assign(fd.oldLines.size(), -1);
        newRow[f].assign(fd.newLines.size(), -1);
        for (size_t k = 0; k < fd.rows.size(); ++k) {
            if (fd.rows[k].oldLine >= 0)
                oldRow[f][static_cast<size_t>(fd.rows[k].oldLine)] = static_cast<int>(k);
            if (fd.rows[k].newLine >= 0)
                newRow[f][static_cast<size_t>(fd.rows[k].newLine)] = static_cast<int>(k);
        }
        for (size_t l = 0; l < fd.newLines.size(); ++l)
            if (isChanged(fd.newInfo[l]) && !trivial(fd.newLines[l]))
                added[key(fd.newLines[l])].emplace_back(static_cast<int>(f), static_cast<int>(l));
    }

    std::set<std::pair<int, int>> usedNew;
    std::vector<MovedBlock> blocks;
    for (size_t f = 0; f < r.files.size(); ++f) {
        auto& fd = r.files[f];
        for (size_t i = 0; i < fd.oldLines.size();) {
            if (!isChanged(fd.oldInfo[i]) || trivial(fd.oldLines[i])) {
                ++i;
                continue;
            }
            auto it = added.find(oldKey(fd.oldLines[i]));
            if (it == added.end()) {
                ++i;
                continue;
            }
            int bestLen = 0, bestNonTrivial = 0, bestChars = 0;
            std::pair<int, int> best{-1, -1};
            for (auto [g, j] : it->second) {
                if (usedNew.count({g, j}))
                    continue;
                // A whitespace-only edit in place is not a move.
                if (static_cast<size_t>(g) == f && oldRow[f][i] == newRow[static_cast<size_t>(g)][static_cast<size_t>(j)])
                    continue;
                auto& gd = r.files[static_cast<size_t>(g)];
                int len = 0, nonTrivial = 0, chars = 0;
                while (i + static_cast<size_t>(len) < fd.oldLines.size() &&
                       static_cast<size_t>(j + len) < gd.newLines.size()) {
                    const auto& ol = fd.oldLines[i + static_cast<size_t>(len)];
                    const auto& nl = gd.newLines[static_cast<size_t>(j + len)];
                    if (!isChanged(fd.oldInfo[i + static_cast<size_t>(len)]) ||
                        !isChanged(gd.newInfo[static_cast<size_t>(j + len)]) || usedNew.count({g, j + len}) ||
                        oldKey(ol) != key(nl))
                        break;
                    if (!trivial(ol)) {
                        ++nonTrivial;
                        chars += alnumCount(ol);
                    }
                    ++len;
                }
                if (nonTrivial > bestNonTrivial || (nonTrivial == bestNonTrivial && len > bestLen)) {
                    bestLen = len;
                    bestNonTrivial = nonTrivial;
                    bestChars = chars;
                    best = {g, j};
                }
            }
            if (best.first >= 0 && bestNonTrivial >= 2 && bestChars >= 25) {
                blocks.push_back({static_cast<int>(f), static_cast<int>(i), best.first, best.second, bestLen});
                for (int k = 0; k < bestLen; ++k) {
                    fd.oldInfo[i + static_cast<size_t>(k)].tag = LineTag::Moved;
                    r.files[static_cast<size_t>(best.first)].newInfo[static_cast<size_t>(best.second + k)].tag =
                        LineTag::Moved;
                    usedNew.emplace(best.first, best.second + k);
                }
                i += static_cast<size_t>(bestLen);
            } else {
                ++i;
            }
        }
    }
    return blocks;
}

// Lines whose only difference is a consistent rename get their own tag, linked to the
// change that reports the rename.
void markRenameOnlyLines(ReviewResult& r)
{
    if (r.renames.empty())
        return;
    auto simple = [](const std::string& qualified) {
        auto p = qualified.rfind("::");
        return p == std::string::npos ? qualified : qualified.substr(p + 2);
    };
    std::map<std::pair<std::string, std::string>, int> owner;
    for (size_t c = 0; c < r.changes.size(); ++c) {
        const auto& ch = r.changes[c];
        if (ch.kind == ChangeKind::SymbolRenamed)
            owner.emplace(std::make_pair(ch.oldName, ch.newName), static_cast<int>(c));
        else if (ch.kind == ChangeKind::Renamed || ch.kind == ChangeKind::Moved)
            owner.emplace(std::make_pair(simple(ch.oldName), simple(ch.newName)), static_cast<int>(c));
    }
    for (auto& fd : r.files) {
        for (const auto& row : fd.rows) {
            if (row.kind != RowKind::Modified)
                continue;
            const auto& a = fd.oldLines[static_cast<size_t>(row.oldLine)];
            const auto& b = fd.newLines[static_cast<size_t>(row.newLine)];
            std::vector<Word> wa, wb;
            for (auto& w : splitWords(a))
                if (!w.space)
                    wa.push_back(w);
            for (auto& w : splitWords(b))
                if (!w.space)
                    wb.push_back(w);
            if (wa.size() != wb.size())
                continue;
            bool ok = true;
            int change = -1;
            bool renamedSomething = false;
            for (size_t k = 0; k < wa.size() && ok; ++k) {
                auto ta = a.substr(static_cast<size_t>(wa[k].begin), static_cast<size_t>(wa[k].end - wa[k].begin));
                auto tb = b.substr(static_cast<size_t>(wb[k].begin), static_cast<size_t>(wb[k].end - wb[k].begin));
                if (ta == tb)
                    continue;
                auto it = r.renames.find(ta);
                if (wa[k].ident && it != r.renames.end() && it->second == tb) {
                    renamedSomething = true;
                    if (auto o = owner.find({ta, tb}); change < 0 && o != owner.end())
                        change = o->second;
                } else {
                    ok = false;
                }
            }
            if (ok && renamedSomething) {
                for (auto* info : {&fd.oldInfo[static_cast<size_t>(row.oldLine)], &fd.newInfo[static_cast<size_t>(row.newLine)]}) {
                    info->tag = LineTag::RenameOnly;
                    info->change = change;
                }
            }
        }
    }
}

void paint(std::vector<LineInfo>& info, const Location& loc, int change, const std::vector<SemanticChange>& all)
{
    if (!loc.valid() || loc.external)
        return;
    int end = std::max(loc.line, loc.endLine);
    for (int l = loc.line; l <= end; ++l) {
        if (l < 1 || static_cast<size_t>(l) > info.size())
            continue;
        auto& li = info[static_cast<size_t>(l - 1)];
        if (!isChanged(li))
            continue;
        if (li.change < 0 ||
            priority(all[static_cast<size_t>(change)].kind) >= priority(all[static_cast<size_t>(li.change)].kind))
            li.change = change;
    }
}

bool overlaps(const Location& loc, const std::string& file, int begin, int end)
{
    if (!loc.valid() || loc.file != file)
        return false;
    int le = std::max(loc.line, loc.endLine);
    return loc.line <= end && le >= begin;
}

// Every place where `from` was replaced by `to`: in paired (modified) lines and in lines of
// moved blocks. Words of the two lines are aligned, so only true replacements count.
void collectRenameOccurrences(const ReviewResult& r, const std::vector<MovedBlock>& blocks, SemanticChange& c,
                              const std::string& from, const std::string& to)
{
    auto scan = [&](const FileDiff& of, int oldLine, const FileDiff& nf, int newLine) {
        const auto& a = of.oldLines[static_cast<size_t>(oldLine)];
        const auto& b = nf.newLines[static_cast<size_t>(newLine)];
        if (a.find(from) == std::string::npos || b.find(to) == std::string::npos)
            return;
        auto wa = splitWords(a), wb = splitWords(b);
        std::vector<Word> na, nb;
        for (auto& w : wa)
            if (!w.space)
                na.push_back(w);
        for (auto& w : wb)
            if (!w.space)
                nb.push_back(w);
        auto edits = diffSequences(wordIds(a, na, false), wordIds(b, nb, false));
        for (size_t k = 0; k + 1 < edits.size(); ++k) {
            const auto& d = edits[k];
            const auto& ins = edits[k + 1];
            if (d.op != EditOp::Delete || ins.op != EditOp::Insert || d.aEnd - d.aBegin != ins.bEnd - ins.bBegin)
                continue;
            for (int m = 0; m < d.aEnd - d.aBegin; ++m) {
                const auto& x = na[static_cast<size_t>(d.aBegin + m)];
                const auto& y = nb[static_cast<size_t>(ins.bBegin + m)];
                if (a.compare(static_cast<size_t>(x.begin), static_cast<size_t>(x.end - x.begin), from) == 0 &&
                    b.compare(static_cast<size_t>(y.begin), static_cast<size_t>(y.end - y.begin), to) == 0) {
                    c.related.push_back({"renamed", {of.oldPath, oldLine + 1, x.begin + 1, 0, false},
                                         {nf.newPath, newLine + 1, y.begin + 1, 0, false}});
                }
            }
        }
    };
    for (const auto& fd : r.files)
        for (const auto& row : fd.rows)
            if (row.kind == RowKind::Modified)
                scan(fd, row.oldLine, fd, row.newLine);
    for (const auto& b : blocks)
        for (int k = 0; k < b.length; ++k)
            scan(r.files[static_cast<size_t>(b.oldFile)], b.oldLine + k, r.files[static_cast<size_t>(b.newFile)],
                 b.newLine + k);
    // Keep them in reading order (moved blocks were appended at the end).
    std::stable_sort(c.related.begin(), c.related.end(), [](const RelatedItem& x, const RelatedItem& y) {
        return std::tie(x.newLoc.file, x.newLoc.line, x.newLoc.col) < std::tie(y.newLoc.file, y.newLoc.line, y.newLoc.col);
    });
}

bool isCppKeyword(const std::string& w)
{
    static const std::set<std::string> kw = {"return", "case", "throw", "new", "delete", "co_await", "co_return",
                                             "co_yield", "else", "do", "sizeof", "decltype", "noexcept", "not",
                                             "and", "or", "typeid", "alignof", "static_cast", "const_cast"};
    return kw.count(w) > 0;
}

// Text-level usages of `name` on one side of all changed files. For functions only `name(` and
// `name<...>(` count. Matches inside `exclude` (the entity itself) are skipped.
void collectUsages(const ReviewResult& r, Side side, const std::string& name, bool function, const Location& exclude,
                   const std::string& memberClass, const std::vector<Location>& memberScope,
                   const std::function<void(const FileDiff&, int line, int col, bool declaration)>& emit)
{
    if (name.empty() || name == "(anonymous)")
        return;
    for (const auto& fd : r.files) {
        const auto& path = side == Side::Old ? fd.oldPath : fd.newPath;
        if (path.empty() || !fd.cpp)
            continue;
        const auto& lines = side == Side::Old ? fd.oldLines : fd.newLines;
        for (size_t l = 0; l < lines.size(); ++l) {
            const auto& text = lines[l];
            if (text.find(name) == std::string::npos)
                continue;
            if (exclude.valid() && exclude.file == path && static_cast<int>(l) + 1 >= exclude.line &&
                static_cast<int>(l) + 1 <= std::max(exclude.line, exclude.endLine))
                continue;
            auto words = splitWords(text);
            // Skip // comments.
            auto commentPos = text.find("//");
            for (size_t w = 0; w < words.size(); ++w) {
                const auto& wd = words[w];
                if (commentPos != std::string::npos && static_cast<size_t>(wd.begin) > commentPos)
                    break;
                if (!wd.ident || text.compare(static_cast<size_t>(wd.begin), static_cast<size_t>(wd.end - wd.begin), name) != 0)
                    continue;
                size_t n = w + 1;
                while (n < words.size() && words[n].space)
                    ++n;
                if (function) {
                    bool call = n < words.size() && (text[static_cast<size_t>(words[n].begin)] == '(' ||
                                                     text[static_cast<size_t>(words[n].begin)] == '<');
                    if (!call)
                        continue;
                }
                // Declaration if preceded (past any `ns::` qualifiers) by a type-like token.
                int p = static_cast<int>(w) - 1;
                auto tok = [&](int i) {
                    return text.substr(static_cast<size_t>(words[static_cast<size_t>(i)].begin),
                                       static_cast<size_t>(words[static_cast<size_t>(i)].end - words[static_cast<size_t>(i)].begin));
                };
                auto skipSpace = [&](int& i) {
                    while (i >= 0 && words[static_cast<size_t>(i)].space)
                        --i;
                };
                skipSpace(p);
                while (p >= 1 && tok(p) == ":" && tok(p - 1) == ":") {
                    p -= 2;
                    skipSpace(p);
                    if (p >= 0 && words[static_cast<size_t>(p)].ident)
                        --p;
                    skipSpace(p);
                }
                // Members: only `obj.name`, `ptr->name`, `Class::name`, or unqualified use inside the class.
                if (!memberClass.empty()) {
                    int b = static_cast<int>(w) - 1;
                    skipSpace(b);
                    bool ok = false;
                    if (b >= 0 && tok(b) == ".")
                        ok = true;
                    else if (b >= 1 && tok(b) == ">" && tok(b - 1) == "-")
                        ok = true;
                    else if (b >= 1 && tok(b) == ":" && tok(b - 1) == ":") {
                        int q = b - 2;
                        skipSpace(q);
                        // `Class::name` or `Class<T>::name`
                        if (q >= 0 && tok(q) == ">") {
                            int depth = 0;
                            for (; q >= 0; --q) {
                                if (tok(q) == ">")
                                    ++depth;
                                else if (tok(q) == "<" && --depth == 0) {
                                    --q;
                                    break;
                                }
                            }
                            skipSpace(q);
                        }
                        ok = q >= 0 && tok(q) == memberClass;
                    } else {
                        const auto& path = side == Side::Old ? fd.oldPath : fd.newPath;
                        for (const auto& s : memberScope)
                            if (s.file == path && static_cast<int>(l) + 1 >= s.line &&
                                static_cast<int>(l) + 1 <= std::max(s.line, s.endLine))
                                ok = true;
                    }
                    if (!ok)
                        continue;
                }
                bool declaration = false;
                if (p >= 0) {
                    auto t = tok(p);
                    declaration = (words[static_cast<size_t>(p)].ident && !isCppKeyword(t)) || t == "*" || t == "&" ||
                                  t == ">" || t == "~";
                }
                emit(fd, static_cast<int>(l), wd.begin + 1, declaration);
            }
        }
    }
}

// Fills SemanticChange::related (and the identifiers to highlight) for every change.
void buildRelated(ReviewResult& r, const std::vector<MovedBlock>& blocks)
{
    constexpr size_t kMaxItems = 400;
    auto simple = [](const std::string& q) {
        auto p = q.rfind("::");
        return p == std::string::npos ? q : q.substr(p + 2);
    };
    // Counterpart of a line on the other side (same row of the diff), for context when navigating.
    std::vector<std::vector<int>> newToOld(r.files.size()), oldToNew(r.files.size());
    for (size_t f = 0; f < r.files.size(); ++f) {
        const auto& fd = r.files[f];
        newToOld[f].assign(fd.newLines.size(), -1);
        oldToNew[f].assign(fd.oldLines.size(), -1);
        for (const auto& row : fd.rows)
            if (row.oldLine >= 0 && row.newLine >= 0) {
                newToOld[f][static_cast<size_t>(row.newLine)] = row.oldLine;
                oldToNew[f][static_cast<size_t>(row.oldLine)] = row.newLine;
            }
    }
    auto usage = [&](SemanticChange& c, Side side, const std::string& name, const Location& exclude,
                     const std::string& label, bool warnIfUnchanged, bool warn = false) {
        const auto& cls = side == Side::Old ? c.memberClassOld : c.memberClassNew;
        const auto& scope = side == Side::Old ? c.memberScopeOld : c.memberScopeNew;
        collectUsages(r, side, name, c.isFunction, exclude, cls, scope, [&](const FileDiff& fd, int line, int col, bool decl) {
            if (c.related.size() >= kMaxItems)
                return;
            const auto f = static_cast<size_t>(&fd - r.files.data());
            RelatedItem it;
            it.label = decl ? "declaration" : label;
            it.warning = warn;
            Location here{side == Side::Old ? fd.oldPath : fd.newPath, line + 1, col, 0, false};
            if (side == Side::New) {
                it.newLoc = here;
                if (int o = newToOld[f][static_cast<size_t>(line)]; o >= 0)
                    it.oldLoc = {fd.oldPath, o + 1, 1, 0, false};
                if (warnIfUnchanged && !decl && fd.newInfo[static_cast<size_t>(line)].tag == LineTag::None) {
                    it.label = "not updated: " + label;
                    it.warning = true;
                }
            } else {
                it.oldLoc = here;
                it.onOldSide = true;
                if (int n = oldToNew[f][static_cast<size_t>(line)]; n >= 0)
                    it.newLoc = {fd.newPath, n + 1, 1, 0, false};
            }
            c.related.push_back(std::move(it));
        });
    };

    for (auto& c : r.changes) {
        const auto on = simple(c.oldName), nn = simple(c.newName);
        switch (c.kind) {
        case ChangeKind::SymbolRenamed:
            collectRenameOccurrences(r, blocks, c, on, nn);
            c.highlightOld = on;
            c.highlightNew = nn;
            break;
        case ChangeKind::Renamed:
            collectRenameOccurrences(r, blocks, c, on, nn);
            c.highlightOld = on;
            c.highlightNew = nn;
            break;
        case ChangeKind::Extracted:
        case ChangeKind::CopiedCode:
            c.related.push_back({c.kind == ChangeKind::Extracted ? "extracted code" : "original code", c.oldLoc, c.newLoc});
            usage(c, Side::New, nn, c.newLoc, "call", false);
            c.highlightNew = nn;
            break;
        case ChangeKind::Inlined:
            c.related.push_back({"inlined body", c.oldLoc, c.newLoc});
            usage(c, Side::Old, on, c.oldLoc, "former call", false);
            c.highlightOld = on;
            break;
        case ChangeKind::SignatureChanged:
            c.related.push_back({"definition", c.oldLoc, c.newLoc});
            usage(c, Side::New, nn, c.newLoc, "call", true);
            c.highlightOld = on;
            c.highlightNew = nn;
            break;
        case ChangeKind::Added:
            c.related.push_back({"definition", {}, c.newLoc});
            usage(c, Side::New, nn, c.newLoc, c.isFunction ? "call" : "use", false);
            c.highlightNew = nn;
            break;
        case ChangeKind::Removed:
            c.related.push_back({"definition", c.oldLoc, {}});
            usage(c, Side::Old, on, c.oldLoc, c.isFunction ? "former call" : "former use", false);
            // Anything still using the name deserves a look (another overload may be picked now).
            usage(c, Side::New, on, {}, "still referenced", false, true);
            c.highlightOld = on;
            c.highlightNew = on;
            break;
        case ChangeKind::Moved:
        case ChangeKind::Reordered:
            c.related.push_back({"before → after", c.oldLoc, c.newLoc});
            if (c.isFunction && on != nn)
                usage(c, Side::New, nn, c.newLoc, "call", false);
            c.highlightOld = on;
            c.highlightNew = nn;
            break;
        case ChangeKind::MovedCode:
        case ChangeKind::MovedLines:
            c.related.push_back({"moved code", c.oldLoc, c.newLoc});
            break;
        case ChangeKind::Modified: {
            // One item per changed hunk inside the entity.
            int f = r.fileIndex(Side::New, c.newLoc.file);
            if (f < 0)
                break;
            const auto& fd = r.files[static_cast<size_t>(f)];
            auto inside = [&](const DiffRow& row) {
                return (row.newLine >= 0 && row.newLine + 1 >= c.newLoc.line && row.newLine + 1 <= c.newLoc.endLine) ||
                       (row.oldLine >= 0 && c.oldLoc.file == fd.oldPath && row.oldLine + 1 >= c.oldLoc.line &&
                        row.oldLine + 1 <= c.oldLoc.endLine);
            };
            for (size_t k = 0; k < fd.rows.size();) {
                if (fd.rows[k].kind == RowKind::Equal || !inside(fd.rows[k])) {
                    ++k;
                    continue;
                }
                RelatedItem it;
                int plus = 0, minus = 0;
                for (; k < fd.rows.size() && fd.rows[k].kind != RowKind::Equal && inside(fd.rows[k]); ++k) {
                    const auto& row = fd.rows[k];
                    if (row.oldLine >= 0) {
                        ++minus;
                        if (!it.oldLoc.valid())
                            it.oldLoc = {fd.oldPath, row.oldLine + 1, 1, 0, false};
                        it.oldLoc.endLine = row.oldLine + 1;
                    }
                    if (row.newLine >= 0) {
                        ++plus;
                        if (!it.newLoc.valid())
                            it.newLoc = {fd.newPath, row.newLine + 1, 1, 0, false};
                        it.newLoc.endLine = row.newLine + 1;
                    }
                }
                it.label = "hunk +" + std::to_string(plus) + " −" + std::to_string(minus);
                c.related.push_back(std::move(it));
            }
            break;
        }
        }
    }
}

} // namespace

std::string applyRenames(const std::string& line, const std::map<std::string, std::string>& renames)
{
    if (renames.empty())
        return line;
    std::string out;
    out.reserve(line.size());
    for (size_t i = 0; i < line.size();) {
        if (isWordChar(line[i])) {
            size_t j = i;
            while (j < line.size() && isWordChar(line[j]))
                ++j;
            auto word = line.substr(i, j - i);
            auto it = renames.find(word);
            out += it == renames.end() ? word : it->second;
            i = j;
        } else {
            out += line[i++];
        }
    }
    return out;
}

int ReviewResult::fileIndex(Side side, const std::string& path) const
{
    for (size_t i = 0; i < files.size(); ++i)
        if ((side == Side::Old ? files[i].oldPath : files[i].newPath) == path)
            return static_cast<int>(i);
    return -1;
}

ReviewResult computeReview(const std::vector<ChangedFile>& changed, const Snapshot& base, const Snapshot& target,
                           ClangProject* oldProject, ClangProject* newProject, const ReviewOptions& options,
                           const ProgressFn& progress, const std::atomic<bool>* cancel)
{
    ReviewResult r;
    auto cancelled = [&] { return cancel && cancel->load(); };

    // 1. Load both versions and diff them line by line.
    int done = 0;
    for (const auto& cf : changed) {
        if (cancelled())
            return r;
        FileDiff fd;
        fd.change = cf;
        fd.oldPath = cf.oldPath;
        fd.newPath = cf.newPath;
        fd.cpp = isCppFile(fd.path());
        std::string oldText, newText;
        if (!fd.oldPath.empty())
            oldText = base.read(fd.oldPath).value_or("");
        if (!fd.newPath.empty())
            newText = target.read(fd.newPath).value_or("");
        fd.binary = isBinary(oldText) || isBinary(newText);
        if (!fd.binary) {
            fd.oldLines = splitLines(oldText);
            fd.newLines = splitLines(newText);
            diffLines(fd, options.ignoreWhitespace);
        }
        r.files.push_back(std::move(fd));
        if (progress)
            progress("Diffing files", ++done, static_cast<int>(changed.size()));
    }

    // 2. Parse C++ files of both revisions in parallel.
    struct Job {
        size_t file;
        Side side;
    };
    std::vector<Job> jobs;
    const bool semantic = options.semantic && oldProject && newProject;
    if (semantic) {
        for (size_t f = 0; f < r.files.size(); ++f) {
            const auto& fd = r.files[f];
            if (!fd.cpp || fd.binary)
                continue;
            if (!fd.oldPath.empty())
                jobs.push_back({f, Side::Old});
            if (!fd.newPath.empty())
                jobs.push_back({f, Side::New});
        }
    }
    std::vector<std::shared_ptr<const ParsedFile>> parsed(jobs.size());
    {
        std::atomic<size_t> next{0};
        std::atomic<int> finished{0};
        std::mutex progressMutex;
        auto worker = [&] {
            for (;;) {
                if (cancelled())
                    return;
                size_t k = next++;
                if (k >= jobs.size())
                    return;
                const auto& job = jobs[k];
                const auto& fd = r.files[job.file];
                parsed[k] = job.side == Side::Old ? oldProject->parse(fd.oldPath) : newProject->parse(fd.newPath);
                int d = ++finished;
                if (progress) {
                    std::lock_guard lock(progressMutex);
                    progress("Parsing C++ (libclang)", d, static_cast<int>(jobs.size()));
                }
            }
        };
        unsigned n = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
        n = std::min<unsigned>(n, static_cast<unsigned>(std::max<size_t>(jobs.size(), 1)));
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < n; ++t)
            threads.emplace_back(worker);
        for (auto& t : threads)
            t.join();
    }
    if (cancelled())
        return r;

    // 3. Semantic analysis.
    if (semantic) {
        if (progress)
            progress("Analyzing changes", 0, 1);
        SemanticInput in;
        for (size_t k = 0; k < jobs.size(); ++k) {
            if (!parsed[k])
                continue;
            auto& fd = r.files[jobs[k].file];
            (jobs[k].side == Side::Old ? fd.oldErrors : fd.newErrors) = parsed[k]->errorCount;
            auto& list = jobs[k].side == Side::Old ? in.oldEntities : in.newEntities;
            for (const auto& e : parsed[k]->entities)
                list.push_back({parsed[k].get(), &e});
        }
        for (const auto& fd : r.files)
            if (!fd.oldPath.empty() && !fd.newPath.empty() && fd.oldPath != fd.newPath)
                in.fileRenames[fd.oldPath] = fd.newPath;
        in.lineChanged = [&r](Side side, const std::string& file, int line) {
            int f = r.fileIndex(side, file);
            if (f < 0 || line < 1)
                return false;
            const auto& info = side == Side::Old ? r.files[static_cast<size_t>(f)].oldInfo
                                                 : r.files[static_cast<size_t>(f)].newInfo;
            return static_cast<size_t>(line) <= info.size() && isChanged(info[static_cast<size_t>(line - 1)]);
        };
        auto out = analyzeEntities(in);
        r.changes = std::move(out.changes);
        r.renames = std::move(out.renames);
    }

    // 4. Text-level moves and rename-only lines.
    auto blocks = detectMovedLines(r);
    markRenameOnlyLines(r);

    // Statistics for modified entities; drop ones whose lines didn't change (comment-only edits etc.).
    std::vector<bool> unchanged(r.changes.size(), false);
    for (size_t ci = 0; ci < r.changes.size(); ++ci) {
        auto& c = r.changes[ci];
        if (c.kind != ChangeKind::Modified)
            continue;
        int plus = 0, minus = 0;
        if (int f = r.fileIndex(Side::New, c.newLoc.file); f >= 0)
            for (int l = c.newLoc.line; l <= c.newLoc.endLine; ++l)
                if (l >= 1 && static_cast<size_t>(l) <= r.files[static_cast<size_t>(f)].newInfo.size() &&
                    isChanged(r.files[static_cast<size_t>(f)].newInfo[static_cast<size_t>(l - 1)]))
                    ++plus;
        if (int f = r.fileIndex(Side::Old, c.oldLoc.file); f >= 0)
            for (int l = c.oldLoc.line; l <= c.oldLoc.endLine; ++l)
                if (l >= 1 && static_cast<size_t>(l) <= r.files[static_cast<size_t>(f)].oldInfo.size() &&
                    isChanged(r.files[static_cast<size_t>(f)].oldInfo[static_cast<size_t>(l - 1)]))
                    ++minus;
        if (plus == 0 && minus == 0) {
            unchanged[ci] = true;
            continue;
        }
        auto dash = c.title.find(" — ");
        auto stats = " (+" + std::to_string(plus) + " −" + std::to_string(minus) + ")";
        if (dash == std::string::npos)
            c.title += stats;
        else
            c.title.insert(dash, stats);
    }

    if (std::find(unchanged.begin(), unchanged.end(), true) != unchanged.end()) {
        std::vector<SemanticChange> kept;
        for (size_t ci = 0; ci < r.changes.size(); ++ci)
            if (!unchanged[ci])
                kept.push_back(std::move(r.changes[ci]));
        r.changes = std::move(kept);
    }

    // Link moved text blocks to a semantic change that explains them, or report them on their own.
    for (const auto& b : blocks) {
        const auto& of = r.files[static_cast<size_t>(b.oldFile)];
        const auto& nf = r.files[static_cast<size_t>(b.newFile)];
        int owner = -1;
        for (size_t c = 0; c < r.changes.size(); ++c) {
            auto k = r.changes[c].kind;
            if (k == ChangeKind::Added || k == ChangeKind::Removed || k == ChangeKind::SymbolRenamed)
                continue;
            if (overlaps(r.changes[c].oldLoc, of.oldPath, b.oldLine + 1, b.oldLine + b.length) &&
                overlaps(r.changes[c].newLoc, nf.newPath, b.newLine + 1, b.newLine + b.length)) {
                owner = static_cast<int>(c);
                break;
            }
        }
        if (owner < 0) {
            SemanticChange c;
            c.kind = ChangeKind::MovedLines;
            c.oldLoc = {of.oldPath, b.oldLine + 1, 1, b.oldLine + b.length, false};
            c.newLoc = {nf.newPath, b.newLine + 1, 1, b.newLine + b.length, false};
            c.similarity = 1.0;
            c.title = "Moved " + std::to_string(b.length) + " lines " +
                      (b.oldFile == b.newFile ? "within " + nf.newPath + " (line " + std::to_string(b.oldLine + 1) +
                                                    " → " + std::to_string(b.newLine + 1) + ")"
                                              : "from " + of.oldPath + ":" + std::to_string(b.oldLine + 1) + " to " +
                                                    nf.newPath + ":" + std::to_string(b.newLine + 1));
            r.changes.push_back(std::move(c));
        }
    }

    buildRelated(r, blocks);

    // 5. Attach changes to lines and files.
    for (size_t c = 0; c < r.changes.size(); ++c) {
        const auto& ch = r.changes[c];
        if (ch.kind == ChangeKind::SymbolRenamed)
            continue;
        if (int f = r.fileIndex(Side::Old, ch.oldLoc.file); f >= 0)
            paint(r.files[static_cast<size_t>(f)].oldInfo, ch.oldLoc, static_cast<int>(c), r.changes);
        if (int f = r.fileIndex(Side::New, ch.newLoc.file); f >= 0)
            paint(r.files[static_cast<size_t>(f)].newInfo, ch.newLoc, static_cast<int>(c), r.changes);
    }
    for (size_t c = 0; c < r.changes.size(); ++c) {
        const auto& ch = r.changes[c];
        std::set<int> files;
        if (ch.oldLoc.valid())
            files.insert(r.fileIndex(Side::Old, ch.oldLoc.file));
        if (ch.newLoc.valid())
            files.insert(r.fileIndex(Side::New, ch.newLoc.file));
        for (int f : files)
            if (f >= 0)
                r.files[static_cast<size_t>(f)].changes.push_back(static_cast<int>(c));
    }
    if (progress)
        progress("Done", 1, 1);
    return r;
}

void intralineDiff(const std::string& a, const std::string& b, std::vector<Span>& aSpans, std::vector<Span>& bSpans)
{
    aSpans.clear();
    bSpans.clear();
    auto wa = splitWords(a);
    auto wb = splitWords(b);
    auto edits = diffSequences(wordIds(a, wa, false), wordIds(b, wb, false));
    auto add = [](std::vector<Span>& spans, int begin, int end) {
        if (begin >= end)
            return;
        if (!spans.empty() && spans.back().end == begin)
            spans.back().end = end;
        else
            spans.push_back({begin, end});
    };
    for (const auto& e : edits) {
        if (e.op == EditOp::Delete)
            add(aSpans, wa[static_cast<size_t>(e.aBegin)].begin, wa[static_cast<size_t>(e.aEnd - 1)].end);
        else if (e.op == EditOp::Insert)
            add(bSpans, wb[static_cast<size_t>(e.bBegin)].begin, wb[static_cast<size_t>(e.bEnd - 1)].end);
    }
}

} // namespace cr
