#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <functional>

namespace gui {

struct PullRequestInfo {
    int number = 0;
    QString title;
    QString author;
    QString baseRef;
    QString headRef;
    QString updated;
    bool draft = false;
};

// Lists pull requests through the GitHub REST API. Uses $GITHUB_TOKEN (or $GH_TOKEN) when set,
// which is required for private repositories and raises the rate limit.
class GitHubClient : public QObject {
    Q_OBJECT
public:
    explicit GitHubClient(QObject* parent = nullptr);

    void listPullRequests(const QString& slug, std::function<void(QVector<PullRequestInfo>, QString error)> done);

private:
    QNetworkAccessManager nam_;
};

} // namespace gui
