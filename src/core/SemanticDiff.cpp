#include "SemanticDiff.hpp"

#include "Diff.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace cr {

namespace {

enum class Cat { Func, Type, Var };

Cat category(const Entity& e)
{
    switch (e.kind) {
    case EntityKind::Function: return Cat::Func;
    case EntityKind::Class:
    case EntityKind::Enum: return Cat::Type;
    case EntityKind::Variable: return Cat::Var;
    }
    return Cat::Var;
}

std::string displayName(const Entity& e)
{
    return e.qualifiedName + (e.isFunction() ? "()" : "");
}

std::string describe(const Entity& e)
{
    return e.kindName + " " + displayName(e);
}

int percent(double v)
{
    return static_cast<int>(std::lround(v * 100.0));
}

int stmtTokens(const Stmt& s)
{
    return s.tokEnd - s.tokBegin;
}

class Analyzer {
public:
    explicit Analyzer(const SemanticInput& in)
        : in_(in), old_(in.oldEntities), new_(in.newEntities),
          matchOld_(old_.size(), -1), matchNew_(new_.size(), -1), passOld_(old_.size(), 0),
          scoreOld_(old_.size(), 1.0), explainedOld_(old_.size(), false), explainedNew_(new_.size(), false)
    {
    }

    SemanticOutput run()
    {
        matchByIdentity();
        matchBySignatureChange();
        matchByNormalizedBody();
        matchBySimilarity();
        matchClassesByMembers();

        computeReordered();
        for (size_t i = 0; i < old_.size(); ++i)
            if (matchOld_[i] >= 0)
                emitMatch(static_cast<int>(i), matchOld_[i]);

        detectExtractions();
        detectInlining();
        detectMovedStatements();
        emitAddedRemoved();
        detectIdentifierRenames();
        attachMemberScopes();
        return std::move(out_);
    }

private:
    // ---------------------------------------------------------------- helpers

    const Entity& O(int i) const { return *old_[static_cast<size_t>(i)].e; }
    const Entity& N(int j) const { return *new_[static_cast<size_t>(j)].e; }

    std::string mappedFile(const std::string& oldFile) const
    {
        auto it = in_.fileRenames.find(oldFile);
        return it == in_.fileRenames.end() ? oldFile : it->second;
    }

    bool sameFile(int i, int j) const { return mappedFile(O(i).file) == N(j).file; }

    void link(int i, int j, int pass, double score = 1.0)
    {
        matchOld_[static_cast<size_t>(i)] = j;
        matchNew_[static_cast<size_t>(j)] = i;
        passOld_[static_cast<size_t>(i)] = pass;
        scoreOld_[static_cast<size_t>(i)] = score;
    }

    static Location locOf(const Entity& e)
    {
        Location l;
        l.file = e.file;
        l.line = e.beginLine;
        l.endLine = e.endLine;
        l.col = 1;
        return l;
    }

    static Location stmtsLoc(const Entity& e, const std::vector<int>& idx)
    {
        Location l;
        l.file = e.file;
        l.col = 1;
        for (int k : idx) {
            const auto& s = e.stmts[static_cast<size_t>(k)];
            if (l.line == 0 || s.beginLine < l.line)
                l.line = s.beginLine;
            l.endLine = std::max(l.endLine, s.endLine);
        }
        return l;
    }

    const std::vector<uint64_t>& ids(const EntityRef& r, int b, int e, bool normalized,
                                     const std::vector<std::pair<int, int>>* skip = nullptr)
    {
        uint64_t key = hashCombine(hashCombine(reinterpret_cast<uintptr_t>(r.file), static_cast<uint64_t>(b)),
                                   (static_cast<uint64_t>(e) << 1) | (normalized ? 1 : 0));
        auto it = idsCache_.find(key);
        if (it != idsCache_.end())
            return it->second;
        return idsCache_.emplace(key, tokenIds(r.file->tokens, b, e, normalized, skip)).first->second;
    }

    const std::vector<uint64_t>& entityIds(const EntityRef& r, bool normalized)
    {
        return ids(r, r.e->tokBegin, r.e->tokEnd, normalized, &r.e->skipRanges);
    }

    const std::vector<uint64_t>& stmtIds(const EntityRef& r, const Stmt& s)
    {
        return ids(r, s.tokBegin, s.tokEnd, true);
    }

    double entitySimilarity(int i, int j)
    {
        return similarity(entityIds(old_[static_cast<size_t>(i)], true), entityIds(new_[static_cast<size_t>(j)], true));
    }

    // Fraction of an entity's lines that belong to the textual diff.
    double changedLineFraction(Side side, const Entity& e) const
    {
        if (!in_.lineChanged || e.endLine < e.beginLine)
            return 0.0;
        int changed = 0;
        for (int l = e.beginLine; l <= e.endLine; ++l)
            if (in_.lineChanged(side, e.file, l))
                ++changed;
        return static_cast<double>(changed) / (e.endLine - e.beginLine + 1);
    }

    // ---------------------------------------------------------------- matching passes

    static std::string identityKey(const Entity& e)
    {
        return std::to_string(static_cast<int>(category(e))) + "|" + e.qualifiedName + "|" + e.params;
    }

