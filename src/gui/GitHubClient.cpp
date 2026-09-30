#include "GitHubClient.hpp"

#include "core/Process.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace gui {

namespace {

// $GITHUB_TOKEN / $GH_TOKEN, else the token of a logged-in GitHub CLI for that host.
QByteArray tokenFor(const QString& host)
{
    QByteArray token = qgetenv("GITHUB_TOKEN");
    if (token.isEmpty())
        token = qgetenv("GH_TOKEN");
    if (!token.isEmpty())
        return token;
    static QHash<QString, QByteArray> cache;
    auto it = cache.find(host);
    if (it == cache.end()) {
        const auto r = cr::runProcess({"gh", "auth", "token", "--hostname", host.toStdString()});
        it = cache.insert(host, r.ok() ? QByteArray::fromStdString(r.out).trimmed() : QByteArray());
    }
    return *it;
}

QNetworkRequest apiRequest(const QString& host, const QString& path)
{
    const QString api = host.isEmpty() || host == QLatin1String("github.com") ? QStringLiteral("https://api.github.com")
                                                                             : QStringLiteral("https://%1/api/v3").arg(host);
    QNetworkRequest req(QUrl(api + path));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("User-Agent", "cppreviewer");
    const QByteArray token = tokenFor(host.isEmpty() ? QStringLiteral("github.com") : host);
    if (!token.isEmpty())
        req.setRawHeader("Authorization", "Bearer " + token);
    return req;
}

QString errorOf(QNetworkReply* reply, const QByteArray& body)
{
    QString msg = reply->errorString();
    auto doc = QJsonDocument::fromJson(body);
    if (doc.isObject() && doc.object().contains(QStringLiteral("message")))
        msg = doc.object().value(QStringLiteral("message")).toString();
    return msg;
}

PullRequestInfo parsePullRequest(const QJsonObject& o)
{
    PullRequestInfo pr;
    pr.number = o.value(QStringLiteral("number")).toInt();
    pr.title = o.value(QStringLiteral("title")).toString();
    pr.author = o.value(QStringLiteral("user")).toObject().value(QStringLiteral("login")).toString();
    pr.baseRef = o.value(QStringLiteral("base")).toObject().value(QStringLiteral("ref")).toString();
    pr.headRef = o.value(QStringLiteral("head")).toObject().value(QStringLiteral("label")).toString();
    pr.updated = o.value(QStringLiteral("updated_at")).toString().left(16).replace(QLatin1Char('T'), QLatin1Char(' '));
    pr.draft = o.value(QStringLiteral("draft")).toBool();
    return pr;
}

} // namespace

GitHubClient::GitHubClient(QObject* parent) : QObject(parent) {}

void GitHubClient::getPullRequest(const QString& host, const QString& slug, int number,
                                  std::function<void(PullRequestInfo, QString)> done)
{
    QNetworkReply* reply = nam_.get(apiRequest(host, QStringLiteral("/repos/%1/pulls/%2").arg(slug).arg(number)));
    connect(reply, &QNetworkReply::finished, this, [reply, done = std::move(done)] {
        reply->deleteLater();
        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            done({}, errorOf(reply, body));
            return;
        }
        done(parsePullRequest(QJsonDocument::fromJson(body).object()), {});
    });
}

void GitHubClient::listPullRequests(const QString& host, const QString& slug,
                                    std::function<void(QVector<PullRequestInfo>, QString)> done)
{
    QNetworkReply* reply = nam_.get(apiRequest(host, QStringLiteral("/repos/%1/pulls?state=open&per_page=100").arg(slug)));
    connect(reply, &QNetworkReply::finished, this, [reply, done = std::move(done)] {
        reply->deleteLater();
        QVector<PullRequestInfo> prs;
        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            done(prs, errorOf(reply, body));
            return;
        }
        for (const auto& v : QJsonDocument::fromJson(body).array())
            prs.push_back(parsePullRequest(v.toObject()));
        done(prs, {});
    });
}

} // namespace gui
