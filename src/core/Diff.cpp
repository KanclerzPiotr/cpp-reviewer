#include "Diff.hpp"

#include <algorithm>
#include <climits>

namespace cr {

namespace {

// Implementation follows the classic GNU diff `compareseq`/`diag` structure,
// without the "too expensive" heuristics.
class Myers {
public:
    Myers(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b)
        : a_(a), b_(b), delA_(a.size(), false), insB_(b.size(), false)
    {
        const size_t n = a.size() + b.size() + 3;
        fd_.assign(n, 0);
        bd_.assign(n, 0);
        off_ = static_cast<int>(b.size()) + 1;
    }

    void run() { compare(0, static_cast<int>(a_.size()), 0, static_cast<int>(b_.size())); }

    std::vector<Edit> edits() const
    {
        std::vector<Edit> out;
        const int n = static_cast<int>(a_.size());
        const int m = static_cast<int>(b_.size());
        int i = 0, j = 0;
        auto push = [&](EditOp op, int a0, int a1, int b0, int b1) {
            if (!out.empty() && out.back().op == op && out.back().aEnd == a0 && out.back().bEnd == b0) {
                out.back().aEnd = a1;
                out.back().bEnd = b1;
            } else {
                out.push_back({op, a0, a1, b0, b1});
            }
        };
        while (i < n || j < m) {
            if (i < n && delA_[i]) {
                push(EditOp::Delete, i, i + 1, j, j);
                ++i;
            } else if (j < m && insB_[j]) {
                push(EditOp::Insert, i, i, j, j + 1);
                ++j;
            } else {
                push(EditOp::Equal, i, i + 1, j, j + 1);
                ++i;
                ++j;
            }
        }
        return out;
    }

private:
    int& fd(int d) { return fd_[static_cast<size_t>(d + off_)]; }
    int& bd(int d) { return bd_[static_cast<size_t>(d + off_)]; }

    void compare(int xoff, int xlim, int yoff, int ylim)
    {
        while (xoff < xlim && yoff < ylim && a_[xoff] == b_[yoff]) {
            ++xoff;
            ++yoff;
        }
        while (xlim > xoff && ylim > yoff && a_[xlim - 1] == b_[ylim - 1]) {
            --xlim;
            --ylim;
        }
        if (xoff == xlim) {
            for (int y = yoff; y < ylim; ++y)
                insB_[y] = true;
            return;
        }
        if (yoff == ylim) {
            for (int x = xoff; x < xlim; ++x)
                delA_[x] = true;
            return;
        }
        int xmid = 0, ymid = 0;
        middleSnake(xoff, xlim, yoff, ylim, xmid, ymid);
        compare(xoff, xmid, yoff, ymid);
        compare(xmid, xlim, ymid, ylim);
    }

    void middleSnake(int xoff, int xlim, int yoff, int ylim, int& xmid, int& ymid)
    {
        const int dmin = xoff - ylim;
        const int dmax = xlim - yoff;
        const int fmid = xoff - yoff;
        const int bmid = xlim - ylim;
        int fmin = fmid, fmax = fmid;
        int bmin = bmid, bmax = bmid;
        const bool odd = ((fmid - bmid) & 1) != 0;

        fd(fmid) = xoff;
        bd(bmid) = xlim;

        for (;;) {
            // Extend the forward search by one edit step.
            if (fmin > dmin)
                fd(--fmin - 1) = -1;
            else
                ++fmin;
            if (fmax < dmax)
                fd(++fmax + 1) = -1;
            else
                --fmax;
            for (int d = fmax; d >= fmin; d -= 2) {
                int tlo = fd(d - 1), thi = fd(d + 1);
                int x = tlo >= thi ? tlo + 1 : thi;
                int y = x - d;
                while (x < xlim && y < ylim && a_[x] == b_[y]) {
                    ++x;
                    ++y;
                }
                fd(d) = x;
                if (odd && bmin <= d && d <= bmax && bd(d) <= x) {
                    xmid = x;
                    ymid = y;
                    return;
                }
            }

            // Extend the backward search by one edit step.
            if (bmin > dmin)
                bd(--bmin - 1) = INT_MAX;
            else
                ++bmin;
            if (bmax < dmax)
                bd(++bmax + 1) = INT_MAX;
            else
                --bmax;
            for (int d = bmax; d >= bmin; d -= 2) {
                int tlo = bd(d - 1), thi = bd(d + 1);
                int x = tlo < thi ? tlo : thi - 1;
                int y = x - d;
                while (x > xoff && y > yoff && a_[x - 1] == b_[y - 1]) {
                    --x;
                    --y;
                }
                bd(d) = x;
                if (!odd && fmin <= d && d <= fmax && x <= fd(d)) {
                    xmid = x;
                    ymid = y;
                    return;
                }
            }
        }
    }