    void matchByIdentity()
    {
        std::map<std::string, std::pair<std::vector<int>, std::vector<int>>> groups;
        for (size_t i = 0; i < old_.size(); ++i)
            groups[identityKey(O(static_cast<int>(i)))].first.push_back(static_cast<int>(i));
        for (size_t j = 0; j < new_.size(); ++j)
            groups[identityKey(N(static_cast<int>(j)))].second.push_back(static_cast<int>(j));

        for (auto& [key, g] : groups) {
            auto& [olds, news] = g;
            if (olds.empty() || news.empty())
                continue;
            if (olds.size() == 1 && news.size() == 1) {
                link(olds[0], news[0], 1);
                continue;
            }
            // Several entities share the key (template specializations, constrained overloads,
            // static functions in different files): identical ones first, then by file and order.
            for (int i : olds) {
                int best = -1;
                for (int j : news) {
                    if (matchNew_[static_cast<size_t>(j)] >= 0 || N(j).exactHash != O(i).exactHash)
                        continue;
                    if (best < 0 || (sameFile(i, j) && !sameFile(i, best)))
                        best = j;
                }
                if (best >= 0)
                    link(i, best, 1);
            }
            for (int i : olds) {
                if (matchOld_[static_cast<size_t>(i)] >= 0)
                    continue;
                int best = -1;
                for (int j : news) {
                    if (matchNew_[static_cast<size_t>(j)] >= 0)
                        continue;
                    if (sameFile(i, j)) {
                        best = j;
                        break;
                    }
                    if (best < 0)
                        best = j;
                }
                if (best >= 0)
                    link(i, best, 1);
            }
        }
    }

    void matchBySignatureChange()
    {
        std::map<std::string, std::vector<int>> oldByName, newByName;
        for (size_t i = 0; i < old_.size(); ++i)
            if (matchOld_[i] < 0 && O(static_cast<int>(i)).isFunction())
                oldByName[O(static_cast<int>(i)).qualifiedName].push_back(static_cast<int>(i));
        for (size_t j = 0; j < new_.size(); ++j)
            if (matchNew_[j] < 0 && N(static_cast<int>(j)).isFunction())
                newByName[N(static_cast<int>(j)).qualifiedName].push_back(static_cast<int>(j));

        for (auto& [name, olds] : oldByName) {
            auto it = newByName.find(name);
            if (it == newByName.end())
                continue;
            auto& news = it->second;
            if (olds.size() == 1 && news.size() == 1) {
                link(olds[0], news[0], 2, entitySimilarity(olds[0], news[0]));
                continue;
            }
            // Several overloads changed: pair them by body similarity.
            std::vector<std::tuple<double, int, int>> pairs;
            for (int i : olds)
                for (int j : news)
                    pairs.emplace_back(entitySimilarity(i, j), i, j);
            std::sort(pairs.begin(), pairs.end(), [](auto& a, auto& b) { return std::get<0>(a) > std::get<0>(b); });
            for (auto& [s, i, j] : pairs)
                if (s >= 0.3 && matchOld_[static_cast<size_t>(i)] < 0 && matchNew_[static_cast<size_t>(j)] < 0)
                    link(i, j, 2, s);
        }
    }

    void matchByNormalizedBody()
    {
        std::unordered_multimap<uint64_t, int> byHash;
        for (size_t j = 0; j < new_.size(); ++j)
            if (matchNew_[j] < 0)
                byHash.emplace(N(static_cast<int>(j)).normHash, static_cast<int>(j));
        for (size_t i = 0; i < old_.size(); ++i) {
            if (matchOld_[i] >= 0)
                continue;
            const auto& o = O(static_cast<int>(i));
            if (o.tokenCount() < 6)
                continue;
            auto [b, e] = byHash.equal_range(o.normHash);
            int best = -1;
            int bestRank = -1;
            for (auto it = b; it != e; ++it) {
                int j = it->second;
                const auto& n = N(j);
                if (matchNew_[static_cast<size_t>(j)] >= 0 || category(n) != category(o))
                    continue;
                int rank = (n.name == o.name ? 2 : 0) + (sameFile(static_cast<int>(i), j) ? 1 : 0);
                if (rank > bestRank) {
                    bestRank = rank;
                    best = j;
                }
            }
            if (best >= 0)
                link(static_cast<int>(i), best, 3);
        }
    }

    void matchBySimilarity()
    {
        std::vector<int> olds, news;
        for (size_t i = 0; i < old_.size(); ++i)
            if (matchOld_[i] < 0 && O(static_cast<int>(i)).tokenCount() >= 12)
                olds.push_back(static_cast<int>(i));
        for (size_t j = 0; j < new_.size(); ++j)
            if (matchNew_[j] < 0 && N(static_cast<int>(j)).tokenCount() >= 12)
                news.push_back(static_cast<int>(j));
        if (olds.empty() || news.empty() || olds.size() * news.size() > 40000)
            return;

        std::vector<std::tuple<double, int, int>> pairs;
        for (int i : olds) {
            const auto& o = O(i);
            for (int j : news) {
                const auto& n = N(j);
                if (category(o) != category(n))
                    continue;
                double a = o.tokenCount(), b = n.tokenCount();
                if (std::min(a, b) / std::max(a, b) < 0.6)
                    continue;
                double s = entitySimilarity(i, j);
                if (s >= 0.72)
                    pairs.emplace_back(s, i, j);
            }
        }
        std::sort(pairs.begin(), pairs.end(), [](auto& a, auto& b) { return std::get<0>(a) > std::get<0>(b); });
        for (auto& [s, i, j] : pairs)
            if (matchOld_[static_cast<size_t>(i)] < 0 && matchNew_[static_cast<size_t>(j)] < 0)
                link(i, j, 4, s);
    }

