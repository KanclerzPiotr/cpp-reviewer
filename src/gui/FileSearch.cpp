#include "FileSearch.hpp"

#include <QRegularExpression>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;

namespace gui {

namespace {

constexpr uintmax_t kMaxFileSize = 8 * 1024 * 1024;

std::string lowerAscii(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

std::vector<SearchHit> searchFiles(const std::vector<SearchTarget>& targets, const SearchQuery& query,
                                   const std::atomic<bool>& cancel, size_t maxHits, bool& truncated, QString* error)
{
    truncated = false;
    std::vector<SearchHit> out;
    if (query.text.isEmpty())
        return out;

    QString pattern = query.regex ? query.text : QRegularExpression::escape(query.text);
    if (query.wholeWord)
        pattern = QStringLiteral("\\b(?:") + pattern + QStringLiteral(")\\b");
    QRegularExpression re(pattern, query.caseSensitive ? QRegularExpression::NoPatternOption
                                                       : QRegularExpression::CaseInsensitiveOption);
    if (!re.isValid()) {
        if (error)
            *error = re.errorString();
        return out;
    }
    re.optimize();
    // Plain text is looked up in the raw bytes first, so most files are rejected cheaply.
    const std::string needle = query.regex ? std::string() : query.text.toStdString();
    const std::string lowerNeedle = lowerAscii(needle);

    struct Job {
        size_t target, file;
    };
    std::vector<Job> jobs;
    for (size_t t = 0; t < targets.size(); ++t)
        for (size_t f = 0; f < targets[t].files.size(); ++f)
            jobs.push_back({t, f});

    std::vector<std::vector<SearchHit>> perJob(jobs.size());
    std::atomic<size_t> next{0}, hits{0};
    auto worker = [&] {
        std::string buf;
        for (;;) {
            if (cancel || hits >= maxHits)
                return;
            const size_t j = next++;
            if (j >= jobs.size())
                return;
            const auto& target = targets[jobs[j].target];
            const auto& rel = target.files[jobs[j].file];
            const auto abs = fs::path(target.root) / rel;
            std::error_code ec;
            const auto size = fs::file_size(abs, ec);
            if (ec || size == 0 || size > kMaxFileSize)
                continue;
            std::ifstream in(abs, std::ios::binary);
            buf.assign(size, '\0');
            in.read(buf.data(), static_cast<std::streamsize>(size));
            buf.resize(static_cast<size_t>(in.gcount()));
            if (buf.find('\0', 0) < std::min<size_t>(buf.size(), 8000))
                continue; // binary
            if (!needle.empty()) {
                const bool present = query.caseSensitive ? buf.find(needle) != std::string::npos
                                                         : lowerAscii(buf).find(lowerNeedle) != std::string::npos;
                if (!present)
                    continue;
            }
            auto& found = perJob[j];
            size_t pos = 0;
            for (int line = 1; pos <= buf.size(); ++line) {
                size_t eol = buf.find('\n', pos);
                if (eol == std::string::npos)
                    eol = buf.size();
                std::string_view sv(buf.data() + pos, eol - pos);
                if (!sv.empty() && sv.back() == '\r')
                    sv.remove_suffix(1);
                const bool candidate = needle.empty() || !query.caseSensitive || sv.find(needle) != std::string_view::npos;
                if (candidate) {
                    const QString text = QString::fromUtf8(sv.data(), static_cast<qsizetype>(sv.size()));
                    if (re.match(text).hasMatch()) {
                        found.push_back({target.side, QString::fromStdString(rel), line, text});
                        if (++hits >= maxHits)
                            break;
                    }
                }
                pos = eol + 1;
            }
        }
    };
    const unsigned n = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < n; ++i)
        threads.emplace_back(worker);
    for (auto& th : threads)
        th.join();

    for (auto& v : perJob)
        for (auto& h : v) {
            if (out.size() >= maxHits) {
                truncated = true;
                return out;
            }
            out.push_back(std::move(h));
        }
    truncated = hits >= maxHits;
    return out;
}

} // namespace gui
