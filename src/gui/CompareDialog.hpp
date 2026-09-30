#pragma once

#include "RevisionDialog.hpp"
#include "Sessions.hpp"
#include "core/Git.hpp"
#include "core/Model.hpp"
#include "core/Snapshot.hpp"

#include <QDialog>
#include <optional>

class QButtonGroup;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QStackedWidget;
class QTabWidget;
class QTreeWidget;

namespace gui {

// What to review, chosen in the New Comparison dialog.
struct CompareChoice {
    enum class Kind { Revisions, PullRequest, Directories, PullRequests, Resume };
    Kind kind = Kind::Revisions;
    QString repo;
    cr::Revision base, target;
    PullRequestChoice pr;
    QString oldDir, newDir;
    QString linkA, linkB, mapping; // PullRequests; mapping empty = guess
    SessionRecord session;         // Resume
    bool latest = false;           // Resume: fetch the pull request(s) again instead of the saved commits
};

// Shown at startup (and via File → New Comparison): pick a repository and what to compare in it,
// or two plain directories.
class CompareDialog : public QDialog {
    Q_OBJECT
public:
    explicit CompareDialog(const QString& repoPath, QWidget* parent = nullptr);
    CompareChoice choice() const { return choice_; }

    void accept() override;

private:
    void openRepo(const QString& path);
    void pickRevision(cr::Side side);
    void applyPreset(const QString& preset);
    void updateState();
    void setLink(const QString& text);
    QWidget* buildSessionsPage();
    void updateButtons();

    std::optional<cr::GitRepo> repo_;
    CompareChoice choice_;
    std::optional<cr::Revision> picked_[2]; // chosen in the log browser, until the text is edited
    std::optional<PullRequestChoice> pr_;

    QLineEdit* repoEdit_;
    QLabel* repoInfo_;
    QRadioButton* revisionsMode_;
    QRadioButton* prMode_;
    QRadioButton* dirsMode_;
    QRadioButton* prsMode_;
    QLineEdit* linkAEdit_;
    QLineEdit* linkBEdit_;
    QLineEdit* mappingEdit_;
    QStackedWidget* pages_;
    QLineEdit* baseEdit_;
    QLineEdit* targetEdit_;
    QLabel* prLabel_;
    QLineEdit* linkEdit_;
    GitHubClient* github_;
    QLineEdit* oldDirEdit_;
    QLineEdit* newDirEdit_;
    QLabel* error_;
    QPushButton* ok_;
    QTabWidget* tabs_;
    QWidget* sessionsPage_;
    QTreeWidget* sessionList_;
    QPushButton* latestButton_;
    QVector<SessionRecord> sessions_;
};

} // namespace gui
