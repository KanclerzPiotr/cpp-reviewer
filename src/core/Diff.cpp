#include "Diff.hpp"

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

} // namespace cr