    // Unmatched classes whose members were matched to members of one other class correspond to
    // each other (typically a renamed and/or moved class whose body also changed).
    void matchClassesByMembers()
    {
        std::map<std::string, std::vector<int>> oldClasses, newClasses;
        for (size_t i = 0; i < old_.size(); ++i)
            if (matchOld_[i] < 0 && O(static_cast<int>(i)).kind == EntityKind::Class)
                oldClasses[O(static_cast<int>(i)).qualifiedName].push_back(static_cast<int>(i));
        for (size_t j = 0; j < new_.size(); ++j)
            if (matchNew_[j] < 0 && N(static_cast<int>(j)).kind == EntityKind::Class)
                newClasses[N(static_cast<int>(j)).qualifiedName].push_back(static_cast<int>(j));
        if (oldClasses.empty() || newClasses.empty())
            return;

        // votes[old class][new class] = number of member pairs linking them
        std::map<std::string, std::map<std::string, int>> votes;
        std::map<std::string, int> members;
        for (size_t i = 0; i < old_.size(); ++i) {
            const auto& o = O(static_cast<int>(i));
            if (!oldClasses.count(o.scope))
                continue;
            ++members[o.scope];
            if (int j = matchOld_[i]; j >= 0 && newClasses.count(N(j).scope))
                ++votes[o.scope][N(j).scope];
        }
        for (auto& [from, targets] : votes) {
            auto best = std::max_element(targets.begin(), targets.end(),
                                         [](auto& a, auto& b) { return a.second < b.second; });
            if (best->second * 2 < members[from])
                continue;
            auto& os = oldClasses[from];
            auto& ns = newClasses[best->first];
            if (os.size() == 1 && ns.size() == 1 && matchOld_[static_cast<size_t>(os[0])] < 0 &&
                matchNew_[static_cast<size_t>(ns[0])] < 0)
                link(os[0], ns[0], 5, entitySimilarity(os[0], ns[0]));
        }
    }

    // ---------------------------------------------------------------- classification

    // Entities that stayed in their file but changed position relative to the others: the
    // complement of the heaviest (by size) subsequence whose order is preserved.
    void computeReordered()
    {
        reordered_.assign(old_.size(), false);
        std::map<std::string, std::vector<int>> byFile;
        for (size_t i = 0; i < old_.size(); ++i)
            if (matchOld_[i] >= 0 && sameFile(static_cast<int>(i), matchOld_[i]))
                byFile[O(static_cast<int>(i)).file].push_back(static_cast<int>(i));
        for (auto& [file, items] : byFile) {
            if (items.size() < 2 || items.size() > 3000)
                continue;
            std::sort(items.begin(), items.end(), [&](int a, int b) { return O(a).tokBegin < O(b).tokBegin; });
            const size_t n = items.size();
            std::vector<double> best(n);
            std::vector<int> prev(n, -1);
            for (size_t a = 0; a < n; ++a) {
                const auto& na = N(matchOld_[static_cast<size_t>(items[a])]);
                best[a] = O(items[a]).tokenCount();
                for (size_t b = 0; b < a; ++b) {
                    const auto& nb = N(matchOld_[static_cast<size_t>(items[b])]);
                    if (nb.tokBegin < na.tokBegin && best[b] + O(items[a]).tokenCount() > best[a]) {
                        best[a] = best[b] + O(items[a]).tokenCount();
                        prev[a] = static_cast<int>(b);
                    }
                }
            }
            int k = static_cast<int>(std::max_element(best.begin(), best.end()) - best.begin());
            std::vector<bool> inPlace(n, false);
            for (; k >= 0; k = prev[static_cast<size_t>(k)])
                inPlace[static_cast<size_t>(k)] = true;
            for (size_t a = 0; a < n; ++a)
                if (!inPlace[a])
                    reordered_[static_cast<size_t>(items[a])] = true;
        }
    }

