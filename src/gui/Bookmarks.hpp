#pragma once

#include "core/Model.hpp"

#include <QDateTime>
#include <QHash>
#include <QString>
#include <QVector>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace gui {

// A bookmarked line, optionally with a review comment.
struct Bookmark {
    cr::Side side = cr::Side::New;
    QString path;     // relative to the snapshot, or absolute for files outside it
    int line = 0;     // 0-based, where it was last found
    QString text;     // the line's content, used to find it again when lines move
    QString comment;
    QString revision; // revision it was made on, for display
    QDateTime created;
    bool stale = false; // the line wasn't found in the current review
};

// Bookmarks of one project, persisted in the cache directory.
class BookmarkStore {
public:
    using LinesFn = std::function<std::vector<std::string>(cr::Side side, const QString& path)>;

    void load(const std::string& projectRoot);
    void save() const;

    const QVector<Bookmark>& items() const { return items_; }
    int indexOf(cr::Side side, const QString& path, int line) const;
    std::optional<QString> commentAt(cr::Side side, const QString& path, int line) const;

    void add(Bookmark b);
    void remove(int index);
    void setComment(int index, const QString& comment);

    // Moves bookmarks to where their line is now (nearest line with the same text), marking
    // the ones whose line is gone as stale.
    void relocate(const LinesFn& lines);

    // Every bookmark as Markdown: location, the code line and the comment.
    QString toMarkdown() const;

private:
    void reindex();

    QString path_;
    QVector<Bookmark> items_;
    QHash<QString, QHash<int, int>> index_; // side+path -> line -> item
};

} // namespace gui
