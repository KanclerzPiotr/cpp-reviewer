#pragma once

#include "Bookmarks.hpp"
#include "QuickOpen.hpp"
#include "Sessions.hpp"
#include "CodeView.hpp"
#include "core/PrCompare.hpp"
#include "core/Reviewed.hpp"
#include "core/Session.hpp"

#include <QFuture>
#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

class QLabel;
class QMenu;
class QLineEdit;
class QProgressBar;
class QTabWidget;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class QAction;
class QDockWidget;
class QCheckBox;
class QComboBox;

namespace gui {

class DiffView;
class FindBar;
struct PullRequestChoice;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

    bool openRepository(const QString& path);
    void setRevisions(cr::Revision base, cr::Revision target, bool start = true);
    void compareDirectories(const QString& oldDir, const QString& newDir);
    void reviewPullRequest(const PullRequestChoice& pr);
    void startReview();
    // Asks what to compare (repository, revisions, PR or directories) and starts the review.
    // Returns false if the user cancelled.
    bool newComparison();
    // Compares two pull requests; `mapping` ("A-prefix=B-prefix") may be empty to guess it.
    void comparePullRequests(const QString& linkA, const QString& linkB, const QString& mapping);
    // Reopens a saved review; with `latest`, pull requests are fetched again.
    void resumeSession(const SessionRecord& record, bool latest);
    void saveCurrentSession();

    // "system", "light" or "dark": sets the application style/palette. Safe to call before
    // any window exists; MainWindow::setTheme also re-colors open views.
    static void applyAppTheme(const QString& mode);
    void setTheme(const QString& mode);

    // Diff of `basePath` in the base revision against `targetPath` in the target, with semantic
    // analysis of that pair (e.g. one part of a file that was split in two).
    void compareFiles(const QString& basePath, const QString& targetPath);

    // Test/demo hooks.
    void activateChangeByTitle(const QString& substring, bool compare);

signals:
    void reviewFinished();

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void closeEvent(QCloseEvent* e) override;

private:
    // Two pull requests compared with each other (e.g. the same change in two repositories).
    struct PrComparison {
        cr::ResolvedPullRequest a, b;
        cr::PathMapping mapping;
        std::vector<cr::FilePair> pairs; // point into a.files / b.files
        std::shared_ptr<cr::ReviewResult> interdiff, finalFiles;
    };
    struct PrRequest {
        QString linkA, linkB, mapping;
        // Commits of a resumed session: reused instead of fetching when still there.
        std::optional<SessionRecord::PrSide> storedA, storedB;
    };

    // Shared with background tasks, which keep it alive while they run.
    struct State {
        std::shared_ptr<cr::ReviewSession> session;
        std::shared_ptr<cr::ReviewResult> result;
        std::atomic<bool> cancel{false};
        std::shared_ptr<PrComparison> prs; // when comparing two pull requests
        bool indexing = false;             // the symbol index has been started
        // Every file of each revision, listed on first use (quick open, search).
        std::mutex filesMutex;
        std::shared_ptr<const std::vector<std::string>> allFiles[2];
    };
    static std::shared_ptr<const std::vector<std::string>> filesOf(State& st, cr::Side side);

    struct NavPoint {
        QWidget* tab = nullptr;
        int file = -1;
        cr::Side side = cr::Side::New;
        QString path;
        int line = 0; // 0-based
        bool valid() const { return tab != nullptr; }
    };

    void buildUi();
    void buildMenus();
    void updateRevisionButtons();
    void updateTitle();
    void pickRevision(cr::Side side);
    void pickPullRequest();
    void applyPreset(const QString& preset);
    void chooseCompileDatabase();
    void showProgress(const QString& stage, int done, int total);
    void finishProgress(const QString& message);
    void startIndexing(const std::shared_ptr<State>& st);
    std::shared_ptr<State> beginReview();
    void clearResultView();
    void showResult(const std::shared_ptr<State>& st);
    void startPullRequestComparison();
    void setPrView(bool finalFiles);
    void updatePrViewActions();
    std::optional<SessionRecord> currentSessionRecord() const;
    bool applyPendingRestore(); // true if it started something that shows another result first

    void populateFiles();
    void populateChanges();
    void updateChangesIndentation();
    void filterChanges();
    void showFile(int index);
    QString fileTitle(const cr::FileDiff& fd) const;
    ChangeMeta changeMeta(int change) const;