    void emitMatch(int i, int j)
    {
        const auto& o = O(i);
        const auto& n = N(j);
        const int pass = passOld_[static_cast<size_t>(i)];
        const bool fileMoved = !sameFile(i, j);
        const bool scopeMoved = o.scope != n.scope;
        const bool renamed = o.name != n.name;
        const bool identical = o.exactHash == n.exactHash;
        const bool renameOnly = o.normHash == n.normHash;

        SemanticChange c;
        c.oldLoc = locOf(o);
        c.newLoc = locOf(n);
        c.oldName = o.qualifiedName;
        c.newName = n.qualifiedName;
        c.isFunction = o.isFunction();
        c.similarity = scoreOld_[static_cast<size_t>(i)];

        std::string where;
        if (fileMoved && scopeMoved && !renamed)
            where = " to " + displayName(n) + " (" + o.file + " → " + n.file + ")";
        else if (fileMoved)
            where = " from " + o.file + " to " + n.file;
        else if (scopeMoved)
            where = " from " + (o.scope.empty() ? std::string("global scope") : o.scope) + " to " +
                    (n.scope.empty() ? std::string("global scope") : n.scope);

        if (pass == 2) {
            c.kind = ChangeKind::SignatureChanged;
            c.title = "Changed signature of " + describe(o) + ": " + o.params + " → " + n.params;
            if (fileMoved)
                c.title += " (moved to " + n.file + ")";
        } else if (renamed && (fileMoved || scopeMoved)) {
            c.kind = ChangeKind::Moved;
            c.title = "Moved and renamed " + describe(o) + " → " + displayName(n) + where;
        } else if (renamed) {
            c.kind = ChangeKind::Renamed;
            c.title = "Renamed " + describe(o) + " → " + n.name;
        } else if (fileMoved || scopeMoved) {
            c.kind = ChangeKind::Moved;
            c.title = "Moved " + describe(o) + where;
        } else if (!identical && reordered_[static_cast<size_t>(i)] && renameOnly) {
            c.kind = ChangeKind::Reordered;
            c.title = "Moved " + describe(o) + " within " + n.file + " (line " + std::to_string(o.beginLine) + " → " +
                      std::to_string(n.beginLine) + "; identifiers renamed)";
        } else if (!identical) {
            c.kind = ChangeKind::Modified;
            c.title = "Modified " + describe(n);
            if (reordered_[static_cast<size_t>(i)])
                c.title += " and moved within the file";
            if (renameOnly) {
                c.title += " — identifier renames only";
                c.trivial = true;
            }
        } else {
            // Same entity, same file, same tokens: only interesting if it changed position.
            if (reordered_[static_cast<size_t>(i)] && changedLineFraction(Side::Old, o) >= 0.5 &&
                changedLineFraction(Side::New, n) >= 0.5) {
                c.kind = ChangeKind::Reordered;
                c.title = "Moved " + describe(o) + " within " + n.file + " (line " + std::to_string(o.beginLine) +
                          " → " + std::to_string(n.beginLine) + ")";
                out_.changes.push_back(std::move(c));
            }
            return;
        }

        if (c.kind != ChangeKind::Modified && c.kind != ChangeKind::SignatureChanged && !identical) {
            if (renameOnly && c.kind != ChangeKind::Renamed && c.kind != ChangeKind::Reordered)
                c.title += " (identifiers renamed)";
            else if (!renameOnly) {
                double s = pass >= 4 ? c.similarity : entitySimilarity(i, j);
                c.similarity = s;
                c.title += " (modified, " + std::to_string(percent(s)) + "% similar)";
            }
        }
        if (renamed)
            entityRenames_.emplace_back(o.name, n.name);
        out_.changes.push_back(std::move(c));
    }

    // ---------------------------------------------------------------- extraction / inlining

    struct Coverage {
        double score = 0.0;
        std::vector<int> dstStmts;
    };

    // How much of `src`'s body (its top-level statements) can be found among `dst`'s statements.
    Coverage coverage(const EntityRef& src, const EntityRef& dst)
    {
        Coverage cov;
        const auto& s = *src.e;
        const auto& d = *dst.e;
        if (s.stmts.empty() || d.stmts.empty())
            return cov;
        std::vector<int> top;
        int total = 0;
        for (size_t k = 0; k < s.stmts.size(); ++k)
            if (s.stmts[k].parent < 0) {
                top.push_back(static_cast<int>(k));
                total += stmtTokens(s.stmts[k]);
            }
        if (total == 0)
            return cov;
        const bool fuzzy = top.size() * d.stmts.size() <= 6000;

        double score = 0.0;
        std::set<int> used;
        for (int k : top) {
            const auto& ss = s.stmts[static_cast<size_t>(k)];
            double best = 0.0;
            int bestIdx = -1;
            for (size_t m = 0; m < d.stmts.size(); ++m) {
                if (used.count(static_cast<int>(m)))
                    continue;
                const auto& ds = d.stmts[m];
                double sim = 0.0;
                if (ds.normHash == ss.normHash) {
                    sim = 1.0;
                } else if (fuzzy) {
                    double a = stmtTokens(ss), b = stmtTokens(ds);
                    if (std::min(a, b) / std::max(a, b) < 0.5)
                        continue;
                    sim = similarity(stmtIds(src, ss), stmtIds(dst, ds));
                }
                if (sim > best) {
                    best = sim;
                    bestIdx = static_cast<int>(m);
                    if (sim == 1.0)
                        break;
                }
            }
            if (best >= 0.6 && bestIdx >= 0) {
                score += best * stmtTokens(ss);
                used.insert(bestIdx);
                cov.dstStmts.push_back(bestIdx);
            }
        }
        cov.score = score / total;
        std::sort(cov.dstStmts.begin(), cov.dstStmts.end());
        return cov;
    }

