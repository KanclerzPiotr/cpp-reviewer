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

// Lists pull requests through the GitHub REST API. Uses $GITHUB_TOKEN (or $GH_TOKEN), or else the
// token of a logged-in GitHub CLI (`gh auth token`); required for private repositories.
class GitHubClient : public QObject {
    Q_OBJECT
public:
    explicit GitHubClient(QObject* parent = nullptr);

    // Open pull requests of `slug` ("owner/repo") on github.com or a GitHub Enterprise `host`.
    void listPullRequests(const QString& host, const QString& slug,
                          std::function<void(QVector<PullRequestInfo>, QString error)> done);
    // One pull request, on github.com or a GitHub Enterprise `host`.
    void getPullRequest(const QString& host, const QString& slug, int number,
                        std::function<void(PullRequestInfo, QString error)> done);

private:
    QNetworkAccessManager nam_;
};

} // namespace gui