    const std::vector<uint64_t>& a_;
    const std::vector<uint64_t>& b_;
    std::vector<bool> delA_, insB_;
    std::vector<int> fd_, bd_;
    int off_ = 0;
};

} // namespace

std::vector<Edit> diffSequences(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b)
{
    Myers m(a, b);
    m.run();
    return m.edits();
}

int commonLength(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b)
{
    int n = 0;
    for (const auto& e : diffSequences(a, b))
        if (e.op == EditOp::Equal)
            n += e.aEnd - e.aBegin;
    return n;
}

double similarity(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b)
{
    if (a.empty() && b.empty())
        return 1.0;
    return 2.0 * commonLength(a, b) / static_cast<double>(a.size() + b.size());
}

uint64_t hashString(std::string_view s, uint64_t seed)
{
    uint64_t h = seed;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

uint64_t hashCombine(uint64_t h, uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

std::string normalizeWhitespace(std::string_view line)
{
    std::string out;
    bool pendingSpace = false;
    for (char c : line) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            pendingSpace = !out.empty();
        } else {
            if (pendingSpace)
                out += ' ';
            pendingSpace = false;
            out += c;
        }
    }
    return out;
}

std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            size_t end = i;
            if (end > start && text[end - 1] == '\r')
                --end;
            lines.emplace_back(text, start, end - start);
            start = i + 1;
        }
    }
    if (start < text.size())
        lines.emplace_back(text, start, text.size() - start);
    return lines;
}

// ------------------------------------------------------------------------ slider compaction
// A port of the indent heuristic of git's xdiff (xdl_change_compact in xdiffi.c).