    // Fraction of the given statements of `e` whose normalized hash no longer occurs in `other`.
    static double goneFraction(const Entity& e, const std::vector<int>& stmts, const Entity* other)
    {
        if (stmts.empty())
            return 0.0;
        if (!other)
            return 1.0;
        std::unordered_multiset<uint64_t> present;
        for (const auto& s : other->stmts)
            present.insert(s.normHash);
        int gone = 0;
        for (int k : stmts)
            if (!present.count(e.stmts[static_cast<size_t>(k)].normHash))
                ++gone;
        return static_cast<double>(gone) / stmts.size();
    }

    void markConsumed(std::set<std::pair<const Entity*, int>>& set, const Entity& e, const std::vector<int>& stmts)
    {
        for (int k : stmts) {
            set.emplace(&e, k);
            // Nested statements of a consumed statement are consumed as well.
            for (size_t m = 0; m < e.stmts.size(); ++m) {
                int p = e.stmts[m].parent;
                while (p >= 0 && p != k)
                    p = e.stmts[static_cast<size_t>(p)].parent;
                if (p == k)
                    set.emplace(&e, static_cast<int>(m));
            }
        }
    }

    void detectExtractions()
    {
        int oldFunctions = 0;
        for (size_t i = 0; i < old_.size(); ++i)
            if (O(static_cast<int>(i)).isFunction())
                ++oldFunctions;
        const bool searchUncalled = oldFunctions <= 400;

        for (size_t j = 0; j < new_.size(); ++j) {
            if (matchNew_[j] >= 0)
                continue;
            const auto& f = N(static_cast<int>(j));
            if (!f.isFunction() || f.stmts.empty() || f.bodyTokEnd - f.bodyTokBegin < 8)
                continue;

            double bestRank = 0.0;
            int bestI = -1;
            Coverage bestCov;
            bool bestCalled = false;
            for (size_t i = 0; i < old_.size(); ++i) {
                const auto& g = O(static_cast<int>(i));
                if (!g.isFunction() || g.stmts.empty())
                    continue;
                int gp = matchOld_[i];
                const bool called = gp >= 0 && N(gp).calls.count(f.name) && !g.calls.count(f.name);
                if (!called && !searchUncalled)
                    continue;
                auto cov = coverage(new_[j], old_[i]);
                const double threshold = called ? 0.5 : 0.75;
                if (cov.score < threshold)
                    continue;
                double rank = cov.score + (called ? 0.25 : 0.0);
                if (rank > bestRank) {
                    bestRank = rank;
                    bestI = static_cast<int>(i);
                    bestCov = std::move(cov);
                    bestCalled = called;
                }
            }
            if (bestI < 0)
                continue;

            const auto& g = O(bestI);
            const int gp = matchOld_[static_cast<size_t>(bestI)];
            const double gone = goneFraction(g, bestCov.dstStmts, gp >= 0 ? &N(gp) : nullptr);

            SemanticChange c;
            c.oldLoc = stmtsLoc(g, bestCov.dstStmts);
            c.newLoc = locOf(f);
            c.oldName = g.qualifiedName;
            c.newName = f.qualifiedName;
            c.similarity = bestCov.score;
            if (gone >= 0.5) {
                c.kind = ChangeKind::Extracted;
                c.title = "Extracted " + describe(f) + " from " + displayName(g);
                if (bestCov.score < 0.95)
                    c.title += " (" + std::to_string(percent(bestCov.score)) + "% of body matches)";
            } else {
                c.kind = ChangeKind::CopiedCode;
                c.title = "New " + describe(f) + " duplicates code from " + displayName(g);
            }
            c.isFunction = true;
            c.detail = bestCalled ? displayName(g) + " now calls " + f.name + "()." : std::string{};
            out_.changes.push_back(std::move(c));
            explainedNew_[j] = true;
            if (gone >= 0.5)
                markConsumed(consumedOld_, g, bestCov.dstStmts);
            std::vector<int> all(f.stmts.size());
            for (size_t k = 0; k < all.size(); ++k)
                all[k] = static_cast<int>(k);
            markConsumed(consumedNew_, f, all);
        }
    }

    void detectInlining()
    {
        for (size_t i = 0; i < old_.size(); ++i) {
            if (matchOld_[i] >= 0)
                continue;
            const auto& f = O(static_cast<int>(i));
            if (!f.isFunction() || f.stmts.empty() || f.bodyTokEnd - f.bodyTokBegin < 8)
                continue;
            double best = 0.0;
            int bestJ = -1;
            Coverage bestCov;
            for (size_t j = 0; j < new_.size(); ++j) {
                int h = matchNew_[j];
                if (h < 0)
                    continue;
                const auto& caller = N(static_cast<int>(j));
                if (!caller.isFunction() || !O(h).calls.count(f.name) || caller.calls.count(f.name))
                    continue;
                auto cov = coverage(old_[i], new_[j]);
                if (cov.score >= 0.5 && cov.score > best) {
                    best = cov.score;
                    bestJ = static_cast<int>(j);
                    bestCov = std::move(cov);
                }
            }
            if (bestJ < 0)
                continue;
            const auto& h = N(bestJ);
            SemanticChange c;
            c.kind = ChangeKind::Inlined;
            c.isFunction = true;
            c.title = "Inlined " + describe(f) + " into " + displayName(h);
            c.oldLoc = locOf(f);
            c.newLoc = stmtsLoc(h, bestCov.dstStmts);
            c.oldName = f.qualifiedName;
            c.newName = h.qualifiedName;
            c.similarity = bestCov.score;
            out_.changes.push_back(std::move(c));
            explainedOld_[i] = true;
            markConsumed(consumedNew_, h, bestCov.dstStmts);
        }
    }

