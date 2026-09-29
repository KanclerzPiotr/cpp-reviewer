#include "GitHubClient.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace gui {

GitHubClient::GitHubClient(QObject* parent) : QObject(parent) {}

void GitHubClient::listPullRequests(const QString& slug,
                                    std::function<void(QVector<PullRequestInfo>, QString)> done)
{
    QNetworkRequest req(QUrl(QStringLiteral("https://api.github.com/repos/%1/pulls?state=open&per_page=100").arg(slug)));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("User-Agent", "cppreviewer");
    QByteArray token = qgetenv("GITHUB_TOKEN");
    if (token.isEmpty())
        token = qgetenv("GH_TOKEN");
    if (!token.isEmpty())
        req.setRawHeader("Authorization", "Bearer " + token);

    QNetworkReply* reply = nam_.get(req);
    connect(reply, &QNetworkReply::finished, this, [reply, done = std::move(done)] {
        reply->deleteLater();
        QVector<PullRequestInfo> prs;
        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            QString msg = reply->errorString();
            auto doc = QJsonDocument::fromJson(body);
            if (doc.isObject() && doc.object().contains(QStringLiteral("message")))
                msg = doc.object().value(QStringLiteral("message")).toString();
            done(prs, msg);
            return;
        }
        const auto arr = QJsonDocument::fromJson(body).array();
        for (const auto& v : arr) {
            const auto o = v.toObject();
            PullRequestInfo pr;
            pr.number = o.value(QStringLiteral("number")).toInt();
            pr.title = o.value(QStringLiteral("title")).toString();
            pr.author = o.value(QStringLiteral("user")).toObject().value(QStringLiteral("login")).toString();
            pr.baseRef = o.value(QStringLiteral("base")).toObject().value(QStringLiteral("ref")).toString();
            pr.headRef = o.value(QStringLiteral("head")).toObject().value(QStringLiteral("label")).toString();
            pr.updated = o.value(QStringLiteral("updated_at")).toString().left(16).replace(QLatin1Char('T'), QLatin1Char(' '));
            pr.draft = o.value(QStringLiteral("draft")).toBool();
            prs.push_back(pr);
        }
        done(prs, {});
    });
}

} // namespace gui
