#include "Bookmarks.hpp"

#include "core/Diff.hpp"
#include "core/Snapshot.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>

namespace gui {

namespace {

QString key(cr::Side side, const QString& path)
{
    return (side == cr::Side::Old ? QStringLiteral("o:") : QStringLiteral("n:")) + path;
}

} // namespace

void BookmarkStore::load(const std::string& projectRoot)
{
    items_.clear();
    const QString dir = QString::fromStdString(cr::cacheDirectory()) + QStringLiteral("/bookmarks");
    QDir().mkpath(dir);
    path_ = dir + QStringLiteral("/%1.json").arg(cr::hashString(projectRoot), 16, 16, QLatin1Char('0'));
    QFile f(path_);
    if (f.open(QIODevice::ReadOnly)) {
        for (const auto& v : QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("bookmarks")).toArray()) {
            const auto o = v.toObject();
            Bookmark b;
            b.side = o.value(QStringLiteral("side")).toString() == QLatin1String("base") ? cr::Side::Old : cr::Side::New;
            b.path = o.value(QStringLiteral("path")).toString();
            b.line = o.value(QStringLiteral("line")).toInt();
            b.text = o.value(QStringLiteral("text")).toString();
            b.comment = o.value(QStringLiteral("comment")).toString();
            b.revision = o.value(QStringLiteral("revision")).toString();
            b.created = QDateTime::fromString(o.value(QStringLiteral("created")).toString(), Qt::ISODate);
            if (!b.path.isEmpty())
                items_.push_back(std::move(b));
        }
    }
    reindex();
}

void BookmarkStore::save() const
{
    if (path_.isEmpty())
        return;
    QJsonArray arr;
    for (const auto& b : items_) {
        QJsonObject o;
        o.insert(QStringLiteral("side"), b.side == cr::Side::Old ? QStringLiteral("base") : QStringLiteral("target"));
        o.insert(QStringLiteral("path"), b.path);
        o.insert(QStringLiteral("line"), b.line);
        o.insert(QStringLiteral("text"), b.text);
        o.insert(QStringLiteral("comment"), b.comment);
        o.insert(QStringLiteral("revision"), b.revision);
        o.insert(QStringLiteral("created"), b.created.toString(Qt::ISODate));
        arr.push_back(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("bookmarks"), arr);
    QSaveFile f(path_);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(root).toJson());
        f.commit();
    }
}

void BookmarkStore::reindex()
{
    std::stable_sort(items_.begin(), items_.end(), [](const Bookmark& a, const Bookmark& b) {
        return std::tie(a.path, a.side, a.line) < std::tie(b.path, b.side, b.line);
    });
    index_.clear();
    for (int i = 0; i < items_.size(); ++i)
        if (!items_[i].stale)
            index_[key(items_[i].side, items_[i].path)].insert(items_[i].line, i);
}

int BookmarkStore::indexOf(cr::Side side, const QString& path, int line) const
{
    auto it = index_.find(key(side, path));
    return it == index_.end() ? -1 : it->value(line, -1);
}

std::optional<QString> BookmarkStore::commentAt(cr::Side side, const QString& path, int line) const
{
    const int i = indexOf(side, path, line);
    if (i < 0)
        return std::nullopt;
    return items_[i].comment;
}

void BookmarkStore::add(Bookmark b)
{
    items_.push_back(std::move(b));
    reindex();
    save();
}

void BookmarkStore::remove(int index)
{
    if (index < 0 || index >= items_.size())
        return;
    items_.removeAt(index);
    reindex();
    save();
}

void BookmarkStore::setComment(int index, const QString& comment)
{
    if (index < 0 || index >= items_.size())
        return;
    items_[index].comment = comment;
    save();
}

void BookmarkStore::relocate(const LinesFn& lines)
{
    bool moved = false;
    for (auto& b : items_) {
        const auto content = lines(b.side, b.path);
        const std::string wanted = cr::normalizeWhitespace(b.text.toStdString());
        auto same = [&](int l) {
            return l >= 0 && l < static_cast<int>(content.size()) &&
                   cr::normalizeWhitespace(content[static_cast<size_t>(l)]) == wanted;
        };
        b.stale = false;
        if (same(b.line))
            continue;
        int found = -1;
        if (!wanted.empty()) // blank lines can't be told apart
            for (int d = 1; d < static_cast<int>(content.size()) && found < 0; ++d) {
                if (same(b.line - d))
                    found = b.line - d;
                else if (same(b.line + d))
                    found = b.line + d;
            }
        if (found >= 0) {
            b.line = found;
            moved = true;
        } else {
            b.stale = true;
        }
    }
    reindex();
    if (moved)
        save();
}

QString BookmarkStore::toMarkdown() const
{
    QString out;
    for (const auto& b : items_) {
        out += QStringLiteral("**`%1:%2`**").arg(b.path).arg(b.line + 1);
        if (b.side == cr::Side::Old)
            out += QStringLiteral(" (base)");
        out += QStringLiteral("\n```cpp\n%1\n```\n").arg(b.text);
        if (!b.comment.isEmpty())
            out += b.comment + QLatin1Char('\n');
        out += QLatin1Char('\n');
    }
    return out;
}

} // namespace gui