    // ---------------------------------------------------------------- moved statements

    struct StmtRef {
        int entity;
        int stmt;
    };

    // Outermost statements of an entity whose normalized hash is missing from its counterpart.
    std::vector<int> vanishedStmts(const Entity& e, const Entity* counterpart,
                                   const std::set<std::pair<const Entity*, int>>& consumed) const
    {
        std::unordered_multiset<uint64_t> present;
        if (counterpart)
            for (const auto& s : counterpart->stmts)
                present.insert(s.normHash);
        std::vector<bool> gone(e.stmts.size(), false);
        std::vector<int> out;
        for (size_t k = 0; k < e.stmts.size(); ++k) {
            const auto& s = e.stmts[k];
            if (present.count(s.normHash) || consumed.count({&e, static_cast<int>(k)}))
                continue;
            gone[k] = true;
            if (s.parent >= 0 && gone[static_cast<size_t>(s.parent)])
                continue; // reported via its parent
            if (stmtTokens(s) >= 8)
                out.push_back(static_cast<int>(k));
        }
        return out;
    }

    void detectMovedStatements()
    {
        std::unordered_multimap<uint64_t, StmtRef> added;
        for (size_t j = 0; j < new_.size(); ++j) {
            const auto& n = N(static_cast<int>(j));
            if (!n.isFunction())
                continue;
            int h = matchNew_[j];
            for (int k : vanishedStmts(n, h >= 0 ? &O(h) : nullptr, consumedNew_))
                added.emplace(n.stmts[static_cast<size_t>(k)].normHash, StmtRef{static_cast<int>(j), k});
        }

        struct Pair {
            StmtRef from, to;
        };
        std::vector<Pair> pairs;
        std::set<std::pair<int, int>> usedNew;
        for (size_t i = 0; i < old_.size(); ++i) {
            const auto& o = O(static_cast<int>(i));
            if (!o.isFunction())
                continue;
            int gp = matchOld_[i];
            for (int k : vanishedStmts(o, gp >= 0 ? &N(gp) : nullptr, consumedOld_)) {
                auto [b, e] = added.equal_range(o.stmts[static_cast<size_t>(k)].normHash);
                for (auto it = b; it != e; ++it) {
                    const auto& to = it->second;
                    if (to.entity == gp || usedNew.count({to.entity, to.stmt}))
                        continue; // moves inside one function are visible in the text diff
                    usedNew.emplace(to.entity, to.stmt);
                    pairs.push_back({{static_cast<int>(i), k}, to});
                    break;
                }
            }
        }

        // Merge runs of consecutive statements moved together.
        for (size_t p = 0; p < pairs.size();) {
            size_t q = p + 1;
            while (q < pairs.size()) {
                const auto& prev = pairs[q - 1];
                const auto& cur = pairs[q];
                const auto& ps = O(prev.from.entity).stmts[static_cast<size_t>(prev.from.stmt)];
                const auto& cs = O(cur.from.entity).stmts[static_cast<size_t>(cur.from.stmt)];
                const auto& pt = N(prev.to.entity).stmts[static_cast<size_t>(prev.to.stmt)];
                const auto& ct = N(cur.to.entity).stmts[static_cast<size_t>(cur.to.stmt)];
                if (cur.from.entity != prev.from.entity || cur.to.entity != prev.to.entity || cs.block != ps.block ||
                    ct.block != pt.block || cs.indexInBlock != ps.indexInBlock + 1 ||
                    ct.indexInBlock != pt.indexInBlock + 1)
                    break;
                ++q;
            }
            const auto& from = O(pairs[p].from.entity);
            const auto& to = N(pairs[p].to.entity);
            std::vector<int> fromStmts, toStmts;
            for (size_t r = p; r < q; ++r) {
                fromStmts.push_back(pairs[r].from.stmt);
                toStmts.push_back(pairs[r].to.stmt);
            }
            SemanticChange c;
            c.kind = ChangeKind::MovedCode;
            const auto count = q - p;
            c.title = "Moved " + std::to_string(count) + (count == 1 ? " statement" : " statements") + " from " +
                      displayName(from) + " to " + displayName(to);
            c.oldLoc = stmtsLoc(from, fromStmts);
            c.newLoc = stmtsLoc(to, toStmts);
            c.oldName = from.qualifiedName;
            c.newName = to.qualifiedName;
            c.similarity = 1.0;
            out_.changes.push_back(std::move(c));
            p = q;
        }
    }

    // ---------------------------------------------------------------- added / removed