    void selectChange(int change);
    void addRelatedItems(QTreeWidgetItem* parent, int change);
    void showChangeDetails(int change);
    void gotoRelated(int change, int index);
    void setWordHighlight(const QString& oldWord, const QString& newWord);
    void openComparison(const cr::Location& oldLoc, const cr::Location& newLoc, const QString& heading,
                        const QString& tabTitle);
    void openChangeComparison(int change);
    std::vector<std::string> fileLines(cr::Side side, const QString& path);

    // Review marks: hunks and semantic changes the user has checked.
    bool changeReviewed(int change) const;
    bool hunkReviewed(int file, const cr::Hunk& h) const;
    void refreshReviewMarks();
    void onChangeItemChanged(QTreeWidgetItem* it);

    void connectCodeView(CodeView* v);
    void onDefinitionRequested(CodeView* v, int row, int column, bool declaration);
    void onHoverRequested(CodeView* v, int row, int column, QPoint globalPos);
    void navigateTo(cr::Side side, const cr::Location& loc, bool record = true);
    CodeView* openFileTab(cr::Side side, const QString& path, bool external);
    NavPoint currentPosition() const;
    void restore(const NavPoint& p);
    void goBack();
    void goForward();
    void pushHistory(const NavPoint& before);
    void openWholeFile(cr::Side side, const QString& path, int line);

    // Search.
    CodeView* activeCodeView() const;
    void applySearch();
    void findInView(bool backward);
    void closeFindBar();
    void openQuickOpen();
    QuickOpenDialog::FilesFn fileListProvider();
    void pickFilesToCompare();
    void showFilesMenu(QPoint pos);
    void showChangesMenu(QPoint pos);
    QString changeItemText(QTreeWidgetItem* it, bool details) const;
    void copyChanges(bool details);
    void addCopyPathActions(QMenu& menu, const QList<QTreeWidgetItem*>& items,
                            const std::function<const cr::FileDiff*(QTreeWidgetItem*)>& fileOf);
    void startFileSearch();

    // Bookmarks.
    void onBookmarkRequested(CodeView* v, int line, BookmarkAction action);
    void bookmarkAtCursor(BookmarkAction action);
    void editBookmarkComment(int index);
    void populateBookmarks();
    void gotoBookmark(int index);
    void stepBookmark(bool forward);

    std::optional<cr::GitRepo> repo_;
    cr::Revision base_, target_;
    bool haveRevisions_ = false;
    std::shared_ptr<State> state_;
    QFuture<void> indexFuture_;
    QString compileDbOverride_;
    QHash<QString, std::vector<std::string>> lineCache_;
    cr::ReviewedStore reviewed_;
    std::vector<uint64_t> changeKeys_;             // per semantic change
    std::vector<std::vector<cr::Hunk>> fileHunks_; // per file
    std::vector<int> reviewedChanges_;             // changes checked as reviewed

    QToolButton* baseButton_ = nullptr;
    QToolButton* targetButton_ = nullptr;
    QTreeWidget* files_ = nullptr;
    QTreeWidget* changes_ = nullptr;
    QLineEdit* changeFilter_ = nullptr;
    QCheckBox* hideTrivial_ = nullptr;
    QCheckBox* hideReviewed_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    DiffView* diff_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* dbLabel_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QAction* ignoreWhitespace_ = nullptr;
    QAction* semantic_ = nullptr;
    QAction* interdiffAction_ = nullptr;
    QAction* finalFilesAction_ = nullptr;
    std::optional<PrRequest> prRequest_;
    QString currentPrLink_;                     // the pull request the current revisions came from
    std::optional<SessionRecord> pendingRestore_; // position etc. to restore once the review is shown
    QAction* backAction_ = nullptr;
    QAction* forwardAction_ = nullptr;
    int currentFile_ = -1;
    QString highlightOld_, highlightNew_;

    FindBar* findBar_ = nullptr;
    QPointer<CodeView> lastCodeView_; // last code view with focus, for find and bookmarks
    QLineEdit* searchEdit_ = nullptr;
    QComboBox* searchScope_ = nullptr;
    QCheckBox* searchCase_ = nullptr;
    QCheckBox* searchWord_ = nullptr;
    QCheckBox* searchRegex_ = nullptr;
    QTreeWidget* searchResults_ = nullptr;
    QLabel* searchInfo_ = nullptr;
    QDockWidget* searchDock_ = nullptr;
    std::shared_ptr<std::atomic<bool>> searchCancel_;
    BookmarkStore bookmarks_;
    QTreeWidget* bookmarkList_ = nullptr;
    QDockWidget* bookmarksDock_ = nullptr;
    int bookmarkCursor_ = -1;

    QVector<NavPoint> back_, forward_;
    bool restoring_ = false;
};

} // namespace gui
