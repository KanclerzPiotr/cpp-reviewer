#include "Sessions.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

namespace gui {

namespace {

constexpr int kMaxSessions = 40;

QString storePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/sessions.json");
}

QString kindName(cr::Revision::Kind k)
{
    using K = cr::Revision::Kind;
    switch (k) {
    case K::Commit: return QStringLiteral("commit");
    case K::WorkingTree: return QStringLiteral("worktree");
    case K::Index: return QStringLiteral("index");
    case K::Directory: return QStringLiteral("directory");
    }
    return {};
}

QJsonObject revisionJson(const cr::Revision& r)
{
    return QJsonObject{{QStringLiteral("kind"), kindName(r.kind)},
                       {QStringLiteral("ref"), QString::fromStdString(r.ref)},
                       {QStringLiteral("sha"), QString::fromStdString(r.sha)},
                       {QStringLiteral("label"), QString::fromStdString(r.label)}};
}

cr::Revision revisionFrom(const QJsonObject& o)
{
    const QString kind = o.value(QStringLiteral("kind")).toString();
    const auto ref = o.value(QStringLiteral("ref")).toString().toStdString();
    if (kind == QLatin1String("worktree"))
        return cr::Revision::workingTree();
    if (kind == QLatin1String("index"))
        return cr::Revision::index();
    if (kind == QLatin1String("directory"))
        return cr::Revision::directory(ref);
    return cr::Revision::commit(ref, o.value(QStringLiteral("sha")).toString().toStdString(),
                                o.value(QStringLiteral("label")).toString().toStdString());
}

QString revisionKey(const cr::Revision& r)
{
    return r.kind == cr::Revision::Kind::Commit ? QString::fromStdString(r.sha) : kindName(r.kind);
}

} // namespace

QString SessionRecord::key() const
{
    switch (kind) {
    case Kind::Revisions: return QStringLiteral("rev|%1|%2|%3").arg(repo, revisionKey(base), revisionKey(target));
    case Kind::PullRequests: return QStringLiteral("prs|%1|%2").arg(linkA, linkB);
    case Kind::Directories: return QStringLiteral("dirs|%1|%2").arg(oldDir, newDir);
    }
    return {};
}

QString SessionRecord::title() const
{
    switch (kind) {
    case Kind::Revisions:
        return QString::fromStdString(base.display()) + QStringLiteral(" → ") + QString::fromStdString(target.display());
    case Kind::PullRequests:
        return (a.label.isEmpty() ? linkA : a.label) + QStringLiteral(" ⇄ ") + (b.label.isEmpty() ? linkB : b.label);
    case Kind::Directories:
        return oldDir + QStringLiteral(" → ") + newDir;
    }
    return {};
}

QJsonObject SessionRecord::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("kind"), kind == Kind::Revisions      ? QStringLiteral("revisions")
                                     : kind == Kind::PullRequests ? QStringLiteral("pullrequests")
                                                                  : QStringLiteral("directories"));
    o.insert(QStringLiteral("repo"), repo);
    o.insert(QStringLiteral("used"), used.toString(Qt::ISODate));
    if (kind == Kind::Revisions) {
        o.insert(QStringLiteral("base"), revisionJson(base));
        o.insert(QStringLiteral("target"), revisionJson(target));
        if (!prLink.isEmpty())
            o.insert(QStringLiteral("prLink"), prLink);
    } else if (kind == Kind::PullRequests) {
        o.insert(QStringLiteral("linkA"), linkA);
        o.insert(QStringLiteral("linkB"), linkB);
        o.insert(QStringLiteral("mapping"), mapping);
        o.insert(QStringLiteral("finalFiles"), finalFiles);
        for (auto [name, s] : {std::pair{"a", &a}, std::pair{"b", &b}})
            o.insert(QLatin1String(name), QJsonObject{{QStringLiteral("repo"), s->repo},
                                                      {QStringLiteral("base"), s->base},
                                                      {QStringLiteral("head"), s->head},
                                                      {QStringLiteral("title"), s->title},
                                                      {QStringLiteral("label"), s->label},
                                                      {QStringLiteral("cached"), s->cached}});
    } else {
        o.insert(QStringLiteral("oldDir"), oldDir);
        o.insert(QStringLiteral("newDir"), newDir);
    }
    o.insert(QStringLiteral("file"), file);
    o.insert(QStringLiteral("line"), line);
    QJsonArray tabArray;
    for (const auto& t : tabs)
        tabArray.push_back(QJsonObject{{QStringLiteral("side"), t.side == cr::Side::Old ? QStringLiteral("base") : QStringLiteral("target")},
                                       {QStringLiteral("path"), t.path},
                                       {QStringLiteral("line"), t.line}});
    o.insert(QStringLiteral("tabs"), tabArray);
    return o;
}