    void emitAddedRemoved()
    {
        auto nestedIn = [](const std::vector<EntityRef>& side, const std::vector<int>& match,
                           const std::vector<bool>& explained, size_t idx) {
            const auto& e = *side[idx].e;
            for (size_t k = 0; k < side.size(); ++k) {
                if (k == idx || match[k] >= 0 || explained[k])
                    continue;
                const auto& outer = *side[k].e;
                if (outer.file == e.file && outer.tokBegin <= e.tokBegin && outer.tokEnd >= e.tokEnd &&
                    outer.tokenCount() > e.tokenCount())
                    return true;
            }
            return false;
        };
        for (size_t j = 0; j < new_.size(); ++j) {
            if (matchNew_[j] >= 0 || explainedNew_[j] || nestedIn(new_, matchNew_, explainedNew_, j))
                continue;
            const auto& n = N(static_cast<int>(j));
            SemanticChange c;
            c.kind = ChangeKind::Added;
            c.isFunction = n.isFunction();
            c.title = "Added " + describe(n);
            c.newLoc = locOf(n);
            c.newName = n.qualifiedName;
            out_.changes.push_back(std::move(c));
        }
        for (size_t i = 0; i < old_.size(); ++i) {
            if (matchOld_[i] >= 0 || explainedOld_[i] || nestedIn(old_, matchOld_, explainedOld_, i))
                continue;
            const auto& o = O(static_cast<int>(i));
            SemanticChange c;
            c.kind = ChangeKind::Removed;
            c.isFunction = o.isFunction();
            c.title = "Removed " + describe(o);
            c.oldLoc = locOf(o);
            c.oldName = o.qualifiedName;
            out_.changes.push_back(std::move(c));
        }
    }

    // ---------------------------------------------------------------- member scopes

    // For changes about class members, record where the class and its members live, so that
    // usage search can ignore unrelated functions that happen to share the name.
    void attachMemberScopes()
    {
        auto fill = [](const std::vector<EntityRef>& side, const std::string& name, const Location& loc,
                       std::string& cls, std::vector<Location>& ranges) {
            if (!loc.valid())
                return;
            const Entity* ent = nullptr;
            for (const auto& r : side)
                if (r.e->qualifiedName == name && r.e->file == loc.file && r.e->beginLine == loc.line) {
                    ent = r.e;
                    break;
                }
            if (!ent || !ent->isMember)
                return;
            auto p = ent->scope.rfind("::");
            cls = p == std::string::npos ? ent->scope : ent->scope.substr(p + 2);
            for (const auto& r : side)
                if ((r.e->kind == EntityKind::Class && r.e->qualifiedName == ent->scope) || r.e->scope == ent->scope)
                    ranges.push_back(locOf(*r.e));
        };
        for (auto& c : out_.changes) {
            fill(old_, c.oldName, c.oldLoc, c.memberClassOld, c.memberScopeOld);
            fill(new_, c.newName, c.newLoc, c.memberClassNew, c.memberScopeNew);
        }
    }

    // ---------------------------------------------------------------- identifier renames

    struct RenameStats {
        int occurrences = 0;
        std::vector<std::string> places;
        Location firstOld, firstNew;
    };

    // Token ranges of the statements of `e` already explained by an extraction or inlining.
    static std::vector<std::pair<int, int>> consumedRanges(const Entity& e,
                                                           const std::set<std::pair<const Entity*, int>>& consumed)
    {
        std::vector<std::pair<int, int>> out;
        for (size_t k = 0; k < e.stmts.size(); ++k)
            if (consumed.count({&e, static_cast<int>(k)}))
                out.emplace_back(e.stmts[k].tokBegin, e.stmts[k].tokEnd);
        return out;
    }

    static bool inRanges(const std::vector<std::pair<int, int>>& ranges, int tok)
    {
        return std::any_of(ranges.begin(), ranges.end(), [&](auto r) { return tok >= r.first && tok < r.second; });
    }

