#pragma once

#include "CodeView.hpp"
#include "core/Session.hpp"

#include <QFuture>
#include <QHash>
#include <QMainWindow>
#include <atomic>
#include <memory>
#include <optional>

class QLabel;
class QLineEdit;
class QProgressBar;
class QTabWidget;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class QAction;
class QCheckBox;

namespace gui {

class DiffView;
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

    // "system", "light" or "dark": sets the application style/palette. Safe to call before
    // any window exists; MainWindow::setTheme also re-colors open views.
    static void applyAppTheme(const QString& mode);
    void setTheme(const QString& mode);

    // Test/demo hooks.
    void activateChangeByTitle(const QString& substring, bool compare);

signals:
    void reviewFinished();

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void closeEvent(QCloseEvent* e) override;

private:
    // Shared with background tasks, which keep it alive while they run.
    struct State {
        std::shared_ptr<cr::ReviewSession> session;
        std::shared_ptr<cr::ReviewResult> result;
        std::atomic<bool> cancel{false};
    };

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

    void populateFiles();
    void populateChanges();
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

    void connectCodeView(CodeView* v);
    void onDefinitionRequested(CodeView* v, int row, int column, bool declaration);
    void onHoverRequested(CodeView* v, int row, int column, QPoint globalPos);
    void navigateTo(cr::Side side, const cr::Location& loc, bool record = true);
    CodeView* openFileTab(cr::Side side, const QString& path, bool external);
    NavPoint currentPosition() const;
    void restore(const NavPoint& p);
    void goBack();
    void goForward();

    std::optional<cr::GitRepo> repo_;
    cr::Revision base_, target_;
    bool haveRevisions_ = false;
    std::shared_ptr<State> state_;
    QFuture<void> indexFuture_;
    QString compileDbOverride_;
    QHash<QString, std::vector<std::string>> lineCache_;

    QToolButton* baseButton_ = nullptr;
    QToolButton* targetButton_ = nullptr;
    QTreeWidget* files_ = nullptr;
    QTreeWidget* changes_ = nullptr;
    QLineEdit* changeFilter_ = nullptr;
    QCheckBox* hideTrivial_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    DiffView* diff_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* dbLabel_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QAction* ignoreWhitespace_ = nullptr;
    QAction* semantic_ = nullptr;
    QAction* backAction_ = nullptr;
    QAction* forwardAction_ = nullptr;
    int currentFile_ = -1;
    QString highlightOld_, highlightNew_;

    QVector<NavPoint> back_, forward_;
    bool restoring_ = false;
};

} // namespace gui
