#pragma once

#include "core/Git.hpp"
#include "core/Snapshot.hpp"

#include <QDialog>
#include <optional>

class QComboBox;
class QLineEdit;
class QTreeWidget;
class QTabWidget;
class QRadioButton;
class QLabel;
class QSpinBox;
class QPushButton;

namespace gui {

class GitHubClient;

// Picks one revision: a commit from the log, a branch/tag, the working tree or the index.
class RevisionDialog : public QDialog {
    Q_OBJECT
public:
    RevisionDialog(const cr::GitRepo& repo, const QString& title, QWidget* parent = nullptr);
    std::optional<cr::Revision> selectedRevision() const;

private:
    void loadLog(bool append);
    void applyFilter();

    const cr::GitRepo& repo_;
    QTabWidget* tabs_;
    QComboBox* refCombo_;
    QLineEdit* filter_;
    QTreeWidget* log_;
    QPushButton* more_;
    QTreeWidget* refs_;
    QRadioButton* workTree_;
    QRadioButton* index_;
    QRadioButton* custom_;
    QLineEdit* customEdit_;
    int loaded_ = 0;
};

struct PullRequestChoice {
    QString remote;  // remote name, or a URL to fetch from
    int number = 0;
    QString baseRef; // empty: the PR's target branch (from refs/pull/N/merge)
    QString title;
    QString host, slug; // repository on GitHub, for the API
};

// Resolves a pasted pull request link (https://github.com/owner/repo/pull/123) against `repo`: fetched
// from the remote pointing to that repository, or from the link itself. `note` explains the choice.
std::optional<PullRequestChoice> pullRequestFromUrl(const cr::GitRepo& repo, const QString& text,
                                                    QString* note = nullptr);

// Lists open GitHub pull requests of a remote, or accepts a PR number typed by hand.
class PullRequestDialog : public QDialog {
    Q_OBJECT
public:
    PullRequestDialog(const cr::GitRepo& repo, QWidget* parent = nullptr);
    PullRequestChoice choice() const;

private:
    void refresh();

    const cr::GitRepo& repo_;
    GitHubClient* client_;
    QComboBox* remote_;
    QTreeWidget* list_;
    QSpinBox* number_;
    QLineEdit* base_;
    QLineEdit* link_;
    QLabel* status_;
    QString title_;
    bool fromLink_ = false; // base branch comes from the link's PR, not the remote's default
};

} // namespace gui