    void detectIdentifierRenames()
    {
        std::map<std::pair<std::string, std::string>, RenameStats> accepted;

        for (size_t i = 0; i < old_.size(); ++i) {
            int j = matchOld_[i];
            if (j < 0)
                continue;
            const auto& o = O(static_cast<int>(i));
            const auto& n = N(j);
            if (o.exactHash == n.exactHash)
                continue;
            const auto& ot = old_[i].file->tokens;
            const auto& nt = new_[static_cast<size_t>(j)].file->tokens;

            // Token positions (skipping comments), aligned with the id sequences.
            auto positions = [](const std::vector<Token>& toks, const Entity& e) {
                std::vector<int> pos;
                std::vector<bool> skip(static_cast<size_t>(std::max(0, e.tokEnd - e.tokBegin)), false);
                for (auto [b, en] : e.skipRanges)
                    for (int t = std::max(b, e.tokBegin); t < std::min(en, e.tokEnd); ++t)
                        skip[static_cast<size_t>(t - e.tokBegin)] = true;
                for (int t = e.tokBegin; t < e.tokEnd; ++t)
                    if (toks[static_cast<size_t>(t)].kind != TokKind::Comment && !skip[static_cast<size_t>(t - e.tokBegin)])
                        pos.push_back(t);
                return pos;
            };
            auto opos = positions(ot, o);
            auto npos = positions(nt, n);
            const auto& oids = entityIds(old_[i], false);
            const auto& nids = entityIds(new_[static_cast<size_t>(j)], false);
            if (oids.size() != opos.size() || nids.size() != npos.size())
                continue;

            // Code that moved into (or came from) another function is aligned against unrelated
            // code here, e.g. the call replacing an extracted body; it says nothing about renames.
            const auto movedOld = consumedRanges(o, consumedOld_);
            const auto movedNew = consumedRanges(n, consumedNew_);

            std::map<std::string, std::map<std::string, int>> votes;
            std::map<std::string, int> kept;
            std::map<std::string, std::pair<Location, Location>> firstSeen;
            auto edits = diffSequences(oids, nids);
            for (size_t k = 0; k < edits.size(); ++k) {
                const auto& ed = edits[k];
                if (ed.op == EditOp::Equal) {
                    for (int a = ed.aBegin; a < ed.aEnd; ++a) {
                        const auto& t = ot[static_cast<size_t>(opos[static_cast<size_t>(a)])];
                        if (t.kind == TokKind::Ident)
                            ++kept[t.text];
                    }
                    continue;
                }
                if (k + 1 >= edits.size())
                    continue;
                const auto& nx = edits[k + 1];
                const Edit* del = ed.op == EditOp::Delete ? &ed : (nx.op == EditOp::Delete ? &nx : nullptr);
                const Edit* ins = ed.op == EditOp::Insert ? &ed : (nx.op == EditOp::Insert ? &nx : nullptr);
                if (!del || !ins || del == ins)
                    continue;
                ++k;
                if (del->aEnd - del->aBegin != ins->bEnd - ins->bBegin)
                    continue;
                for (int d = 0; d < del->aEnd - del->aBegin; ++d) {
                    const int ta = opos[static_cast<size_t>(del->aBegin + d)];
                    const int tb = npos[static_cast<size_t>(ins->bBegin + d)];
                    if (inRanges(movedOld, ta) || inRanges(movedNew, tb))
                        continue;
                    const auto& a = ot[static_cast<size_t>(ta)];
                    const auto& b = nt[static_cast<size_t>(tb)];
                    if (a.kind == TokKind::Ident && b.kind == TokKind::Ident && a.text != b.text) {
                        ++votes[a.text][b.text];
                        if (!firstSeen.count(a.text)) {
                            Location lo{o.file, a.line, a.col, 0, false};
                            Location ln{n.file, b.line, b.col, 0, false};
                            firstSeen[a.text] = {lo, ln};
                        }
                    }
                }
            }
            for (auto& [from, targets] : votes) {
                if (targets.size() != 1 || kept[from] > 0)
                    continue;
                const auto& [to, count] = *targets.begin();
                if (kept.count(to) && kept[to] > 0 && from.size() > 0) {
                    // `to` already existed unchanged here: more likely a different symbol was used.
                    continue;
                }
                auto& st = accepted[{from, to}];
                if (st.occurrences == 0) {
                    st.firstOld = firstSeen[from].first;
                    st.firstNew = firstSeen[from].second;
                }
                st.occurrences += count;
                st.places.push_back(displayName(n));
            }
        }

        std::set<std::pair<std::string, std::string>> entityPairs(entityRenames_.begin(), entityRenames_.end());
        std::map<std::string, std::set<std::string>> targets;
        for (auto& [pair, st] : accepted)
            targets[pair.first].insert(pair.second);
        for (auto& [from, to] : entityRenames_)
            targets[from].insert(to);
        for (auto& [from, tos] : targets)
            if (tos.size() == 1)
                out_.renames[from] = *tos.begin();

        for (auto& [pair, st] : accepted) {
            if (entityPairs.count(pair))
                continue; // already reported as a renamed entity
            SemanticChange c;
            c.kind = ChangeKind::SymbolRenamed;
            c.oldName = pair.first;
            c.newName = pair.second;
            c.title = "Renamed identifier " + pair.first + " → " + pair.second + " (" + std::to_string(st.occurrences) +
                      (st.occurrences == 1 ? " occurrence" : " occurrences");
            std::set<std::string> uniquePlaces(st.places.begin(), st.places.end());
            if (uniquePlaces.size() == 1)
                c.title += " in " + *uniquePlaces.begin();
            else
                c.title += " in " + std::to_string(uniquePlaces.size()) + " entities";
            c.title += ")";
            for (auto& p : uniquePlaces)
                c.detail += (c.detail.empty() ? "" : ", ") + p;
            c.oldLoc = st.firstOld;
            c.newLoc = st.firstNew;
            c.similarity = 1.0;
            out_.changes.push_back(std::move(c));
        }
    }

    const SemanticInput& in_;
    const std::vector<EntityRef>& old_;
    const std::vector<EntityRef>& new_;
    std::vector<int> matchOld_, matchNew_, passOld_;
    std::vector<double> scoreOld_;
    std::vector<bool> explainedOld_, explainedNew_;
    std::vector<bool> reordered_;
    std::set<std::pair<const Entity*, int>> consumedOld_, consumedNew_;
    std::vector<std::pair<std::string, std::string>> entityRenames_;
    std::unordered_map<uint64_t, std::vector<uint64_t>> idsCache_;
    SemanticOutput out_;
};

} // namespace

SemanticOutput analyzeEntities(const SemanticInput& input)
{
    return Analyzer(input).run();
}

} // namespace cr