namespace {

constexpr int kMaxIndent = 200;
constexpr int kMaxBlanks = 20;
constexpr int kMaxSliding = 100;

int indentOf(const std::string& line)
{
    int ret = 0;
    for (char c : line) {
        if (c == ' ')
            ++ret;
        else if (c == '\t')
            ret += 8 - ret % 8;
        else if (c != '\r' && c != '\f' && c != '\v')
            return ret;
        if (ret >= kMaxIndent)
            return kMaxIndent;
    }
    return -1; // only whitespace
}

struct SplitMeasurement {
    bool endOfFile = false;
    int indent = -1, preBlank = 0, preIndent = -1, postBlank = 0, postIndent = -1;
};

struct SplitScore {
    int effectiveIndent = 0;
    int penalty = 0;
};

SplitMeasurement measureSplit(const std::vector<std::string>& lines, int split)
{
    SplitMeasurement m;
    const int n = static_cast<int>(lines.size());
    m.endOfFile = split >= n;
    m.indent = split >= n ? -1 : indentOf(lines[static_cast<size_t>(split)]);
    for (int i = split - 1; i >= 0; --i) {
        const int ind = indentOf(lines[static_cast<size_t>(i)]);
        if (ind != -1) {
            m.preIndent = ind;
            break;
        }
        if (++m.preBlank == kMaxBlanks) {
            m.preIndent = 0;
            break;
        }
    }
    for (int i = split + 1; i < n; ++i) {
        const int ind = indentOf(lines[static_cast<size_t>(i)]);
        if (ind != -1) {
            m.postIndent = ind;
            break;
        }
        if (++m.postBlank == kMaxBlanks) {
            m.postIndent = 0;
            break;
        }
    }
    return m;
}

void scoreAddSplit(const SplitMeasurement& m, SplitScore& s)
{
    constexpr int startOfFilePenalty = 1, endOfFilePenalty = 21, totalBlankWeight = -30, postBlankWeight = 6;
    constexpr int relativeIndentPenalty = -4, relativeIndentWithBlankPenalty = 10;
    constexpr int relativeOutdentPenalty = 24, relativeOutdentWithBlankPenalty = 17;
    constexpr int relativeDedentPenalty = 23, relativeDedentWithBlankPenalty = 17;

    if (m.preIndent == -1 && m.preBlank == 0)
        s.penalty += startOfFilePenalty;
    if (m.endOfFile)
        s.penalty += endOfFilePenalty;
    const int postBlank = m.indent == -1 ? 1 + m.postBlank : 0;
    const int totalBlank = m.preBlank + postBlank;
    s.penalty += totalBlankWeight * totalBlank;
    s.penalty += postBlankWeight * postBlank;
    const int indent = m.indent != -1 ? m.indent : m.postIndent;
    const bool anyBlanks = totalBlank != 0;
    s.effectiveIndent += indent;
    if (indent == -1 || m.preIndent == -1 || indent == m.preIndent) {
        // no adjustment
    } else if (indent > m.preIndent) {
        s.penalty += anyBlanks ? relativeIndentWithBlankPenalty : relativeIndentPenalty;
    } else if (m.postIndent != -1 && m.postIndent > indent) {
        s.penalty += anyBlanks ? relativeOutdentWithBlankPenalty : relativeOutdentPenalty;
    } else {
        s.penalty += anyBlanks ? relativeDedentWithBlankPenalty : relativeDedentPenalty;
    }
}

int scoreCmp(const SplitScore& a, const SplitScore& b)
{
    constexpr int indentWeight = 60;
    const int cmpIndents = (a.effectiveIndent > b.effectiveIndent) - (a.effectiveIndent < b.effectiveIndent);
    return indentWeight * cmpIndents + (a.penalty - b.penalty);
}

// `changed[i]` marks lines of one side that are added/removed. Slides every group of changed lines
// up or down over equal lines (which keeps the diff valid) to its best-scoring position.
void compact(std::vector<char>& changed, const std::vector<uint64_t>& h, const std::vector<std::string>& lines)
{
    const int n = static_cast<int>(changed.size());
    auto at = [&](int i) { return i >= 0 && i < n && changed[static_cast<size_t>(i)]; };
    for (int start = 0; start < n;) {
        if (!changed[static_cast<size_t>(start)]) {
            ++start;
            continue;
        }
        int end = start;
        while (at(end))
            ++end;
        int groupSize;
        int earliestEnd;
        do {
            groupSize = end - start;
            // Up as far as possible, absorbing groups that become adjacent.
            while (start > 0 && h[static_cast<size_t>(start - 1)] == h[static_cast<size_t>(end - 1)]) {
                changed[static_cast<size_t>(--start)] = 1;
                changed[static_cast<size_t>(--end)] = 0;
                while (at(start - 1))
                    --start;
            }
            earliestEnd = end;
            // Then down as far as possible.
            while (end < n && h[static_cast<size_t>(start)] == h[static_cast<size_t>(end)]) {
                changed[static_cast<size_t>(start++)] = 0;
                changed[static_cast<size_t>(end++)] = 1;
                while (at(end))
                    ++end;
            }
        } while (groupSize != end - start);

        if (end > earliestEnd) {
            int shift = std::max({earliestEnd, end - groupSize - 1, end - kMaxSliding});
            int bestShift = -1;
            SplitScore best;
            for (; shift <= end; ++shift) {
                SplitScore score;
                scoreAddSplit(measureSplit(lines, shift), score);
                scoreAddSplit(measureSplit(lines, shift - groupSize), score);
                if (bestShift == -1 || scoreCmp(score, best) <= 0) {
                    best = score;
                    bestShift = shift;
                }
            }
            while (end > bestShift) {
                changed[static_cast<size_t>(--start)] = 1;
                changed[static_cast<size_t>(--end)] = 0;
            }
        }
        start = end;
    }
}

} // namespace

std::vector<Edit> diffLinesReadable(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b,
                                    const std::vector<std::string>& aLines, const std::vector<std::string>& bLines)
{
    std::vector<char> del(a.size(), 0), ins(b.size(), 0);
    for (const auto& e : diffSequences(a, b)) {
        if (e.op == EditOp::Delete)
            std::fill(del.begin() + e.aBegin, del.begin() + e.aEnd, 1);
        else if (e.op == EditOp::Insert)
            std::fill(ins.begin() + e.bBegin, ins.begin() + e.bEnd, 1);
    }
    compact(del, a, aLines);
    compact(ins, b, bLines);

    // Rebuild the edit script; unchanged lines still pair up in order.
    std::vector<Edit> out;
    const int n = static_cast<int>(a.size()), m = static_cast<int>(b.size());
    int i = 0, j = 0;
    while (i < n || j < m) {
        if (i < n && del[static_cast<size_t>(i)]) {
            const int s0 = i;
            while (i < n && del[static_cast<size_t>(i)])
                ++i;
            out.push_back({EditOp::Delete, s0, i, j, j});
        } else if (j < m && ins[static_cast<size_t>(j)]) {
            const int s0 = j;
            while (j < m && ins[static_cast<size_t>(j)])
                ++j;
            out.push_back({EditOp::Insert, i, i, s0, j});
        } else {
            const int si = i, sj = j;
            while (i < n && j < m && !del[static_cast<size_t>(i)] && !ins[static_cast<size_t>(j)]) {
                ++i;
                ++j;
            }
            if (i == si) // can't happen while both sides agree on the unchanged lines
                break;
            out.push_back({EditOp::Equal, si, i, sj, j});
        }
    }
    return out;
}

} // namespace cr
