#include "Model.hpp"

#include "Diff.hpp"

#include <algorithm>
#include <unordered_map>

namespace cr {

const char* changeKindName(ChangeKind k)
{
    switch (k) {
    case ChangeKind::Added: return "Added";
    case ChangeKind::Removed: return "Removed";
    case ChangeKind::Modified: return "Modified";
    case ChangeKind::Renamed: return "Renamed";
    case ChangeKind::Moved: return "Moved";
    case ChangeKind::Reordered: return "Reordered";
    case ChangeKind::SignatureChanged: return "Signature changed";
    case ChangeKind::Extracted: return "Extracted";
    case ChangeKind::Inlined: return "Inlined";
    case ChangeKind::MovedCode: return "Moved code";
    case ChangeKind::CopiedCode: return "Copied code";
    case ChangeKind::SymbolRenamed: return "Symbol renamed";
    case ChangeKind::MovedLines: return "Moved lines";
    }
    return "?";
}

std::vector<uint64_t> tokenIds(const std::vector<Token>& toks, int begin, int end, bool normalized,
                               const std::vector<std::pair<int, int>>* skip)
{
    std::vector<uint64_t> out;
    if (end <= begin)
        return out;
    out.reserve(static_cast<size_t>(end - begin));
    std::vector<bool> skipped;
    if (skip && !skip->empty()) {
        skipped.assign(static_cast<size_t>(end - begin), false);
        for (auto [b, e] : *skip)
            for (int i = std::max(b, begin); i < std::min(e, end); ++i)
                skipped[static_cast<size_t>(i - begin)] = true;
    }
    std::unordered_map<std::string_view, uint64_t> ids;
    for (int i = begin; i < end; ++i) {
        const auto& t = toks[static_cast<size_t>(i)];
        if (t.kind == TokKind::Comment || (!skipped.empty() && skipped[static_cast<size_t>(i - begin)]))
            continue;
        if (normalized && t.kind == TokKind::Ident) {
            auto [it, inserted] = ids.emplace(t.text, ids.size() + 1);
            out.push_back(0x5eed0000000000ULL ^ it->second);
        } else {
            out.push_back(hashString(t.text));
        }
    }
    return out;
}

uint64_t hashIds(const std::vector<uint64_t>& ids)
{
    uint64_t h = 1469598103934665603ULL;
    for (auto v : ids)
        h = hashCombine(h, v);
    return h;
}

} // namespace cr
