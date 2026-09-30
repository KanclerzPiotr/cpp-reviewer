#pragma once

#include "core/Model.hpp"
#include "core/Snapshot.hpp"

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace gui {

// A review that can be resumed: what was compared and where the reviewer was.
struct SessionRecord {
    enum class Kind { Revisions, PullRequests, Directories };
    Kind kind = Kind::Revisions;
    QString repo;
    cr::Revision base, target;   // Revisions (a single pull request is stored as its two commits)
    QString prLink;              // the pull request a Revisions session came from, to fetch it again
    QString linkA, linkB, mapping; // PullRequests
    struct PrSide {
        QString repo, base, head, title, label;
        bool cached = false;
    } a, b;                      // PullRequests: the commits, to resume without fetching
    bool finalFiles = false;     // PullRequests: showing the final files instead of the interdiff
    QString oldDir, newDir;      // Directories

    // Where the reviewer was.
    QString file;                // current file of the diff (target path, else base path)
    int line = 0;                // 1-based, in the target side
    struct Tab {
        cr::Side side;
        QString path;
        int line;
    };
    QVector<Tab> tabs;           // open file tabs
    QDateTime used;

    QString key() const;         // identifies the comparison, for replacing an older record
    QString title() const;       // one line for the session list
    QJsonObject toJson() const;
    static SessionRecord fromJson(const QJsonObject& o);
};

// The most recent sessions, newest first, in the configuration directory.
class SessionStore {
public:
    static QVector<SessionRecord> load();
    // Adds or replaces the record of the same comparison and keeps the newest ones.
    static void save(SessionRecord record);
    static void remove(const QString& key);

private:
    static void write(const QVector<SessionRecord>& records);
};

} // namespace gui