SessionRecord SessionRecord::fromJson(const QJsonObject& o)
{
    SessionRecord r;
    const QString kind = o.value(QStringLiteral("kind")).toString();
    r.kind = kind == QLatin1String("pullrequests") ? Kind::PullRequests
             : kind == QLatin1String("directories") ? Kind::Directories
                                                    : Kind::Revisions;
    r.repo = o.value(QStringLiteral("repo")).toString();
    r.used = QDateTime::fromString(o.value(QStringLiteral("used")).toString(), Qt::ISODate);
    r.base = revisionFrom(o.value(QStringLiteral("base")).toObject());
    r.target = revisionFrom(o.value(QStringLiteral("target")).toObject());
    r.prLink = o.value(QStringLiteral("prLink")).toString();
    r.linkA = o.value(QStringLiteral("linkA")).toString();
    r.linkB = o.value(QStringLiteral("linkB")).toString();
    r.mapping = o.value(QStringLiteral("mapping")).toString();
    r.finalFiles = o.value(QStringLiteral("finalFiles")).toBool();
    for (auto [name, s] : {std::pair{"a", &r.a}, std::pair{"b", &r.b}}) {
        const auto so = o.value(QLatin1String(name)).toObject();
        s->repo = so.value(QStringLiteral("repo")).toString();
        s->base = so.value(QStringLiteral("base")).toString();
        s->head = so.value(QStringLiteral("head")).toString();
        s->title = so.value(QStringLiteral("title")).toString();
        s->label = so.value(QStringLiteral("label")).toString();
        s->cached = so.value(QStringLiteral("cached")).toBool();
    }
    r.oldDir = o.value(QStringLiteral("oldDir")).toString();
    r.newDir = o.value(QStringLiteral("newDir")).toString();
    r.file = o.value(QStringLiteral("file")).toString();
    r.line = o.value(QStringLiteral("line")).toInt();
    for (const auto& v : o.value(QStringLiteral("tabs")).toArray()) {
        const auto t = v.toObject();
        r.tabs.push_back({t.value(QStringLiteral("side")).toString() == QLatin1String("base") ? cr::Side::Old : cr::Side::New,
                          t.value(QStringLiteral("path")).toString(), t.value(QStringLiteral("line")).toInt()});
    }
    return r;
}

QVector<SessionRecord> SessionStore::load()
{
    QVector<SessionRecord> out;
    QFile f(storePath());
    if (!f.open(QIODevice::ReadOnly))
        return out;
    for (const auto& v : QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("sessions")).toArray())
        out.push_back(SessionRecord::fromJson(v.toObject()));
    std::stable_sort(out.begin(), out.end(), [](const SessionRecord& x, const SessionRecord& y) { return x.used > y.used; });
    return out;
}

void SessionStore::save(SessionRecord record)
{
    if (!record.used.isValid())
        record.used = QDateTime::currentDateTime();
    auto all = load();
    const QString key = record.key();
    all.erase(std::remove_if(all.begin(), all.end(), [&](const SessionRecord& r) { return r.key() == key; }), all.end());
    all.prepend(record);
    if (all.size() > kMaxSessions)
        all.resize(kMaxSessions);
    write(all);
}

void SessionStore::remove(const QString& key)
{
    auto all = load();
    all.erase(std::remove_if(all.begin(), all.end(), [&](const SessionRecord& r) { return r.key() == key; }), all.end());
    write(all);
}

void SessionStore::write(const QVector<SessionRecord>& records)
{
    QJsonArray arr;
    for (const auto& r : records)
        arr.push_back(r.toJson());
    QSaveFile f(storePath());
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(QJsonObject{{QStringLiteral("sessions"), arr}}).toJson());
        f.commit();
    }
}

} // namespace gui
