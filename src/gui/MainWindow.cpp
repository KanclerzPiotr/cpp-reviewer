#include "MainWindow.hpp"

#include "CompareDialog.hpp"
#include "DiffView.hpp"
#include "FileSearch.hpp"
#include "FindBar.hpp"
#include "QuickOpen.hpp"
#include "RevisionDialog.hpp"
#include "Theme.hpp"
#include "core/Diff.hpp"

#include <QActionGroup>
#include <QApplication>
#include <QStyle>
#include <QStyleFactory>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QInputDialog>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QTreeWidgetItemIterator>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTextBlock>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace gui {

namespace {

QString q(const std::string& s)
{
    return QString::fromStdString(s);
}

QString esc(const QString& s)
{
    return s.toHtmlEscaped();
}

constexpr int RoleChange = Qt::UserRole + 1;
constexpr int RoleFile = Qt::UserRole + 2;
constexpr int RoleOccurrence = Qt::UserRole + 3;
constexpr int RoleLabel = Qt::UserRole + 4; // text of an item before review progress is added


// Order in which change groups are listed: refactorings first, plain edits last.
const cr::ChangeKind kGroupOrder[] = {
    cr::ChangeKind::SignatureChanged, cr::ChangeKind::Renamed,   cr::ChangeKind::SymbolRenamed,
    cr::ChangeKind::Moved,            cr::ChangeKind::Reordered, cr::ChangeKind::Extracted,
    cr::ChangeKind::Inlined,          cr::ChangeKind::MovedCode, cr::ChangeKind::CopiedCode,
    cr::ChangeKind::MovedLines,       cr::ChangeKind::Modified,  cr::ChangeKind::Added,
    cr::ChangeKind::Removed,
};

QString groupName(cr::ChangeKind k)
{
    using K = cr::ChangeKind;
    switch (k) {
    case K::SignatureChanged: return QObject::tr("Signature changes");
    case K::Renamed: return QObject::tr("Renamed entities");
    case K::SymbolRenamed: return QObject::tr("Renamed identifiers");
    case K::Moved: return QObject::tr("Moved entities");
    case K::Reordered: return QObject::tr("Reordered within file");
    case K::Extracted: return QObject::tr("Extracted functions");
    case K::Inlined: return QObject::tr("Inlined functions");
    case K::MovedCode: return QObject::tr("Code moved between functions");
    case K::CopiedCode: return QObject::tr("Duplicated code");
    case K::MovedLines: return QObject::tr("Moved lines");
    case K::Modified: return QObject::tr("Modified");
    case K::Added: return QObject::tr("Added");
    case K::Removed: return QObject::tr("Removed");
    }
    return {};
}

QString locText(const cr::Location& l)
{
    if (!l.valid())
        return QStringLiteral("—");
    QString s = q(l.file) + QLatin1Char(':') + QString::number(l.line);
    if (l.endLine > l.line)
        s += QLatin1Char('-') + QString::number(l.endLine);
    return s;
}

} // namespace

MainWindow::MainWindow()
{
    setWindowTitle(tr("C++ Reviewer"));
    resize(1500, 900);
    buildUi();
    buildMenus();
    QSettings settings;
    restoreGeometry(settings.value(QStringLiteral("geometry")).toByteArray());
    // Layouts saved before the Search and Bookmarks docks existed leave them stacked under the
    // other docks; tab them with the semantic changes once.
    constexpr int kLayoutVersion = 2;
    restoreState(settings.value(QStringLiteral("windowState")).toByteArray());
    if (settings.value(QStringLiteral("layoutVersion"), 1).toInt() < kLayoutVersion) {
        auto* changesDock = findChild<QDockWidget*>(QStringLiteral("changesDock"));
        for (auto* dock : {searchDock_, bookmarksDock_})
            tabifyDockWidget(changesDock, dock);
        changesDock->raise();
        settings.setValue(QStringLiteral("layoutVersion"), kLayoutVersion);
    }
    updateRevisionButtons();
}

MainWindow::~MainWindow()
{
    if (state_)
        state_->cancel = true;
    indexFuture_.waitForFinished();
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    saveCurrentSession();
    QSettings settings;
    settings.setValue(QStringLiteral("geometry"), saveGeometry());
    settings.setValue(QStringLiteral("windowState"), saveState());
    QMainWindow::closeEvent(e);
}

// ------------------------------------------------------------------------------------------ UI

void MainWindow::buildUi()
{
    auto* tb = addToolBar(tr("Review"));
    tb->setObjectName(QStringLiteral("reviewToolbar"));
    tb->setToolButtonStyle(Qt::ToolButtonTextOnly);
    tb->setMovable(false);

    tb->addAction(tr("New…"), this, &MainWindow::newComparison)->setToolTip(tr("New comparison (Ctrl+N)"));
    tb->addSeparator();
    tb->addWidget(new QLabel(tr(" Base: ")));
    baseButton_ = new QToolButton;
    baseButton_->setToolTip(tr("Choose the base revision (the \"before\" side)"));
    connect(baseButton_, &QToolButton::clicked, this, [this] { pickRevision(cr::Side::Old); });
    tb->addWidget(baseButton_);
    tb->addAction(QStringLiteral("⇄"), this, [this] {
        if (!haveRevisions_)
            return;
        setRevisions(target_, base_);
    })->setToolTip(tr("Swap base and target"));
    tb->addWidget(new QLabel(tr(" Target: ")));
    targetButton_ = new QToolButton;
    targetButton_->setToolTip(tr("Choose the target revision (the \"after\" side)"));
    connect(targetButton_, &QToolButton::clicked, this, [this] { pickRevision(cr::Side::New); });
    tb->addWidget(targetButton_);
    tb->addAction(tr("Compare"), this, &MainWindow::startReview)->setShortcut(QKeySequence::Refresh);

    auto* presets = new QToolButton;
    presets->setText(tr("Presets"));
    presets->setPopupMode(QToolButton::InstantPopup);
    auto* pm = new QMenu(presets);
    pm->addAction(tr("Uncommitted changes (HEAD → working tree)"), this, [this] { applyPreset("worktree"); });
    pm->addAction(tr("Staged changes (HEAD → index)"), this, [this] { applyPreset("index"); });
    pm->addAction(tr("Unstaged changes (index → working tree)"), this, [this] { applyPreset("unstaged"); });
    pm->addAction(tr("Last commit (HEAD~1 → HEAD)"), this, [this] { applyPreset("last"); });
    pm->addAction(tr("Current branch vs. its merge base with the default branch"), this,
                  [this] { applyPreset("branch"); });
    pm->addSeparator();
    pm->addAction(tr("GitHub Pull Request…"), this, &MainWindow::pickPullRequest);
    presets->setMenu(pm);
    tb->addWidget(presets);

    // Comparing two pull requests: their patches, or the files after each.
    auto* prView = new QActionGroup(this);
    interdiffAction_ = tb->addAction(tr("Interdiff"), this, [this] { setPrView(false); });
    interdiffAction_->setToolTip(tr("The two pull requests' changes side by side: differences mean they don't do the same"));
    finalFilesAction_ = tb->addAction(tr("Final Files"), this, [this] { setPrView(true); });
    finalFilesAction_->setToolTip(tr("The files after each pull request, with semantic analysis and navigation"));
    for (auto* a : {interdiffAction_, finalFilesAction_}) {
        a->setCheckable(true);
        a->setVisible(false);
        prView->addAction(a);
    }

    tb->addSeparator();
    backAction_ = tb->addAction(QStringLiteral("◀"), this, &MainWindow::goBack);
    backAction_->setToolTip(tr("Navigate back (Alt+Left)"));
    backAction_->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Left));
    forwardAction_ = tb->addAction(QStringLiteral("▶"), this, &MainWindow::goForward);
    forwardAction_->setToolTip(tr("Navigate forward (Alt+Right)"));
    forwardAction_->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Right));
    backAction_->setEnabled(false);
    forwardAction_->setEnabled(false);

    // Central area: the diff of the selected file plus closable tabs for navigation.
    tabs_ = new QTabWidget;
    tabs_->setDocumentMode(true);
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    diff_ = new DiffView;
    diff_->setChangeMetaProvider([this](int c) { return changeMeta(c); });
    tabs_->addTab(diff_, tr("Diff"));
    tabs_->tabBar()->setTabButton(0, QTabBar::RightSide, nullptr);
    tabs_->tabBar()->setTabButton(0, QTabBar::LeftSide, nullptr);
    connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int i) {
        auto* w = tabs_->widget(i);
        if (w == diff_)
            return;
        for (auto* stack : {&back_, &forward_})
            stack->erase(std::remove_if(stack->begin(), stack->end(), [w](const NavPoint& p) { return p.tab == w; }),
                         stack->end());
        tabs_->removeTab(i);
        w->deleteLater();
        backAction_->setEnabled(!back_.isEmpty());
        forwardAction_->setEnabled(!forward_.isEmpty());
    });
    auto* central = new QWidget;
    auto* centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    centralLayout->addWidget(tabs_, 1);
    findBar_ = new FindBar;
    centralLayout->addWidget(findBar_);
    setCentralWidget(central);
    connect(findBar_, &FindBar::searchChanged, this, &MainWindow::applySearch);
    connect(findBar_, &FindBar::findRequested, this, &MainWindow::findInView);
    connect(findBar_, &FindBar::closed, this, &MainWindow::closeFindBar);
    connect(tabs_, &QTabWidget::currentChanged, this, [this] {
        if (findBar_->isVisible())
            applySearch();
    });
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        if (auto* v = qobject_cast<CodeView*>(now))
            lastCodeView_ = v;
    });
    CodeView::setBookmarkProvider([this](cr::Side side, const QString& path, int line) {
        return bookmarks_.commentAt(QFileInfo(path).isAbsolute() ? cr::Side::New : side, path, line);
    });
    connect(diff_, &DiffView::definitionRequested, this, &MainWindow::onDefinitionRequested);
    connect(diff_, &DiffView::hoverRequested, this, &MainWindow::onHoverRequested);
    connect(diff_, &DiffView::changeActivated, this, &MainWindow::openChangeComparison);
    connect(diff_, &DiffView::bookmarkRequested, this, &MainWindow::onBookmarkRequested);
    connect(diff_, &DiffView::openFileRequested, this, &MainWindow::openWholeFile);
    diff_->setReviewedProvider([this](const cr::Hunk& h) { return hunkReviewed(currentFile_, h); });
    connect(diff_, &DiffView::hunkReviewToggled, this, [this](quint64 key, bool on) {
        reviewed_.set(key, on);
        refreshReviewMarks();
    });

    // Files dock.
    files_ = new QTreeWidget;
    files_->setHeaderLabels({tr(""), tr("File"), tr("+/−"), tr("Δ")});
    files_->setSelectionMode(QAbstractItemView::ExtendedSelection); // two files can be compared
    files_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(files_, &QTreeWidget::customContextMenuRequested, this, &MainWindow::showFilesMenu);
    files_->setRootIsDecorated(false);
    files_->setUniformRowHeights(true);
    files_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    files_->header()->setStretchLastSection(false);
    files_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    files_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    files_->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    connect(files_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* it) {
        if (it && !restoring_) {
            auto before = currentPosition();
            showFile(it->data(0, RoleFile).toInt());
            tabs_->setCurrentWidget(diff_);
            if (before.valid() && (before.tab != diff_ || before.file != currentFile_)) {
                back_.push_back(before);
                forward_.clear();
                backAction_->setEnabled(true);
                forwardAction_->setEnabled(false);
            }
        }
    });
    auto* filesDock = new QDockWidget(tr("Changed Files"), this);
    filesDock->setObjectName(QStringLiteral("filesDock"));
    filesDock->setWidget(files_);
    addDockWidget(Qt::LeftDockWidgetArea, filesDock);

    // Semantic changes dock.
    auto* changesWidget = new QWidget;
    auto* cl = new QVBoxLayout(changesWidget);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(2);
    auto* filterRow = new QHBoxLayout;
    changeFilter_ = new QLineEdit;
    changeFilter_->setPlaceholderText(tr("Filter changes…"));
    changeFilter_->setClearButtonEnabled(true);
    hideTrivial_ = new QCheckBox(tr("Hide rename-only"));
    hideTrivial_->setToolTip(tr("Hide modifications that consist only of identifier renames"));
    hideReviewed_ = new QCheckBox(tr("Hide reviewed"));
    hideReviewed_->setToolTip(tr("Hide changes you have checked as reviewed"));
    filterRow->addWidget(changeFilter_, 1);
    filterRow->addWidget(hideTrivial_);
    filterRow->addWidget(hideReviewed_);
    cl->addLayout(filterRow);
    changes_ = new QTreeWidget;
    changes_->setHeaderHidden(true);
    updateChangesIndentation();
    changes_->setUniformRowHeights(true);
    changes_->setToolTip(tr("Click to show, double-click to compare with the original code"));
    changes_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    changes_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(changes_, &QTreeWidget::customContextMenuRequested, this, &MainWindow::showChangesMenu);
    auto* copyChange = new QAction(changes_);
    copyChange->setShortcut(QKeySequence::Copy);
    copyChange->setShortcutContext(Qt::WidgetShortcut);
    connect(copyChange, &QAction::triggered, this, [this] { copyChanges(false); });
    changes_->addAction(copyChange);
    auto* copyDetails = new QAction(changes_);
    copyDetails->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    copyDetails->setShortcutContext(Qt::WidgetShortcut);
    connect(copyDetails, &QAction::triggered, this, [this] { copyChanges(true); });
    changes_->addAction(copyDetails);
    cl->addWidget(changes_, 1);
    connect(changeFilter_, &QLineEdit::textChanged, this, &MainWindow::filterChanges);
    connect(hideTrivial_, &QCheckBox::toggled, this, &MainWindow::filterChanges);
    connect(hideReviewed_, &QCheckBox::toggled, this, &MainWindow::filterChanges);
    connect(changes_, &QTreeWidget::itemChanged, this, &MainWindow::onChangeItemChanged);
    connect(changes_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* it) {
        bool ok = false;
        int c = it->data(0, RoleChange).toInt(&ok);
        if (!ok || c < 0)
            return;
        bool isOcc = false;
        int occ = it->data(0, RoleOccurrence).toInt(&isOcc);
        if (isOcc)
            gotoRelated(c, occ);
        else
            showChangeDetails(c);
    });
    connect(changes_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* it) {
        bool ok = false;
        int c = it->data(0, RoleChange).toInt(&ok);
        if (!ok || c < 0)
            return;
        bool isOcc = false;
        int occ = it->data(0, RoleOccurrence).toInt(&isOcc);
        if (!isOcc) {
            openChangeComparison(c);
            return;
        }
        // A related item with a range on both sides (extracted code, a hunk...) can be compared.
        const auto& ch = state_->result->changes[static_cast<size_t>(c)];
        const auto& item = ch.related[static_cast<size_t>(occ)];
        if (item.oldLoc.valid() && item.newLoc.valid() &&
            (item.oldLoc.endLine > item.oldLoc.line || item.newLoc.endLine > item.newLoc.line))
            openComparison(item.oldLoc, item.newLoc, q(ch.title) + QStringLiteral(" — ") + q(item.label),
                           Theme::badge(ch.kind) + QLatin1Char(' ') + q(item.label));
        else
            gotoRelated(c, occ);
    });
    auto* changesDock = new QDockWidget(tr("Semantic Changes"), this);
    changesDock->setObjectName(QStringLiteral("changesDock"));
    changesDock->setWidget(changesWidget);
    addDockWidget(Qt::LeftDockWidgetArea, changesDock);
    splitDockWidget(filesDock, changesDock, Qt::Vertical);

    // Search in files dock.
    auto* searchWidget = new QWidget;
    auto* sl = new QVBoxLayout(searchWidget);
    sl->setContentsMargins(0, 0, 0, 0);
    sl->setSpacing(2);
    auto* searchRow = new QHBoxLayout;
    searchEdit_ = new QLineEdit;
    searchEdit_->setPlaceholderText(tr("Search in files… (Enter)"));
    searchEdit_->setClearButtonEnabled(true);
    searchScope_ = new QComboBox;
    searchScope_->addItems({tr("Changed files"), tr("Whole target"), tr("Whole base")});
    searchScope_->setToolTip(tr("Changed files: their target version (deleted files: their base version)"));
    searchRow->addWidget(searchEdit_, 1);
    searchRow->addWidget(searchScope_);
    sl->addLayout(searchRow);
    auto* optRow = new QHBoxLayout;
    searchCase_ = new QCheckBox(tr("Case"));
    searchWord_ = new QCheckBox(tr("Word"));
    searchRegex_ = new QCheckBox(tr("Regex"));
    searchInfo_ = new QLabel;
    optRow->addWidget(searchCase_);
    optRow->addWidget(searchWord_);
    optRow->addWidget(searchRegex_);
    optRow->addWidget(searchInfo_, 1);
    sl->addLayout(optRow);
    searchResults_ = new QTreeWidget;
    searchResults_->setHeaderHidden(true);
    searchResults_->setUniformRowHeights(true);
    sl->addWidget(searchResults_, 1);
    connect(searchEdit_, &QLineEdit::returnPressed, this, &MainWindow::startFileSearch);
    connect(searchResults_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* it) {
        const int line = it->data(0, RoleOccurrence).toInt();
        if (line <= 0)
            return;
        cr::Location loc;
        loc.file = it->data(0, RoleFile).toString().toStdString();
        loc.line = line;
        navigateTo(static_cast<cr::Side>(it->data(0, RoleChange).toInt()), loc);
    });
    connect(searchResults_, &QTreeWidget::itemClicked, searchResults_, &QTreeWidget::itemActivated);
    searchDock_ = new QDockWidget(tr("Search"), this);
    searchDock_->setObjectName(QStringLiteral("searchDock"));
    searchDock_->setWidget(searchWidget);
    addDockWidget(Qt::LeftDockWidgetArea, searchDock_);
    tabifyDockWidget(changesDock, searchDock_);

    // Bookmarks dock.
    auto* bookmarksWidget = new QWidget;
    auto* bl = new QVBoxLayout(bookmarksWidget);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(2);
    bookmarkList_ = new QTreeWidget;
    bookmarkList_->setHeaderLabels({tr("Location"), tr("Comment"), tr("Code")});
    bookmarkList_->setRootIsDecorated(false);
    bookmarkList_->setUniformRowHeights(true);
    bookmarkList_->setContextMenuPolicy(Qt::CustomContextMenu);
    bookmarkList_->setToolTip(tr("Ctrl+K: bookmark the current line · Ctrl+Shift+K: comment · F2 / Shift+F2: next / previous"));
    bl->addWidget(bookmarkList_, 1);
    auto* bookmarkButtons = new QHBoxLayout;
    auto* copyMd = new QToolButton;
    copyMd->setText(tr("Copy as Markdown"));
    copyMd->setToolTip(tr("Copy every bookmark with its code line and comment, e.g. for a PR review"));
    bookmarkButtons->addWidget(copyMd);
    bookmarkButtons->addStretch(1);
    bl->addLayout(bookmarkButtons);
    connect(copyMd, &QToolButton::clicked, this, [this] {
        QApplication::clipboard()->setText(bookmarks_.toMarkdown());
        statusLabel_->setText(tr("Copied %n bookmark(s) as Markdown", nullptr, static_cast<int>(bookmarks_.items().size())));
    });
    connect(bookmarkList_, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem* it) { gotoBookmark(it->data(0, RoleOccurrence).toInt()); });
    connect(bookmarkList_, &QTreeWidget::itemClicked, bookmarkList_, &QTreeWidget::itemActivated);
    connect(bookmarkList_, &QTreeWidget::customContextMenuRequested, this, [this](QPoint pos) {
        auto* it = bookmarkList_->itemAt(pos);
        if (!it)
            return;
        const int index = it->data(0, RoleOccurrence).toInt();
        QMenu menu;
        menu.addAction(tr("Go to Bookmark"), this, [this, index] { gotoBookmark(index); });
        menu.addAction(tr("Edit Comment…"), this, [this, index] { editBookmarkComment(index); });
        menu.addAction(tr("Remove Bookmark"), this, [this, index] {
            bookmarks_.remove(index);
            populateBookmarks();
        });
        menu.exec(bookmarkList_->viewport()->mapToGlobal(pos));
    });
    bookmarksDock_ = new QDockWidget(tr("Bookmarks"), this);
    bookmarksDock_->setObjectName(QStringLiteral("bookmarksDock"));
    bookmarksDock_->setWidget(bookmarksWidget);
    addDockWidget(Qt::LeftDockWidgetArea, bookmarksDock_);
    tabifyDockWidget(changesDock, bookmarksDock_);
    changesDock->raise();

    statusLabel_ = new QLabel;
    progress_ = new QProgressBar;
    progress_->setMaximumWidth(220);
    progress_->setVisible(false);
    dbLabel_ = new QLabel;
    statusBar()->addWidget(statusLabel_, 1);
    statusBar()->addPermanentWidget(progress_);
    statusBar()->addPermanentWidget(dbLabel_);
}

void MainWindow::buildMenus()
{
    auto* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&New Comparison…"), QKeySequence::New, this, &MainWindow::newComparison);
    file->addAction(tr("&Open Repository…"), QKeySequence::Open, this, [this] {
        auto dir = QFileDialog::getExistingDirectory(this, tr("Open Git Repository"));
        if (!dir.isEmpty() && openRepository(dir))
            startReview();
    });
    file->addAction(tr("Compare &Directories…"), this, [this] {
        auto a = QFileDialog::getExistingDirectory(this, tr("Old Directory (base)"));
        if (a.isEmpty())
            return;
        auto b = QFileDialog::getExistingDirectory(this, tr("New Directory (target)"));
        if (!b.isEmpty())
            compareDirectories(a, b);
    });
    file->addSeparator();
    file->addAction(tr("&Quit"), QKeySequence::Quit, qApp, &QApplication::quit);

    auto* review = menuBar()->addMenu(tr("&Review"));
    review->addAction(tr("Choose &Base Revision…"), QKeySequence(Qt::CTRL | Qt::Key_B), this,
                      [this] { pickRevision(cr::Side::Old); });
    review->addAction(tr("Choose &Target Revision…"), QKeySequence(Qt::CTRL | Qt::Key_T), this,
                      [this] { pickRevision(cr::Side::New); });
    review->addAction(tr("GitHub &Pull Request…"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P), this,
                      &MainWindow::pickPullRequest);
    review->addAction(tr("&Refresh"), QKeySequence::Refresh, this, &MainWindow::startReview);
    review->addSeparator();
    ignoreWhitespace_ = review->addAction(tr("Ignore &Whitespace"));
    ignoreWhitespace_->setCheckable(true);
    ignoreWhitespace_->setChecked(QSettings().value(QStringLiteral("ignoreWhitespace"), false).toBool());
    connect(ignoreWhitespace_, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("ignoreWhitespace"), on);
        startReview();
    });
    semantic_ = review->addAction(tr("&Semantic Analysis (libclang)"));
    semantic_->setCheckable(true);
    semantic_->setChecked(true);
    connect(semantic_, &QAction::toggled, this, &MainWindow::startReview);
    review->addAction(tr("Set &compile_commands.json…"), this, &MainWindow::chooseCompileDatabase);
    review->addSeparator();
    review->addAction(tr("Compare Two &Files…"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D), this,
                      &MainWindow::pickFilesToCompare);
    review->addAction(tr("&Mark Hunk as Reviewed, Go to Next"), QKeySequence(Qt::CTRL | Qt::Key_Return), this, [this] {
        if (tabs_->currentWidget() == diff_ && !diff_->markHunkAtCursorReviewed())
            statusLabel_->setText(tr("No unreviewed hunk at or below the cursor"));
    });
    auto* showReviewed = review->addAction(tr("Show Re&viewed Hunks"));
    showReviewed->setCheckable(true);
    showReviewed->setToolTip(tr("Show hunks marked as reviewed instead of folding them"));
    connect(showReviewed, &QAction::toggled, this, [this](bool on) { diff_->setFoldingEnabled(!on); });

    auto* edit = menuBar()->addMenu(tr("&Search"));
    edit->addAction(tr("&Find…"), QKeySequence::Find, this, [this] {
        auto* v = activeCodeView();
        findBar_->activate(v ? v->textCursor().selectedText() : QString());
    });
    edit->addAction(tr("Find &Next"), QKeySequence(Qt::Key_F3), this, [this] { findInView(false); });
    edit->addAction(tr("Find &Previous"), QKeySequence(Qt::SHIFT | Qt::Key_F3), this, [this] { findInView(true); });
    edit->addAction(tr("Find in &Files…"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F), this, [this] {
        searchDock_->show();
        searchDock_->raise();
        if (auto* v = activeCodeView(); v && v->textCursor().hasSelection())
            searchEdit_->setText(v->textCursor().selectedText());
        searchEdit_->setFocus();
        searchEdit_->selectAll();
    });
    edit->addSeparator();
    edit->addAction(tr("&Open File…"), QKeySequence(Qt::CTRL | Qt::Key_P), this, &MainWindow::openQuickOpen);

    auto* marks = menuBar()->addMenu(tr("&Bookmarks"));
    marks->addAction(tr("&Toggle Bookmark"), QKeySequence(Qt::CTRL | Qt::Key_K), this,
                     [this] { bookmarkAtCursor(BookmarkAction::Toggle); });
    marks->addAction(tr("Add/Edit &Comment…"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_K), this,
                     [this] { bookmarkAtCursor(BookmarkAction::EditComment); });
    marks->addAction(tr("&Next Bookmark"), QKeySequence(Qt::Key_F2), this, [this] { stepBookmark(true); });
    marks->addAction(tr("&Previous Bookmark"), QKeySequence(Qt::SHIFT | Qt::Key_F2), this, [this] { stepBookmark(false); });
    marks->addSeparator();
    marks->addAction(tr("Show &Bookmarks"), this, [this] {
        bookmarksDock_->show();
        bookmarksDock_->raise();
    });
    marks->addAction(tr("Copy All as &Markdown"), this, [this] {
        QApplication::clipboard()->setText(bookmarks_.toMarkdown());
        statusLabel_->setText(tr("Copied %n bookmark(s) as Markdown", nullptr, static_cast<int>(bookmarks_.items().size())));
    });

    auto* nav = menuBar()->addMenu(tr("&Navigate"));
    nav->addAction(backAction_);
    nav->addAction(forwardAction_);
    nav->addSeparator();
    nav->addAction(tr("&Next Change"), QKeySequence(Qt::Key_F8), this, [this] {
        if (auto* d = qobject_cast<DiffView*>(tabs_->currentWidget()))
            d->gotoHunk(true);
    });
    nav->addAction(tr("&Previous Change"), QKeySequence(Qt::SHIFT | Qt::Key_F8), this, [this] {
        if (auto* d = qobject_cast<DiffView*>(tabs_->currentWidget()))
            d->gotoHunk(false);
    });
    nav->addAction(tr("Next &File"), QKeySequence(Qt::ALT | Qt::Key_Down), this, [this] {
        int i = files_->indexOfTopLevelItem(files_->currentItem());
        if (i + 1 < files_->topLevelItemCount())
            files_->setCurrentItem(files_->topLevelItem(i + 1));
    });
    nav->addAction(tr("Previous F&ile"), QKeySequence(Qt::ALT | Qt::Key_Up), this, [this] {
        int i = files_->indexOfTopLevelItem(files_->currentItem());
        if (i > 0)
            files_->setCurrentItem(files_->topLevelItem(i - 1));
    });
    nav->addSeparator();
    nav->addAction(tr("Go to Definition\tF12 / Ctrl+Click"))->setEnabled(false);
    nav->addAction(tr("Go to Declaration\tCtrl+F12 / Ctrl+Shift+Click"))->setEnabled(false);

    auto* view = menuBar()->addMenu(tr("&View"));
    auto zoom = [this](int delta) {
        auto apply = [delta](CodeView* v) {
            QFont f = v->font();
            f.setPointSize(std::max(6, f.pointSize() + delta));
            v->setFont(f);
        };
        for (auto* v : findChildren<CodeView*>())
            apply(v);
    };
    auto* themeMenu = view->addMenu(tr("&Theme"));
    auto* themeGroup = new QActionGroup(themeMenu);
    const QString currentTheme = QSettings().value(QStringLiteral("theme"), QStringLiteral("system")).toString();
    for (auto [id, label] : {std::pair{"system", QT_TR_NOOP("&System")}, std::pair{"light", QT_TR_NOOP("&Light")},
                             std::pair{"dark", QT_TR_NOOP("&Dark")}}) {
        auto* a = themeMenu->addAction(tr(label));
        a->setCheckable(true);
        a->setChecked(currentTheme == QLatin1String(id));
        themeGroup->addAction(a);
        const QString mode = QLatin1String(id);
        connect(a, &QAction::triggered, this, [this, mode] { setTheme(mode); });
    }
    auto* contextMenu = view->addMenu(tr("Context in &Comparisons"));
    contextMenu->setToolTip(tr("Lines shown around a moved or extracted entity when comparing it with its origin"));
    auto* contextGroup = new QActionGroup(contextMenu);
    const int currentContext = QSettings().value(QStringLiteral("comparisonContext"), 10).toInt();
    for (int n : {0, 5, 10, 25, 50}) {
        auto* a = contextMenu->addAction(n == 0 ? tr("None") : tr("%n line(s)", nullptr, n));
        a->setCheckable(true);
        a->setChecked(n == currentContext);
        contextGroup->addAction(a);
        connect(a, &QAction::triggered, this, [n] { QSettings().setValue(QStringLiteral("comparisonContext"), n); });
    }
    view->addSeparator();
    view->addAction(tr("&Clear Highlights"), QKeySequence(Qt::Key_Escape), this, [this] {
        setWordHighlight({}, {});
        closeFindBar();
    });
    view->addAction(tr("Zoom &In"), QKeySequence::ZoomIn, this, [zoom] { zoom(1); });
    view->addAction(tr("Zoom &Out"), QKeySequence::ZoomOut, this, [zoom] { zoom(-1); });

    auto* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&About"), this, [this] {
        QMessageBox::about(this, tr("About C++ Reviewer"),
                           tr("<h3>C++ Reviewer</h3>"
                              "<p>Semantic code review for C++: side-by-side diffs enriched with libclang analysis "
                              "(moved, renamed, extracted and inlined code), plus go-to-definition across the "
                              "reviewed revisions.</p>"
                              "<p><b>Gutter badges:</b> R renamed, r identifier renamed, → moved, ↕ reordered, "
                              "X extracted, I inlined, ⇢ code moved between functions, ≡ moved lines, "
                              "S signature changed, M modified, A added, D removed, C duplicated.</p>"));
    });
}

// ------------------------------------------------------------------------------------ theme

void MainWindow::applyAppTheme(const QString& mode)
{
    // What the platform gave us, so "system" can go back to it.
    static const QString systemStyle = QApplication::style()->name();
    static const QPalette systemPalette = QApplication::palette();

    if (mode == QLatin1String("dark")) {
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
        QPalette p;
        const QColor window(0x1f, 0x23, 0x28), base(0x16, 0x1b, 0x22), alt(0x21, 0x26, 0x2d);
        const QColor text(0xe6, 0xed, 0xf3), dim(0x8b, 0x94, 0x9e), accent(0x2f, 0x81, 0xf7);
        p.setColor(QPalette::Window, window);
        p.setColor(QPalette::WindowText, text);
        p.setColor(QPalette::Base, base);
        p.setColor(QPalette::AlternateBase, alt);
        p.setColor(QPalette::ToolTipBase, alt);
        p.setColor(QPalette::ToolTipText, text);
        p.setColor(QPalette::PlaceholderText, dim);
        p.setColor(QPalette::Text, text);
        p.setColor(QPalette::Button, alt);
        p.setColor(QPalette::ButtonText, text);
        p.setColor(QPalette::BrightText, Qt::white);
        p.setColor(QPalette::Highlight, accent);
        p.setColor(QPalette::HighlightedText, Qt::white);
        p.setColor(QPalette::Link, QColor(0x58, 0xa6, 0xff));
        p.setColor(QPalette::Light, QColor(0x3a, 0x40, 0x48));
        p.setColor(QPalette::Midlight, QColor(0x30, 0x36, 0x3d));
        p.setColor(QPalette::Mid, QColor(0x26, 0x2c, 0x33));
        p.setColor(QPalette::Dark, QColor(0x10, 0x13, 0x17));
        p.setColor(QPalette::Shadow, Qt::black);
        for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
            p.setColor(QPalette::Disabled, role, QColor(0x6e, 0x76, 0x81));
        QApplication::setPalette(p);
    } else if (mode == QLatin1String("light")) {
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
        QApplication::setPalette(QApplication::style()->standardPalette());
    } else {
        QApplication::setStyle(QStyleFactory::create(systemStyle));
        QApplication::setPalette(systemPalette);
    }
    Theme::syncWithPalette();
}

// Changes have a checkbox before their text; without extra indentation their related places
// (hunks, calls...) would start at the same column and look like more changes.
void MainWindow::updateChangesIndentation()
{
    static const int base = changes_->indentation();
    changes_->setIndentation(base + changes_->style()->pixelMetric(QStyle::PM_IndicatorWidth) + 6);
}

void MainWindow::setTheme(const QString& mode)
{
    QSettings().setValue(QStringLiteral("theme"), mode);
    applyAppTheme(mode);
    updateChangesIndentation(); // the style may have changed
    for (auto* v : findChildren<CodeView*>())
        v->refreshTheme();
    if (state_ && state_->result) {
        // Colors are baked into the lists and the file header; rebuild them.
        auto* currentFileItem = files_->currentItem();
        const int file = currentFileItem ? currentFileItem->data(0, RoleFile).toInt() : -1;
        restoring_ = true;
        populateFiles();
        populateChanges();
        restoring_ = false;
        if (file >= 0 && file == currentFile_) {
            const int top = diff_->view(cr::Side::New)->verticalScrollBar()->value();
            showFile(file);
            diff_->view(cr::Side::New)->verticalScrollBar()->setValue(top);
        }
    }
}

// ------------------------------------------------------------------------------------ revisions

bool MainWindow::openRepository(const QString& path)
{
    auto repo = cr::GitRepo::open(path.toStdString());
    if (!repo) {
        QMessageBox::warning(this, tr("Not a Git Repository"), tr("%1 is not inside a git work tree.").arg(path));
        return false;
    }
    repo_ = std::move(repo);
    compileDbOverride_ = QSettings().value(QStringLiteral("compileDb/") + q(repo_->root())).toString();
    QSettings().setValue(QStringLiteral("lastRepository"), q(repo_->root()));

    // Default: uncommitted changes if there are any, otherwise the last commit.
    auto head = cr::parseRevisionSpec(*repo_, "HEAD");
    if (head && !repo_->diffWorkingTree(head->sha).empty()) {
        setRevisions(*head, cr::Revision::workingTree(), false);
    } else if (auto prev = cr::parseRevisionSpec(*repo_, "HEAD~1"); prev && head) {
        setRevisions(*prev, *head, false);
    } else if (head) {
        setRevisions(*head, cr::Revision::workingTree(), false);
    }
    return true;
}

bool MainWindow::newComparison()
{
    QString repo = repo_ ? q(repo_->root()) : QString();
    if (repo.isEmpty())
        repo = cr::GitRepo::open(QDir::currentPath().toStdString()) ? QDir::currentPath()
                                                                    : QSettings().value(QStringLiteral("lastRepository")).toString();
    CompareDialog dlg(repo, isVisible() ? this : nullptr);
    if (dlg.exec() != QDialog::Accepted)
        return false;
    saveCurrentSession();
    const auto c = dlg.choice();
    switch (c.kind) {
    case CompareChoice::Kind::Resume:
        resumeSession(c.session, c.latest);
        break;
    case CompareChoice::Kind::Directories:
        compareDirectories(c.oldDir, c.newDir);
        break;
    case CompareChoice::Kind::PullRequest:
        if (openRepository(c.repo))
            reviewPullRequest(c.pr);
        break;
    case CompareChoice::Kind::Revisions:
        if (openRepository(c.repo))
            setRevisions(c.base, c.target);
        break;
    case CompareChoice::Kind::PullRequests:
        if (!c.repo.isEmpty())
            openRepository(c.repo); // where to fetch a pull request of this repository
        comparePullRequests(c.linkA, c.linkB, c.mapping);
        break;
    }
    return true;
}

void MainWindow::setRevisions(cr::Revision base, cr::Revision target, bool start)
{
    prRequest_.reset();
    currentPrLink_.clear();
    base_ = std::move(base);
    target_ = std::move(target);
    haveRevisions_ = true;
    updateRevisionButtons();
    if (start)
        startReview();
}

void MainWindow::compareDirectories(const QString& oldDir, const QString& newDir)
{
    repo_.reset();
    setRevisions(cr::Revision::directory(oldDir.toStdString()), cr::Revision::directory(newDir.toStdString()));
}

void MainWindow::updateRevisionButtons()
{
    auto label = [](const cr::Revision& r) {
        QString s = q(r.display());
        return s.size() > 48 ? s.left(46) + QStringLiteral("…") : s;
    };
    baseButton_->setText(haveRevisions_ ? label(base_) : tr("(choose)"));
    targetButton_->setText(haveRevisions_ ? label(target_) : tr("(choose)"));
    baseButton_->setEnabled(repo_.has_value());
    targetButton_->setEnabled(repo_.has_value());
    updateTitle();
}

void MainWindow::updateTitle()
{
    QString t = tr("C++ Reviewer");
    if (repo_)
        t += QStringLiteral(" — ") + QFileInfo(q(repo_->root())).fileName();
    if (haveRevisions_)
        t += QStringLiteral(" — ") + q(base_.display()) + QStringLiteral(" → ") + q(target_.display());
    setWindowTitle(t);
}

void MainWindow::pickRevision(cr::Side side)
{
    if (!repo_)
        return;
    RevisionDialog dlg(*repo_, side == cr::Side::Old ? tr("Choose Base Revision") : tr("Choose Target Revision"), this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    auto rev = dlg.selectedRevision();
    if (!rev) {
        QMessageBox::warning(this, tr("Unknown Revision"), tr("The selected revision could not be resolved."));
        return;
    }
    if (side == cr::Side::Old)
        setRevisions(*rev, haveRevisions_ ? target_ : cr::Revision::workingTree());
    else
        setRevisions(haveRevisions_ ? base_ : *cr::parseRevisionSpec(*repo_, "HEAD"), *rev);
}

void MainWindow::applyPreset(const QString& preset)
{
    if (!repo_)
        return;
    auto head = cr::parseRevisionSpec(*repo_, "HEAD");
    if (!head)
        return;
    if (preset == "worktree") {
        setRevisions(*head, cr::Revision::workingTree());
    } else if (preset == "index") {
        setRevisions(*head, cr::Revision::index());
    } else if (preset == "unstaged") {
        setRevisions(cr::Revision::index(), cr::Revision::workingTree());
    } else if (preset == "last") {
        if (auto prev = cr::parseRevisionSpec(*repo_, "HEAD~1"))
            setRevisions(*prev, *head);
    } else if (preset == "branch") {
        std::string def;
        for (const auto& r : repo_->remotes()) {
            def = repo_->remoteDefaultBranch(r);
            if (!def.empty()) {
                def = r + "/" + def;
                break;
            }
        }
        if (def.empty())
            def = repo_->resolve("main").empty() ? "master" : "main";
        auto mb = repo_->mergeBase(def, "HEAD");
        if (mb.empty()) {
            QMessageBox::warning(this, tr("No Merge Base"), tr("Could not find a merge base with %1.").arg(q(def)));
            return;
        }
        setRevisions(cr::Revision::commit(mb, mb, "merge-base with " + def), *head);
    }
}

void MainWindow::pickPullRequest()
{
    if (!repo_)
        return;
    PullRequestDialog dlg(*repo_, this);
    if (dlg.exec() == QDialog::Accepted)
        reviewPullRequest(dlg.choice());
}

void MainWindow::reviewPullRequest(const PullRequestChoice& pr)
{
    if (!repo_)
        return;
    const QString source = pr.slug.isEmpty() ? pr.remote : pr.slug;
    showProgress(tr("Fetching pull request #%1 from %2…").arg(pr.number).arg(source), 0, 0);
    auto repo = *repo_;
    using Fetched = cr::GitRepo::FetchedPullRequest;
    auto* watcher = new QFutureWatcher<Fetched>(this);
    connect(watcher, &QFutureWatcher<Fetched>::finished, this, [this, watcher, pr] {
        watcher->deleteLater();
        auto f = watcher->result();
        if (!f.error.empty()) {
            finishProgress(tr("Fetching the pull request failed"));
            QMessageBox::warning(this, tr("Pull Request"), q(f.error));
            return;
        }
        auto title = QStringLiteral("PR #%1").arg(pr.number);
        const QString prTitle = pr.title.isEmpty() ? q(f.title) : pr.title;
        if (!prTitle.isEmpty())
            title += QStringLiteral(": ") + prTitle.left(40);
        setRevisions(cr::Revision::commit(f.base, f.base, f.baseLabel), cr::Revision::commit(f.head, f.head, title.toStdString()));
        // Remember the link, so the session can fetch the pull request again later.
        std::string host = pr.host.toStdString(), slug = pr.slug.toStdString();
        if (slug.empty() && repo_)
            cr::hostAndSlugFromUrl(pr.remote.contains(QStringLiteral("://")) ? pr.remote.toStdString()
                                                                             : repo_->remoteUrl(pr.remote.toStdString()),
                                   host, slug);
        if (!slug.empty())
            currentPrLink_ = QStringLiteral("https://%1/%2/pull/%3").arg(q(host), q(slug)).arg(pr.number);
    });
    watcher->setFuture(QtConcurrent::run([repo, pr]() {
        return repo.fetchPullRequestForReview(pr.remote.toStdString(), pr.number, pr.baseRef.toStdString());
    }));
}

void MainWindow::chooseCompileDatabase()
{
    auto path = QFileDialog::getOpenFileName(this, tr("compile_commands.json"),
                                             repo_ ? q(repo_->root()) : QString(),
                                             tr("Compilation database (compile_commands.json);;JSON (*.json)"));
    if (path.isEmpty())
        return;
    compileDbOverride_ = path;
    if (repo_)
        QSettings().setValue(QStringLiteral("compileDb/") + q(repo_->root()), path);
    startReview();
}

// ------------------------------------------------------------------------------------ review

void MainWindow::showProgress(const QString& stage, int done, int total)
{
    statusLabel_->setText(stage);
    progress_->setVisible(true);
    progress_->setRange(0, total);
    progress_->setValue(done);
}

void MainWindow::finishProgress(const QString& message)
{
    progress_->setVisible(false);
    statusLabel_->setText(message);
}

std::shared_ptr<MainWindow::State> MainWindow::beginReview()
{
    if (state_)
        state_->cancel = true;
    auto st = std::make_shared<State>();
    state_ = st;
    reviewed_ = cr::ReviewedStore(repo_ ? repo_->root() : target_.ref);
    bookmarks_.load(repo_ ? repo_->root() : target_.ref);
    lineCache_.clear();
    back_.clear();
    forward_.clear();
    backAction_->setEnabled(false);
    forwardAction_->setEnabled(false);
    clearResultView();
    diff_->showDiff(cr::FileDiff{}, tr("Computing review…"));
    updateTitle();
    return st;
}

void MainWindow::clearResultView()
{
    changeKeys_.clear();
    reviewedChanges_.clear();
    fileHunks_.clear();
    // Close navigation tabs: they belong to the previous revisions.
    for (int i = tabs_->count() - 1; i >= 0; --i) {
        if (tabs_->widget(i) != diff_) {
            auto* w = tabs_->widget(i);
            tabs_->removeTab(i);
            w->deleteLater();
        }
    }
    files_->clear();
    changes_->clear();
    currentFile_ = -1;
    setWordHighlight({}, {});
}

void MainWindow::showResult(const std::shared_ptr<State>& st)
{
    clearResultView();
    const auto dbPath = st->session ? q(st->session->compileDatabasePath()) : QString();
    dbLabel_->setVisible(st->session != nullptr);
    dbLabel_->setText(dbPath.isEmpty() ? tr("⚠ no compile_commands.json (fallback flags)")
                                       : tr("compile_commands: %1").arg(QFileInfo(dbPath).dir().dirName() +
                                                                          QStringLiteral("/") +
                                                                          QFileInfo(dbPath).fileName()));
    dbLabel_->setToolTip(dbPath.isEmpty()
                             ? tr("Without a compilation database includes and macros may not resolve. "
                                  "Configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON or set one in the Review menu.")
                             : dbPath);
    const bool interdiff = st->prs && st->result == st->prs->interdiff;
    if (interdiff)
        diff_->setSideCaptions(tr("Changes of %1").arg(q(st->prs->a.label)), tr("Changes of %1").arg(q(st->prs->b.label)));
    else
        diff_->setSideCaptions(tr("Base: %1").arg(q(base_.display())), tr("Target: %1").arg(q(target_.display())));
    for (const auto& c : st->result->changes)
        changeKeys_.push_back(cr::changeKey(*st->result, c));
    for (const auto& f : st->result->files)
        fileHunks_.push_back(cr::diffHunks(f));
    if (st->session) {
        bookmarks_.relocate([this](cr::Side side, const QString& path) { return fileLines(side, path); });
        populateBookmarks();
    }
    populateFiles();
    populateChanges();
    int changes = 0;
    for (const auto& c : st->result->changes)
        changes += c.trivial ? 0 : 1;
    if (interdiff) {
        int same = 0;
        for (const auto& f : st->result->files)
            same += f.change.status == '=' ? 1 : 0;
        finishProgress(tr("Interdiff: %1 of %n file(s) changed the same way", nullptr,
                          static_cast<int>(st->result->files.size()))
                           .arg(same));
    } else {
        finishProgress(tr("%n file(s) changed", nullptr, static_cast<int>(st->result->files.size())) +
                       tr(", %n semantic change(s)", nullptr, changes));
    }
    if (files_->topLevelItemCount() > 0)
        files_->setCurrentItem(files_->topLevelItem(0));
    else
        diff_->showDiff(cr::FileDiff{}, tr("No differences."));
    updatePrViewActions();
    if (st->session)
        startIndexing(st);
    if (pendingRestore_ && applyPendingRestore())
        return; // another result comes first (the final files of resumed pull requests)
    saveCurrentSession();
    emit reviewFinished();
}

void MainWindow::startReview()
{
    if (prRequest_) {
        startPullRequestComparison();
        return;
    }
    if (!haveRevisions_)
        return;
    auto st = beginReview();
    st->session = std::make_shared<cr::ReviewSession>(repo_, base_, target_);
    if (!compileDbOverride_.isEmpty())
        st->session->setCompileDatabasePath(compileDbOverride_.toStdString());

    cr::ReviewOptions options;
    options.ignoreWhitespace = ignoreWhitespace_->isChecked();
    options.semantic = semantic_->isChecked();

    QPointer<MainWindow> guard(this);
    auto post = [guard](const std::string& stage, int done, int total) {
        QMetaObject::invokeMethod(qApp, [guard, stage, done, total] {
            if (guard)
                guard->showProgress(q(stage), done, total);
        }, Qt::QueuedConnection);
    };
    showProgress(tr("Preparing…"), 0, 0);

    struct Outcome {
        QString error;
        std::shared_ptr<cr::ReviewResult> result;
    };
    auto* watcher = new QFutureWatcher<Outcome>(this);
    connect(watcher, &QFutureWatcher<Outcome>::finished, this, [this, watcher, st] {
        watcher->deleteLater();
        if (st != state_)
            return; // superseded by a newer review
        auto out = watcher->result();
        if (!out.error.isEmpty() || !out.result) {
            pendingRestore_.reset();
            finishProgress(tr("Review failed"));
            diff_->showDiff(cr::FileDiff{}, tr("<b>Review failed:</b> %1").arg(esc(out.error)));
            return;
        }
        st->result = out.result;
        showResult(st);
    });
    watcher->setFuture(QtConcurrent::run([st, options, post]() {
        Outcome out;
        std::string err;
        if (!st->session->prepare(&err, post)) {
            out.error = err.empty() ? tr("Could not materialize the revisions.") : q(err);
            return out;
        }
        out.result = std::make_shared<cr::ReviewResult>(st->session->run(options, post, &st->cancel));
        return out;
    }));
}

// ------------------------------------------------------------------------------------ two pull requests

void MainWindow::comparePullRequests(const QString& linkA, const QString& linkB, const QString& mapping)
{
    prRequest_ = PrRequest{linkA, linkB, mapping, std::nullopt, std::nullopt};
    startPullRequestComparison();
}

void MainWindow::startPullRequestComparison()
{
    if (!prRequest_)
        return;
    const auto req = *prRequest_;
    auto st = beginReview();
    showProgress(tr("Fetching the pull requests…"), 0, 0);
    auto local = repo_;
    const bool ignoreWs = ignoreWhitespace_->isChecked();

    struct Outcome {
        QString error;
        std::shared_ptr<PrComparison> prs;
    };
    auto* watcher = new QFutureWatcher<Outcome>(this);
    connect(watcher, &QFutureWatcher<Outcome>::finished, this, [this, watcher, st] {
        watcher->deleteLater();
        if (st != state_)
            return;
        auto out = watcher->result();
        if (!out.error.isEmpty()) {
            pendingRestore_.reset();
            finishProgress(tr("Comparing the pull requests failed"));
            diff_->showDiff(cr::FileDiff{}, tr("<b>Comparing the pull requests failed:</b> %1").arg(esc(out.error)));
            return;
        }
        st->prs = out.prs;
        const auto& a = st->prs->a;
        const auto& b = st->prs->b;
        base_ = cr::Revision::commit(a.label, a.head, a.label + (a.title.empty() ? "" : ": " + a.title));
        target_ = cr::Revision::commit(b.label, b.head, b.label + (b.title.empty() ? "" : ": " + b.title));
        haveRevisions_ = true;
        updateRevisionButtons();
        st->result = st->prs->interdiff;
        showResult(st);
        const auto m = st->prs->mapping;
        statusLabel_->setText(statusLabel_->text() + tr("  ·  paths: %1")
                                                         .arg(m.from.empty() && m.to.empty() ? tr("same in both")
                                                                                               : q(m.from.empty() ? "(root)" : m.from) +
                                                                                                     QStringLiteral(" → ") +
                                                                                                     q(m.to.empty() ? "(root)" : m.to)));
    });
    watcher->setFuture(QtConcurrent::run([req, local, ignoreWs]() {
        Outcome out;
        auto prs = std::make_shared<PrComparison>();
        for (int k = 0; k < 2; ++k) {
            // A resumed session reopens the commits it had, if they're still there.
            if (const auto& stored = k == 0 ? req.storedA : req.storedB; stored && !stored->head.isEmpty()) {
                auto repo = stored->cached ? cr::GitRepo::openBare(stored->repo.toStdString())
                                           : cr::GitRepo::open(stored->repo.toStdString());
                if (repo)
                    if (auto pr = cr::resolvePullRequest(*repo, stored->cached, stored->base.toStdString(),
                                                         stored->head.toStdString(), stored->title.toStdString(),
                                                         stored->label.toStdString())) {
                        (k == 0 ? prs->a : prs->b) = std::move(*pr);
                        continue;
                    }
            }
            const auto link = (k == 0 ? req.linkA : req.linkB).toStdString();
            auto url = cr::parsePullRequestUrl(link);
            if (!url) {
                out.error = tr("Not a pull request link: %1").arg(q(link));
                return out;
            }
            std::string err;
            auto pr = cr::resolvePullRequest(*url, local ? &*local : nullptr, &err);
            if (!pr) {
                out.error = q(err);
                return out;
            }
            (k == 0 ? prs->a : prs->b) = std::move(*pr);
        }
        if (auto m = cr::PathMapping::parse(req.mapping.toStdString()))
            prs->mapping = *m;
        else
            prs->mapping = cr::guessPathMapping(prs->a, prs->b);
        prs->pairs = cr::pairFiles(prs->a, prs->b, prs->mapping);
        prs->interdiff = std::make_shared<cr::ReviewResult>(cr::computeInterdiff(prs->a, prs->b, prs->pairs, ignoreWs));
        out.prs = prs;
        return out;
    }));
}

std::optional<SessionRecord> MainWindow::currentSessionRecord() const
{
    if (!state_ || !state_->result)
        return std::nullopt;
    SessionRecord r;
    if (prRequest_ && state_->prs) {
        r.kind = SessionRecord::Kind::PullRequests;
        r.linkA = prRequest_->linkA;
        r.linkB = prRequest_->linkB;
        r.mapping = prRequest_->mapping;
        r.finalFiles = state_->result == state_->prs->finalFiles;
        for (auto [side, pr] : {std::pair{&r.a, &state_->prs->a}, std::pair{&r.b, &state_->prs->b}}) {
            side->repo = q(pr->repo->root());
            side->base = q(pr->base);
            side->head = q(pr->head);
            side->title = q(pr->title);
            side->label = q(pr->label);
            side->cached = pr->cached;
        }
        r.repo = repo_ ? q(repo_->root()) : QString();
    } else if (base_.kind == cr::Revision::Kind::Directory) {
        r.kind = SessionRecord::Kind::Directories;
        r.oldDir = q(base_.ref);
        r.newDir = q(target_.ref);
    } else if (repo_) {
        r.kind = SessionRecord::Kind::Revisions;
        r.repo = q(repo_->root());
        r.base = base_;
        r.target = target_;
        r.prLink = currentPrLink_;
    } else {
        return std::nullopt;
    }
    // Where the reviewer is: the diff's file and first visible line, and the open file tabs.
    const auto& files = state_->result->files;
    if (currentFile_ >= 0 && currentFile_ < static_cast<int>(files.size())) {
        const auto& fd = files[static_cast<size_t>(currentFile_)];
        r.file = q(fd.path());
        auto* v = diff_->view(fd.newPath.empty() ? cr::Side::Old : cr::Side::New);
        for (int row = v->topRow(); row < static_cast<int>(v->rows().size()); ++row)
            if (int l = v->lineForRow(row); l >= 0) {
                r.line = l + 1;
                break;
            }
    }
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto* v = qobject_cast<CodeView*>(tabs_->widget(i)); v && !v->path().isEmpty())
            r.tabs.push_back({v->side(), v->path(), std::max(0, v->lineForRow(v->topRow())) + 1});
    return r;
}

void MainWindow::saveCurrentSession()
{
    if (pendingRestore_)
        return; // not where the reviewer was yet
    if (auto r = currentSessionRecord())
        SessionStore::save(*r);
}

void MainWindow::resumeSession(const SessionRecord& record, bool latest)
{
    pendingRestore_ = record;
    switch (record.kind) {
    case SessionRecord::Kind::Directories:
        compareDirectories(record.oldDir, record.newDir);
        break;
    case SessionRecord::Kind::PullRequests:
        if (!record.repo.isEmpty())
            openRepository(record.repo);
        prRequest_ = PrRequest{record.linkA, record.linkB, record.mapping, std::nullopt, std::nullopt};
        if (!latest) {
            prRequest_->storedA = record.a;
            prRequest_->storedB = record.b;
        }
        startPullRequestComparison();
        break;
    case SessionRecord::Kind::Revisions:
        if (!openRepository(record.repo)) {
            pendingRestore_.reset();
            return;
        }
        if (latest && !record.prLink.isEmpty())
            if (auto pr = pullRequestFromUrl(*repo_, record.prLink)) {
                reviewPullRequest(*pr);
                break;
            }
        setRevisions(record.base, record.target);
        currentPrLink_ = record.prLink;
        break;
    }
}

bool MainWindow::applyPendingRestore()
{
    const auto r = *pendingRestore_;
    if (r.kind == SessionRecord::Kind::PullRequests && r.finalFiles && state_->prs &&
        state_->result == state_->prs->interdiff) {
        setPrView(true); // restores the position once the final files are shown
        return true;
    }
    pendingRestore_.reset();
    const auto& result = *state_->result;
    int file = result.fileIndex(cr::Side::New, r.file.toStdString());
    const auto side = file >= 0 ? cr::Side::New : cr::Side::Old;
    if (file < 0)
        file = result.fileIndex(cr::Side::Old, r.file.toStdString());
    for (const auto& t : r.tabs)
        openWholeFile(t.side, t.path, std::max(0, t.line - 1));
    back_.clear();
    backAction_->setEnabled(false);
    if (file >= 0) {
        showFile(file);
        tabs_->setCurrentWidget(diff_);
        if (r.line > 0) {
            // First visible line, as it was.
            auto* v = diff_->view(side);
            const int row = v->rowForLine(r.line - 1);
            if (row >= 0)
                for (auto* view : {diff_->view(cr::Side::Old), diff_->view(cr::Side::New)})
                    view->verticalScrollBar()->setValue(row);
        }
    }
    return false;
}

void MainWindow::setPrView(bool finalFiles)
{
    auto st = state_;
    if (!st || !st->prs)
        return;
    if (!finalFiles) {
        st->result = st->prs->interdiff;
        showResult(st);
        return;
    }
    if (st->prs->finalFiles) {
        st->result = st->prs->finalFiles;
        showResult(st);
        return;
    }
    // The files after each pull request: a review between the two heads, made on first use.
    const auto& a = st->prs->a;
    const auto& b = st->prs->b;
    auto session = std::make_shared<cr::ReviewSession>(a.repo, base_, b.repo, target_);
    session->setFiles(cr::finalFilePairs(a, b, st->prs->pairs));
    for (auto [side, pr] : {std::pair{cr::Side::Old, &a}, std::pair{cr::Side::New, &b}}) {
        const auto db = QSettings().value(QStringLiteral("compileDb/") + q(pr->repo->root())).toString();
        if (!pr->cached && !db.isEmpty())
            session->setCompileDatabasePath(side, db.toStdString());
    }
    cr::ReviewOptions options;
    options.ignoreWhitespace = ignoreWhitespace_->isChecked();
    options.semantic = semantic_->isChecked();
    QPointer<MainWindow> guard(this);
    auto post = [guard](const std::string& stage, int done, int total) {
        QMetaObject::invokeMethod(qApp, [guard, stage, done, total] {
            if (guard)
                guard->showProgress(q(stage), done, total);
        }, Qt::QueuedConnection);
    };
    showProgress(tr("Exporting both pull requests…"), 0, 0);
    struct Outcome {
        QString error;
        std::shared_ptr<cr::ReviewResult> result;
    };
    auto* watcher = new QFutureWatcher<Outcome>(this);
    connect(watcher, &QFutureWatcher<Outcome>::finished, this, [this, watcher, st, session] {
        watcher->deleteLater();
        if (st != state_)
            return;
        auto out = watcher->result();
        if (!out.error.isEmpty()) {
            pendingRestore_.reset();
            finishProgress(tr("Comparing the final files failed"));
            QMessageBox::warning(this, tr("Final Files"), out.error);
            updatePrViewActions();
            return;
        }
        st->session = session;
        st->prs->finalFiles = out.result;
        st->result = out.result;
        showResult(st);
    });
    watcher->setFuture(QtConcurrent::run([st, session, options, post]() {
        Outcome out;
        std::string err;
        if (!session->prepare(&err, post)) {
            out.error = q(err);
            return out;
        }
        out.result = std::make_shared<cr::ReviewResult>(session->run(options, post, &st->cancel));
        return out;
    }));
}

void MainWindow::updatePrViewActions()
{
    const bool prs = state_ && state_->prs;
    for (auto* a : {interdiffAction_, finalFilesAction_})
        a->setVisible(prs);
    if (prs) {
        const bool interdiff = state_->result == state_->prs->interdiff;
        interdiffAction_->setChecked(interdiff);
        finalFilesAction_->setChecked(!interdiff);
    }
}

void MainWindow::startIndexing(const std::shared_ptr<State>& st)
{
    if (!semantic_->isChecked() || st->indexing)
        return;
    st->indexing = true;
    QPointer<MainWindow> guard(this);
    auto report = [guard, st](const QString& what) {
        return [guard, st, what](int done, int total) {
            if (done % 8 != 0 && done != total)
                return;
            QMetaObject::invokeMethod(qApp, [guard, st, what, done, total] {
                if (guard && guard->state_ == st && !guard->progress_->isVisible())
                    guard->statusLabel_->setText(tr("Indexing symbols (%1): %2/%3").arg(what).arg(done).arg(total));
            }, Qt::QueuedConnection);
        };
    };
    indexFuture_ = QtConcurrent::run([st, report, guard]() {
        st->session->project(cr::Side::New)->buildIndex(report(tr("target")), st->cancel);
        st->session->project(cr::Side::Old)->buildIndex(report(tr("base")), st->cancel);
        QMetaObject::invokeMethod(qApp, [guard, st] {
            if (!guard || guard->state_ != st || st->cancel)
                return;
            int files = 0, cached = 0;
            for (auto side : {cr::Side::Old, cr::Side::New}) {
                files += st->session->project(side)->indexedFiles();
                cached += st->session->project(side)->indexCacheHits();
            }
            guard->statusLabel_->setText(tr("Symbol index ready (%1 of %2 files from cache) — Ctrl+Click or F12 to go to definition")
                                             .arg(cached)
                                             .arg(files));
        }, Qt::QueuedConnection);
    });
}

void MainWindow::populateFiles()
{
    files_->clear();
    const auto& r = *state_->result;
    const auto& theme = Theme::current();
    for (size_t i = 0; i < r.files.size(); ++i) {
        const auto& f = r.files[i];
        int semantic = 0;
        for (int c : f.changes)
            semantic += r.changes[static_cast<size_t>(c)].trivial ? 0 : 1;
        QString path = q(f.path());
        if (f.change.status == 'R' || f.change.status == 'C')
            path = q(f.oldPath) + QStringLiteral(" → ") + q(f.newPath);
        auto* it = new QTreeWidgetItem(files_, {QString(QChar::fromLatin1(f.change.status == '?' ? 'A' : f.change.status)),
                                                path,
                                                QStringLiteral("+%1 −%2").arg(f.added).arg(f.removed),
                                                semantic ? QString::number(semantic) : QString()});
        it->setData(0, RoleFile, static_cast<int>(i));
        QColor c = f.change.status == '=' ? palette().color(QPalette::PlaceholderText)
                 : f.change.status == 'A' ? theme.changeColor(cr::ChangeKind::Added)
                 : f.change.status == 'D' ? theme.changeColor(cr::ChangeKind::Removed)
                 : f.change.status == 'R' ? theme.changeColor(cr::ChangeKind::Moved)
                                          : theme.changeColor(cr::ChangeKind::Modified);
        it->setForeground(0, c);
        QFont bold = it->font(0);
        bold.setBold(true);
        it->setFont(0, bold);
        if (f.synthetic)
            path = f.change.status == 'A' ? q(f.newPath) : q(f.oldPath.empty() ? f.newPath : f.oldPath);
        it->setToolTip(1, f.synthetic ? QStringLiteral("A: %1\nB: %2").arg(q(f.oldPath), q(f.newPath)) : path);
        it->setData(1, RoleLabel, path);
        if (f.synthetic)
            it->setToolTip(0, f.change.status == '=' ? tr("Both pull requests change this file the same way")
                              : f.change.status == 'A' ? tr("Only B changes this file")
                              : f.change.status == 'D' ? tr("Only A changes this file")
                                                       : tr("The pull requests change this file differently"));
        if (f.oldErrors || f.newErrors)
            it->setToolTip(3, tr("libclang reported %1 error(s) in base and %2 in target; semantic results may be incomplete")
                                  .arg(f.oldErrors)
                                  .arg(f.newErrors));
    }
}

void MainWindow::populateChanges()
{
    const QSignalBlocker blocker(changes_);
    changes_->clear();
    const auto& r = *state_->result;
    const auto& theme = Theme::current();
    for (auto kind : kGroupOrder) {
        QVector<int> items;
        for (size_t c = 0; c < r.changes.size(); ++c)
            if (r.changes[c].kind == kind)
                items.push_back(static_cast<int>(c));
        if (items.isEmpty())
            continue;
        auto* group = new QTreeWidgetItem(changes_, {groupName(kind) + QStringLiteral(" (%1)").arg(items.size())});
        group->setData(0, RoleChange, -1);
        group->setData(0, RoleLabel, group->text(0));
        group->setForeground(0, theme.changeColor(kind));
        QFont f = group->font(0);
        f.setBold(true);
        group->setFont(0, f);
        for (int c : items) {
            const auto& ch = r.changes[static_cast<size_t>(c)];
            auto* it = new QTreeWidgetItem(group, {q(ch.title)});
            it->setData(0, RoleChange, c);
            QString tip = QStringLiteral("<b>%1</b>").arg(esc(q(ch.title)));
            if (ch.oldLoc.valid())
                tip += QStringLiteral("<br>Before: ") + esc(locText(ch.oldLoc));
            if (ch.newLoc.valid())
                tip += QStringLiteral("<br>After: ") + esc(locText(ch.newLoc));
            if (!ch.detail.empty())
                tip += QStringLiteral("<br>") + esc(q(ch.detail));
            it->setToolTip(0, tip);
            if (ch.trivial)
                it->setForeground(0, palette().color(QPalette::PlaceholderText));
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setCheckState(0, changeReviewed(c) ? Qt::Checked : Qt::Unchecked);
            addRelatedItems(it, c);
        }
        // Plain modifications/additions/removals can be numerous; keep them collapsed.
        group->setExpanded(kind != cr::ChangeKind::Modified && kind != cr::ChangeKind::Added &&
                           kind != cr::ChangeKind::Removed);
    }
    refreshReviewMarks();
}

bool MainWindow::changeReviewed(int change) const
{
    return change >= 0 && static_cast<size_t>(change) < changeKeys_.size() &&
           reviewed_.has(changeKeys_[static_cast<size_t>(change)]);
}

bool MainWindow::hunkReviewed(int file, const cr::Hunk& h) const
{
    if (reviewed_.has(h.key))
        return true;
    if (!state_ || !state_->result || file < 0 || file >= static_cast<int>(state_->result->files.size()))
        return false;
    // Also reviewed when every changed line belongs to a change checked as reviewed, either
    // directly or by lying inside its range (e.g. extracted code inside a modified function).
    const auto& r = *state_->result;
    const auto& fd = r.files[static_cast<size_t>(file)];
    bool any = false;
    auto covered = [&](cr::Side side, int line, const cr::LineInfo& info) {
        if (info.tag == cr::LineTag::None)
            return true;
        any = true;
        if (changeReviewed(info.change))
            return true;
        const auto& path = side == cr::Side::Old ? fd.oldPath : fd.newPath;
        for (int c : reviewedChanges_) {
            const auto& ch = r.changes[static_cast<size_t>(c)];
            const auto& loc = side == cr::Side::Old ? ch.oldLoc : ch.newLoc;
            if (loc.valid() && loc.file == path && line + 1 >= loc.line && line + 1 <= std::max(loc.line, loc.endLine))
                return true;
        }
        return false;
    };
    for (int row = h.rowBegin; row < h.rowEnd; ++row) {
        const auto& dr = fd.rows[static_cast<size_t>(row)];
        if (dr.oldLine >= 0 && !covered(cr::Side::Old, dr.oldLine, fd.oldInfo[static_cast<size_t>(dr.oldLine)]))
            return false;
        if (dr.newLine >= 0 && !covered(cr::Side::New, dr.newLine, fd.newInfo[static_cast<size_t>(dr.newLine)]))
            return false;
    }
    return any;
}

void MainWindow::onChangeItemChanged(QTreeWidgetItem* it)
{
    bool ok = false;
    const int c = it->data(0, RoleChange).toInt(&ok);
    if (!ok || c < 0 || it->data(0, RoleOccurrence).isValid() || static_cast<size_t>(c) >= changeKeys_.size())
        return;
    const bool on = it->checkState(0) == Qt::Checked;
    if (on == changeReviewed(c))
        return;
    reviewed_.set(changeKeys_[static_cast<size_t>(c)], on);
    refreshReviewMarks();
}

void MainWindow::refreshReviewMarks()
{
    if (!state_ || !state_->result)
        return;
    reviewedChanges_.clear();
    for (size_t c = 0; c < changeKeys_.size(); ++c)
        if (reviewed_.has(changeKeys_[c]))
            reviewedChanges_.push_back(static_cast<int>(c));
    diff_->refreshFolding();

    // Files: ✓ once every hunk is reviewed.
    const auto dim = palette().color(QPalette::PlaceholderText);
    for (int i = 0; i < files_->topLevelItemCount(); ++i) {
        auto* it = files_->topLevelItem(i);
        const int f = it->data(0, RoleFile).toInt();
        if (f < 0 || static_cast<size_t>(f) >= fileHunks_.size())
            continue;
        const auto& hunks = fileHunks_[static_cast<size_t>(f)];
        int done = 0;
        for (const auto& h : hunks)
            done += hunkReviewed(f, h) ? 1 : 0;
        const QString label = it->data(1, RoleLabel).toString();
        const bool all = !hunks.empty() && done == static_cast<int>(hunks.size());
        it->setText(1, all ? QStringLiteral("✓ ") + label : label);
        it->setForeground(1, all ? QBrush(dim) : QBrush());
        it->setToolTip(1, label + tr("\n%1 of %2 hunk(s) reviewed").arg(done).arg(hunks.size()));
    }

    // Changes: checked ones are dimmed, groups count them.
    const QSignalBlocker blocker(changes_);
    for (int g = 0; g < changes_->topLevelItemCount(); ++g) {
        auto* group = changes_->topLevelItem(g);
        int done = 0;
        for (int k = 0; k < group->childCount(); ++k) {
            auto* it = group->child(k);
            const int c = it->data(0, RoleChange).toInt();
            const bool on = changeReviewed(c);
            done += on ? 1 : 0;
            it->setCheckState(0, on ? Qt::Checked : Qt::Unchecked);
            QFont font = it->font(0);
            font.setStrikeOut(on);
            it->setFont(0, font);
        }
        const QString label = group->data(0, RoleLabel).toString();
        group->setText(0, done ? label + tr("  ✓ %1/%2").arg(done).arg(group->childCount()) : label);
    }
    filterChanges();
}

void MainWindow::filterChanges()
{
    if (!state_ || !state_->result)
        return;
    const QString f = changeFilter_->text().trimmed();
    const bool hideTrivial = hideTrivial_->isChecked();
    for (int g = 0; g < changes_->topLevelItemCount(); ++g) {
        auto* group = changes_->topLevelItem(g);
        int visible = 0;
        for (int k = 0; k < group->childCount(); ++k) {
            auto* it = group->child(k);
            int c = it->data(0, RoleChange).toInt();
            bool show = (f.isEmpty() || it->text(0).contains(f, Qt::CaseInsensitive)) &&
                        !(hideTrivial && state_->result->changes[static_cast<size_t>(c)].trivial) &&
                        !(hideReviewed_->isChecked() && changeReviewed(c));
            it->setHidden(!show);
            visible += show ? 1 : 0;
        }
        group->setHidden(visible == 0);
    }
}

QString MainWindow::fileTitle(const cr::FileDiff& fd) const
{
    const auto& theme = Theme::current();
    QString status;
    if (fd.synthetic) {
        switch (fd.change.status) {
        case '=': status = tr("same change in both"); break;
        case 'A': status = tr("changed only by B"); break;
        case 'D': status = tr("changed only by A"); break;
        default: status = tr("changed differently"); break;
        }
        return QStringLiteral("<b>%1</b> &nbsp;<span style='color:gray'>%2</span>%3")
            .arg(esc(q(fd.oldPath.empty() ? fd.newPath : fd.oldPath)), status,
                 fd.oldPath != fd.newPath && !fd.oldPath.empty() && !fd.newPath.empty()
                     ? tr(" &nbsp;<span style='color:gray'>⇄ %1</span>").arg(esc(q(fd.newPath)))
                     : QString());
    }
    switch (fd.change.status) {
    case 'A': status = tr("added"); break;
    case 'D': status = tr("deleted"); break;
    case 'R': status = tr("renamed from %1").arg(esc(q(fd.oldPath))); break;
    case 'C': status = tr("copied from %1").arg(esc(q(fd.oldPath))); break;
    default: status = tr("modified"); break;
    }
    QString t = QStringLiteral("<b>%1</b> &nbsp;<span style='color:gray'>%2</span> &nbsp;"
                               "<span style='color:%3'>+%4</span> <span style='color:%5'>−%6</span>")
                    .arg(esc(q(fd.path())), status, theme.changeColor(cr::ChangeKind::Added).name())
                    .arg(fd.added)
                    .arg(theme.changeColor(cr::ChangeKind::Removed).name())
                    .arg(fd.removed);
    if (fd.oldErrors || fd.newErrors)
        t += tr(" &nbsp;<span style='color:%1'>⚠ libclang errors: %2 base / %3 target</span>")
                 .arg(theme.changeColor(cr::ChangeKind::SignatureChanged).name())
                 .arg(fd.oldErrors)
                 .arg(fd.newErrors);
    return t;
}

void MainWindow::showFile(int index)
{
    if (!state_ || !state_->result || index < 0 || index >= static_cast<int>(state_->result->files.size()))
        return;
    currentFile_ = index;
    const auto& fd = state_->result->files[static_cast<size_t>(index)];
    diff_->showDiff(fd, fileTitle(fd));
    // Select the file in the list without re-triggering navigation.
    restoring_ = true;
    for (int i = 0; i < files_->topLevelItemCount(); ++i)
        if (files_->topLevelItem(i)->data(0, RoleFile).toInt() == index)
            files_->setCurrentItem(files_->topLevelItem(i));
    restoring_ = false;
    // Start at the first change still to be reviewed.
    diff_->scrollToFirstChange();
}

ChangeMeta MainWindow::changeMeta(int change) const
{
    ChangeMeta m;
    if (!state_ || !state_->result || change < 0 || change >= static_cast<int>(state_->result->changes.size()))
        return m;
    const auto& c = state_->result->changes[static_cast<size_t>(change)];
    m.badge = Theme::badge(c.kind);
    m.color = Theme::current().changeColor(c.kind);
    m.title = q(c.title);
    return m;
}

// ------------------------------------------------------------------------------------ changes

void MainWindow::setWordHighlight(const QString& oldWord, const QString& newWord)
{
    highlightOld_ = oldWord;
    highlightNew_ = newWord;
    for (int i = 0; i < tabs_->count(); ++i) {
        if (auto* d = qobject_cast<DiffView*>(tabs_->widget(i)))
            d->setWordHighlights(oldWord, newWord);
        else if (auto* v = qobject_cast<CodeView*>(tabs_->widget(i)))
            v->setHighlightWord(v->side() == cr::Side::Old ? oldWord : newWord);
    }
}

namespace {

// Where to go for a related item: the new side when it exists there.
std::pair<cr::Side, cr::Location> primary(const cr::RelatedItem& it)
{
    if (it.onOldSide || !it.newLoc.valid())
        return std::pair{cr::Side::Old, it.oldLoc};
    return std::pair{cr::Side::New, it.newLoc};
}

} // namespace

void MainWindow::addRelatedItems(QTreeWidgetItem* parent, int change)
{
    const auto& r = *state_->result;
    const auto& ch = r.changes[static_cast<size_t>(change)];
    // A single "before → after" entry adds nothing over the change itself.
    const bool renames = ch.kind == cr::ChangeKind::SymbolRenamed || ch.kind == cr::ChangeKind::Renamed;
    if (ch.related.empty() || (ch.related.size() == 1 && !renames))
        return;
    const auto& theme = Theme::current();
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    int warnings = 0;
    int usages = 0;
    for (size_t k = 0; k < ch.related.size(); ++k) {
        const auto& item = ch.related[k];
        const auto [side, loc] = primary(item);
        // Several entries of one kind on the same line become a single row.
        size_t same = 1;
        while (k + same < ch.related.size()) {
            const auto& next = ch.related[k + same];
            const auto [ns, nl] = primary(next);
            if (next.label != item.label || ns != side || nl.file != loc.file || nl.line != loc.line)
                break;
            ++same;
        }
        QString code;
        if (int f = r.fileIndex(side, loc.file); f >= 0) {
            const auto& lines = side == cr::Side::New ? r.files[static_cast<size_t>(f)].newLines
                                                      : r.files[static_cast<size_t>(f)].oldLines;
            if (loc.line >= 1 && static_cast<size_t>(loc.line) <= lines.size())
                code = q(lines[static_cast<size_t>(loc.line - 1)]).trimmed();
        }
        if (code.size() > 80)
            code = code.left(78) + QStringLiteral("…");
        QString where = QFileInfo(q(loc.file)).fileName() + QLatin1Char(':') + QString::number(loc.line);
        if (loc.endLine > loc.line)
            where += QLatin1Char('-') + QString::number(loc.endLine);
        if (same > 1)
            where += QStringLiteral(" ×%1").arg(same);
        QString label = q(item.label);
        if (side == cr::Side::Old && !renames)
            label += tr(" (base)");
        auto* child = new QTreeWidgetItem(parent, {(item.warning ? QStringLiteral("⚠ ") : QString()) + label +
                                                   QStringLiteral("  ") + where + QStringLiteral("   ") + code});
        child->setData(0, RoleChange, change);
        child->setData(0, RoleOccurrence, static_cast<int>(k));
        child->setFont(0, mono);
        if (item.warning)
            child->setForeground(0, theme.changeColor(cr::ChangeKind::SignatureChanged));
        QString tip = QStringLiteral("<b>%1</b>").arg(esc(q(item.label)));
        if (item.oldLoc.valid())
            tip += QStringLiteral("<br>Before: ") + esc(locText(item.oldLoc));
        if (item.newLoc.valid())
            tip += QStringLiteral("<br>After: ") + esc(locText(item.newLoc));
        if (item.oldLoc.valid() && item.newLoc.valid() && (item.oldLoc.endLine > item.oldLoc.line ||
                                                           item.newLoc.endLine > item.newLoc.line))
            tip += tr("<br><i>Double-click to compare</i>");
        child->setToolTip(0, tip);
        warnings += item.warning ? static_cast<int>(same) : 0;
        if (item.label != "definition" && item.label != "declaration" && item.label.rfind("hunk", 0) != 0 &&
            item.label != "before → after" && item.label != "extracted code" && item.label != "inlined body" &&
            item.label != "original code" && item.label != "moved code")
            usages += static_cast<int>(same);
        k += same - 1;
    }
    QString suffix;
    if (renames)
        suffix = tr("[%n occurrence(s)]", nullptr, static_cast<int>(ch.related.size()));
    else if (ch.kind == cr::ChangeKind::Modified)
        suffix = tr("[%n hunk(s)]", nullptr, parent->childCount());
    else if (usages > 0)
        suffix = tr("[%n use(s)]", nullptr, usages);
    if (warnings > 0)
        suffix += (suffix.isEmpty() ? QString() : QStringLiteral(" ")) + tr("⚠ %n to check", nullptr, warnings);
    if (!suffix.isEmpty())
        parent->setText(0, parent->text(0) + QStringLiteral("  ") + suffix);
    if (warnings > 0 && !ch.trivial)
        parent->setForeground(0, theme.changeColor(cr::ChangeKind::SignatureChanged));
}

void MainWindow::showChangeDetails(int change)
{
    if (!state_ || !state_->result)
        return;
    const auto& c = state_->result->changes[static_cast<size_t>(change)];
    // Select and expand the change so its related places are visible.
    for (QTreeWidgetItemIterator it(changes_); *it; ++it) {
        if ((*it)->data(0, RoleChange).toInt() == change && !(*it)->data(0, RoleOccurrence).isValid()) {
            changes_->setCurrentItem(*it);
            (*it)->setExpanded(true);
            if ((*it)->parent())
                (*it)->parent()->setExpanded(true);
            changes_->scrollToItem(*it);
            break;
        }
    }
    if (c.kind == cr::ChangeKind::SymbolRenamed && !c.related.empty())
        gotoRelated(change, 0);
    else
        selectChange(change);
    int warnings = 0;
    for (const auto& it : c.related)
        warnings += it.warning ? 1 : 0;
    QString msg = q(c.title);
    if (c.related.size() > 1)
        msg += tr("  —  %n related place(s) listed under the change", nullptr, static_cast<int>(c.related.size()));
    if (warnings)
        msg += tr(", %n marked ⚠", nullptr, warnings);
    if (!c.highlightOld.empty() || !c.highlightNew.empty())
        msg += tr("  (Esc clears the highlight)");
    statusLabel_->setText(msg);
}

void MainWindow::gotoRelated(int change, int index)
{
    if (!state_ || !state_->result)
        return;
    const auto& r = *state_->result;
    const auto& c = r.changes[static_cast<size_t>(change)];
    if (index < 0 || static_cast<size_t>(index) >= c.related.size())
        return;
    const auto& item = c.related[static_cast<size_t>(index)];
    setWordHighlight(q(c.highlightOld), q(c.highlightNew));
    const auto [side, loc] = primary(item);
    navigateTo(side, loc);
    // Show the counterpart as well when it's in the same diff.
    const auto& other = side == cr::Side::New ? item.oldLoc : item.newLoc;
    const auto otherSide = side == cr::Side::New ? cr::Side::Old : cr::Side::New;
    if (other.valid() && tabs_->currentWidget() == diff_ && r.fileIndex(otherSide, other.file) == currentFile_)
        diff_->view(otherSide)->flashRow(diff_->view(otherSide)->rowForLine(other.line - 1));
    QString msg = tr("%1 (%2 of %3): %4").arg(q(item.label)).arg(index + 1).arg(c.related.size()).arg(locText(loc));
    if (other.valid())
        msg += (side == cr::Side::New ? tr("  — was %1") : tr("  — now %1")).arg(locText(other));
    statusLabel_->setText(msg);
}

void MainWindow::selectChange(int change)
{
    if (!state_ || !state_->result)
        return;
    const auto& r = *state_->result;
    const auto& c = r.changes[static_cast<size_t>(change)];
    setWordHighlight(q(c.highlightOld), q(c.highlightNew));
    const int oldFile = c.oldLoc.valid() ? r.fileIndex(cr::Side::Old, c.oldLoc.file) : -1;
    const int newFile = c.newLoc.valid() ? r.fileIndex(cr::Side::New, c.newLoc.file) : -1;
    const int file = newFile >= 0 ? newFile : oldFile;
    if (file < 0)
        return;
    auto before = currentPosition();
    if (file != currentFile_ || tabs_->currentWidget() != diff_)
        showFile(file);
    tabs_->setCurrentWidget(diff_);
    if (newFile == file)
        diff_->scrollToLine(cr::Side::New, c.newLoc.line - 1, true);
    else
        diff_->scrollToLine(cr::Side::Old, c.oldLoc.line - 1, true);
    if (oldFile == file && newFile == file) // both ends in this file: mark the origin as well
        diff_->view(cr::Side::Old)->flashRow(diff_->view(cr::Side::Old)->rowForLine(c.oldLoc.line - 1));
    if (before.valid()) {
        back_.push_back(before);
        forward_.clear();
        backAction_->setEnabled(true);
        forwardAction_->setEnabled(false);
    }
    QString msg = q(c.title);
    if (oldFile != newFile && c.oldLoc.valid() && c.newLoc.valid())
        msg += tr("   —   double-click to compare with %1").arg(locText(c.oldLoc));
    statusLabel_->setText(msg);
}

std::vector<std::string> MainWindow::fileLines(cr::Side side, const QString& path)
{
    const QString key = QString::number(static_cast<int>(side)) + QLatin1Char(':') + path;
    auto it = lineCache_.find(key);
    if (it != lineCache_.end())
        return it.value();
    std::vector<std::string> lines;
    if (state_ && state_->session) {
        std::optional<std::string> text;
        if (QFileInfo(path).isAbsolute()) {
            QFile f(path);
            if (f.open(QIODevice::ReadOnly))
                text = f.readAll().toStdString();
        } else {
            text = state_->session->snapshot(side).read(path.toStdString());
        }
        if (text)
            lines = cr::splitLines(*text);
    }
    lineCache_.insert(key, lines);
    return lines;
}

void MainWindow::openChangeComparison(int change)
{
    if (!state_ || !state_->result)
        return;
    const auto& c = state_->result->changes[static_cast<size_t>(change)];
    if (c.kind == cr::ChangeKind::SymbolRenamed || !c.oldLoc.valid() || !c.newLoc.valid()) {
        showChangeDetails(change);
        return;
    }
    setWordHighlight(q(c.highlightOld), q(c.highlightNew));
    QString tabTitle = Theme::badge(c.kind) + QLatin1Char(' ') +
                       q(c.newName.empty() ? c.newLoc.file : c.newName).section(QStringLiteral("::"), -1);
    openComparison(c.oldLoc, c.newLoc, q(c.title), tabTitle);
}

void MainWindow::openComparison(const cr::Location& oldLoc, const cr::Location& newLoc, const QString& heading,
                                const QString& tabTitle)
{
    if (!state_ || !state_->result)
        return;
    // Diff the original fragment against the new one, even when they live in different files.
    // Surrounding lines are shown on each side (dimmed) but not compared: the two places are unrelated.
    const int context = QSettings().value(QStringLiteral("comparisonContext"), 10).toInt();
    struct Slice {
        int begin, end;               // the fragment, 0-based [begin, end)
        int ctxBegin, ctxEnd;         // with context
    };
    auto sliceOf = [context](const std::vector<std::string>& lines, const cr::Location& l) {
        Slice s;
        const int n = static_cast<int>(lines.size());
        s.begin = std::clamp(l.line - 1, 0, n);
        s.end = std::clamp(std::max(l.line, l.endLine), s.begin, n);
        s.ctxBegin = std::max(0, s.begin - context);
        s.ctxEnd = std::min(n, s.end + context);
        return s;
    };
    const auto oldAll = fileLines(cr::Side::Old, q(oldLoc.file));
    const auto newAll = fileLines(cr::Side::New, q(newLoc.file));
    const auto os = sliceOf(oldAll, oldLoc), ns = sliceOf(newAll, newLoc);
    cr::FileDiff core;
    core.oldLines.assign(oldAll.begin() + os.begin, oldAll.begin() + os.end);
    core.newLines.assign(newAll.begin() + ns.begin, newAll.begin() + ns.end);
    cr::diffLines(core, ignoreWhitespace_->isChecked());

    cr::FileDiff fd;
    fd.oldPath = oldLoc.file;
    fd.newPath = newLoc.file;
    fd.oldLines.assign(oldAll.begin() + os.ctxBegin, oldAll.begin() + os.ctxEnd);
    fd.newLines.assign(newAll.begin() + ns.ctxBegin, newAll.begin() + ns.ctxEnd);
    fd.oldInfo.assign(fd.oldLines.size(), {});
    fd.newInfo.assign(fd.newLines.size(), {});
    const int oldPre = os.begin - os.ctxBegin, newPre = ns.begin - ns.ctxBegin;
    fd.oldContentBegin = oldPre;
    fd.oldContentEnd = oldPre + (os.end - os.begin);
    fd.newContentBegin = newPre;
    fd.newContentEnd = newPre + (ns.end - ns.begin);
    // Context before, aligned to the fragment (the shorter side starts with fillers).
    const int pre = std::max(oldPre, newPre);
    for (int k = 0; k < pre; ++k) {
        const int o = k - (pre - oldPre), n = k - (pre - newPre);
        fd.rows.push_back({cr::RowKind::Equal, o >= 0 ? o : -1, n >= 0 ? n : -1});
    }
    for (const auto& row : core.rows) {
        cr::DiffRow r = row;
        if (r.oldLine >= 0) {
            fd.oldInfo[static_cast<size_t>(oldPre + r.oldLine)] = core.oldInfo[static_cast<size_t>(r.oldLine)];
            r.oldLine += oldPre;
        }
        if (r.newLine >= 0) {
            fd.newInfo[static_cast<size_t>(newPre + r.newLine)] = core.newInfo[static_cast<size_t>(r.newLine)];
            r.newLine += newPre;
        }
        fd.rows.push_back(r);
    }
    const int oldPost = os.ctxEnd - os.end, newPost = ns.ctxEnd - ns.end;
    for (int k = 0; k < std::max(oldPost, newPost); ++k)
        fd.rows.push_back({cr::RowKind::Equal, k < oldPost ? fd.oldContentEnd + k : -1, k < newPost ? fd.newContentEnd + k : -1});
    // Apply the renames so that rename-only lines are recognizable here too.
    for (const auto& row : fd.rows) {
        if (row.kind != cr::RowKind::Modified)
            continue;
        auto a = cr::applyRenames(fd.oldLines[static_cast<size_t>(row.oldLine)], state_->result->renames);
        if (cr::normalizeWhitespace(a) == cr::normalizeWhitespace(fd.newLines[static_cast<size_t>(row.newLine)])) {
            fd.oldInfo[static_cast<size_t>(row.oldLine)].tag = cr::LineTag::RenameOnly;
            fd.newInfo[static_cast<size_t>(row.newLine)].tag = cr::LineTag::RenameOnly;
        }
    }

    auto* view = new DiffView;
    view->setWordHighlights(highlightOld_, highlightNew_);
    view->setChangeMetaProvider([this](int ch) { return changeMeta(ch); });
    connect(view, &DiffView::definitionRequested, this, &MainWindow::onDefinitionRequested);
    connect(view, &DiffView::hoverRequested, this, &MainWindow::onHoverRequested);
    connect(view, &DiffView::bookmarkRequested, this, &MainWindow::onBookmarkRequested);
    connect(view, &DiffView::openFileRequested, this, &MainWindow::openWholeFile);
    QString title = QStringLiteral("<b>%1</b><br><span style='color:gray'>%2 &nbsp;⇄&nbsp; %3</span>")
                        .arg(esc(heading), esc(locText(oldLoc)), esc(locText(newLoc)));
    view->showDiff(fd, title, os.ctxBegin, ns.ctxBegin);
    view->setSideCaptions(tr("Before: %1").arg(locText(oldLoc)), tr("After: %1").arg(locText(newLoc)));
    auto before = currentPosition();
    tabs_->addTab(view, tabTitle);
    tabs_->setTabToolTip(tabs_->indexOf(view), heading);
    tabs_->setCurrentWidget(view);
    // Start at the fragment, with its context above.
    QTimer::singleShot(0, view, [view, pre] {
        for (auto* v : {view->view(cr::Side::Old), view->view(cr::Side::New)}) {
            v->setTextCursor(QTextCursor(v->document()->findBlockByNumber(pre)));
            v->verticalScrollBar()->setValue(std::max(0, pre - 3));
        }
    });
    if (before.valid()) {
        back_.push_back(before);
        forward_.clear();
        backAction_->setEnabled(true);
        forwardAction_->setEnabled(false);
    }
}

// ------------------------------------------------------------------------------------ navigation

void MainWindow::connectCodeView(CodeView* v)
{
    connect(v, &CodeView::definitionRequested, this, &MainWindow::onDefinitionRequested);
    connect(v, &CodeView::hoverRequested, this, &MainWindow::onHoverRequested);
    connect(v, &CodeView::bookmarkRequested, this, &MainWindow::onBookmarkRequested);
}

namespace {

int byteColumn(CodeView* v, int row, int charColumn)
{
    const QString text = v->document()->findBlockByNumber(row).text();
    return static_cast<int>(text.left(charColumn).toUtf8().size()) + 1;
}

} // namespace

void MainWindow::onDefinitionRequested(CodeView* v, int row, int column, bool declaration)
{
    if (!state_ || !state_->session || v->path().isEmpty())
        return;
    const int line = v->lineForRow(row);
    if (line < 0)
        return;
    const int col = byteColumn(v, row, column);
    const auto side = v->side();
    const auto path = v->path().toStdString();
    auto st = state_;
    statusLabel_->setText(tr("Looking up symbol…"));

    auto* watcher = new QFutureWatcher<std::optional<cr::SymbolInfo>>(this);
    connect(watcher, &QFutureWatcher<std::optional<cr::SymbolInfo>>::finished, this,
            [this, watcher, st, side, path, line, declaration] {
                watcher->deleteLater();
                if (st != state_)
                    return;
                auto info = watcher->result();
                if (!info) {
                    statusLabel_->setText(tr("No symbol found at this position"));
                    return;
                }
                std::optional<cr::Location> target = declaration ? info->declaration : info->definition;
                // Already at the definition: jump to the declaration instead (and vice versa).
                auto here = [&](const std::optional<cr::Location>& l) {
                    return l && l->file == path && l->line == line + 1;
                };
                if (here(target))
                    target = declaration ? info->definition : info->declaration;
                if (!target)
                    target = declaration ? info->definition : info->declaration;
                if (!target || here(target)) {
                    statusLabel_->setText(
                        st->session->project(side)->indexReady()
                            ? tr("No other definition of %1 found").arg(q(info->qualifiedName))
                            : tr("Definition of %1 not found yet — the symbol index is still being built")
                                  .arg(q(info->qualifiedName)));
                    return;
                }
                statusLabel_->setText(q(info->kind) + QLatin1Char(' ') + q(info->qualifiedName));
                navigateTo(side, *target);
            });
    watcher->setFuture(QtConcurrent::run([st, side, path, line, col] {
        return st->session->project(side)->symbolAt(path, line + 1, col);
    }));
}

void MainWindow::onHoverRequested(CodeView* v, int row, int column, QPoint globalPos)
{
    if (!state_ || !state_->session || v->path().isEmpty())
        return;
    const int line = v->lineForRow(row);
    if (line < 0)
        return;
    const int col = byteColumn(v, row, column);
    const auto side = v->side();
    const auto path = v->path().toStdString();
    auto st = state_;
    QPointer<CodeView> view(v);
    auto* watcher = new QFutureWatcher<std::optional<cr::SymbolInfo>>(this);
    connect(watcher, &QFutureWatcher<std::optional<cr::SymbolInfo>>::finished, this,
            [this, watcher, st, view, globalPos] {
                watcher->deleteLater();
                auto info = watcher->result();
                if (st != state_ || !info || !view)
                    return;
                // Only show it if the mouse is still where the request was made.
                if ((QCursor::pos() - globalPos).manhattanLength() > 24)
                    return;
                QString html;
                if (info->kind == "include") {
                    html = tr("<b>#include</b> %1").arg(esc(q(info->spelling)));
                } else {
                    html = QStringLiteral("<span style='color:gray'>%1</span> <b>%2</b>")
                               .arg(esc(q(info->kind)), esc(q(info->qualifiedName)));
                    if (!info->type.empty())
                        html += QStringLiteral("<pre style='margin:4px 0'>%1</pre>").arg(esc(q(info->type)));
                    if (!info->comment.empty())
                        html += QStringLiteral("<pre style='margin:4px 0;color:gray'>%1</pre>")
                                    .arg(esc(q(info->comment)).left(1200));
                    if (info->definition)
                        html += tr("Definition: %1<br>").arg(esc(locText(*info->definition)));
                    if (info->declaration && (!info->definition || info->declaration->file != info->definition->file ||
                                              info->declaration->line != info->definition->line))
                        html += tr("Declaration: %1<br>").arg(esc(locText(*info->declaration)));
                    html += tr("<span style='color:gray'>Ctrl+Click: definition · Ctrl+Shift+Click: declaration</span>");
                }
                QToolTip::showText(globalPos, html, view->viewport());
            });
    watcher->setFuture(QtConcurrent::run([st, side, path, line, col] {
        return st->session->project(side)->symbolAt(path, line + 1, col);
    }));
}

CodeView* MainWindow::openFileTab(cr::Side side, const QString& path, bool external)
{
    for (int i = 0; i < tabs_->count(); ++i) {
        auto* v = qobject_cast<CodeView*>(tabs_->widget(i));
        if (v && v->side() == side && v->path() == path) {
            tabs_->setCurrentIndex(i);
            return v;
        }
    }
    auto lines = fileLines(side, path);
    QStringList texts;
    QVector<RowData> rows;
    texts.reserve(static_cast<int>(lines.size()));
    for (size_t i = 0; i < lines.size(); ++i) {
        texts << q(lines[i]);
        RowData r;
        r.line = static_cast<int>(i);
        rows.push_back(r);
    }
    auto* v = new CodeView;
    v->setSource(side, path, external);
    v->setContent(texts, rows, side == cr::Side::Old);
    v->setHighlightWord(side == cr::Side::Old ? highlightOld_ : highlightNew_);
    connectCodeView(v);
    QString title = QFileInfo(path).fileName() + (side == cr::Side::Old ? tr(" @base") : tr(" @target"));
    tabs_->addTab(v, title);
    tabs_->setTabToolTip(tabs_->indexOf(v),
                         path + (side == cr::Side::Old ? tr("\nin base: %1").arg(q(base_.display()))
                                                       : tr("\nin target: %1").arg(q(target_.display()))));
    tabs_->setCurrentWidget(v);
    return v;
}

MainWindow::NavPoint MainWindow::currentPosition() const
{
    NavPoint p;
    auto* w = tabs_->currentWidget();
    if (!w)
        return p;
    p.tab = w;
    if (auto* d = qobject_cast<DiffView*>(w)) {
        auto* v = d->view(cr::Side::Old)->hasFocus() ? d->view(cr::Side::Old) : d->view(cr::Side::New);
        p.side = v->side();
        p.path = v->path();
        p.file = d == diff_ ? currentFile_ : -1;
        int row = v->cursorRow();
        if (row < v->topRow())
            row = v->topRow();
        int line = v->lineForRow(row);
        for (int r = row; line < 0 && r < static_cast<int>(v->rows().size()); ++r)
            line = v->lineForRow(r);
        p.line = std::max(0, line);
    } else if (auto* v = qobject_cast<CodeView*>(w)) {
        p.side = v->side();
        p.path = v->path();
        p.line = std::max(0, v->lineForRow(v->cursorRow()));
    }
    return p;
}

void MainWindow::navigateTo(cr::Side side, const cr::Location& loc, bool record)
{
    if (!state_ || !state_->result)
        return;
    auto before = currentPosition();
    const int line = std::max(0, loc.line - 1);
    int fileIdx = loc.external ? -1 : state_->result->fileIndex(side, loc.file);
    if (fileIdx >= 0) {
        if (fileIdx != currentFile_)
            showFile(fileIdx);
        tabs_->setCurrentWidget(diff_);
        diff_->scrollToLine(side, line, true);
        diff_->view(side)->setFocus();
    } else {
        auto* v = openFileTab(side, q(loc.file), loc.external);
        v->scrollToLine(line, true);
        v->setFocus();
    }
    if (record && before.valid()) {
        back_.push_back(before);
        forward_.clear();
        backAction_->setEnabled(true);
        forwardAction_->setEnabled(false);
    }
}

void MainWindow::restore(const NavPoint& p)
{
    if (!p.valid() || tabs_->indexOf(p.tab) < 0)
        return;
    tabs_->setCurrentWidget(p.tab);
    if (p.tab == diff_) {
        if (p.file >= 0 && p.file != currentFile_)
            showFile(p.file);
        diff_->scrollToLine(p.side, p.line, true);
        diff_->view(p.side)->setFocus();
    } else if (auto* d = qobject_cast<DiffView*>(p.tab)) {
        d->scrollToLine(p.side, p.line, true);
    } else if (auto* v = qobject_cast<CodeView*>(p.tab)) {
        v->scrollToLine(p.line, true);
        v->setFocus();
    }
}

void MainWindow::goBack()
{
    while (!back_.isEmpty()) {
        auto p = back_.takeLast();
        if (tabs_->indexOf(p.tab) < 0)
            continue;
        forward_.push_back(currentPosition());
        restore(p);
        break;
    }
    backAction_->setEnabled(!back_.isEmpty());
    forwardAction_->setEnabled(!forward_.isEmpty());
}

void MainWindow::goForward()
{
    while (!forward_.isEmpty()) {
        auto p = forward_.takeLast();
        if (tabs_->indexOf(p.tab) < 0)
            continue;
        back_.push_back(currentPosition());
        restore(p);
        break;
    }
    backAction_->setEnabled(!back_.isEmpty());
    forwardAction_->setEnabled(!forward_.isEmpty());
}

void MainWindow::activateChangeByTitle(const QString& substringSpec, bool compare)
{
    // "occ:N:title" jumps to the N-th occurrence of a rename.
    QString substring = substringSpec;
    int occurrence = 0;
    if (substring.startsWith(QStringLiteral("occ:"))) {
        occurrence = substring.section(QLatin1Char(':'), 1, 1).toInt();
        substring = substring.section(QLatin1Char(':'), 2);
    }
    if (!state_ || !state_->result)
        return;
    for (size_t c = 0; c < state_->result->changes.size(); ++c)
        if (q(state_->result->changes[c].title).contains(substring)) {
            if (compare)
                openChangeComparison(static_cast<int>(c));
            else {
                showChangeDetails(static_cast<int>(c));
                if (occurrence > 0)
                    gotoRelated(static_cast<int>(c), occurrence);
            }
            return;
        }
}

void MainWindow::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::BackButton) {
        goBack();
        return;
    }
    if (e->button() == Qt::ForwardButton) {
        goForward();
        return;
    }
    QMainWindow::mousePressEvent(e);
}

// ------------------------------------------------------------------------------------ files

std::shared_ptr<const std::vector<std::string>> MainWindow::filesOf(State& st, cr::Side side)
{
    std::lock_guard lock(st.filesMutex);
    auto& files = st.allFiles[side == cr::Side::Old ? 0 : 1];
    if (!files && st.session)
        files = std::make_shared<const std::vector<std::string>>(cr::listFiles(st.session->snapshot(side)));
    return files ? files : std::make_shared<const std::vector<std::string>>();
}

void MainWindow::pushHistory(const NavPoint& before)
{
    if (!before.valid())
        return;
    back_.push_back(before);
    forward_.clear();
    backAction_->setEnabled(true);
    forwardAction_->setEnabled(false);
}

void MainWindow::openWholeFile(cr::Side side, const QString& path, int line)
{
    if (!state_ || !state_->session)
        return;
    auto before = currentPosition();
    auto* v = openFileTab(side, path, QFileInfo(path).isAbsolute());
    v->scrollToLine(line, true);
    v->setFocus();
    pushHistory(before);
}

QuickOpenDialog::FilesFn MainWindow::fileListProvider()
{
    auto st = state_;
    return [this, st](cr::Side side, std::function<void(QuickOpenDialog::Files)> done) {
        auto* watcher = new QFutureWatcher<QuickOpenDialog::Files>(this);
        connect(watcher, &QFutureWatcher<QuickOpenDialog::Files>::finished, this, [watcher, done] {
            watcher->deleteLater();
            done(watcher->result());
        });
        watcher->setFuture(QtConcurrent::run([st, side] { return filesOf(*st, side); }));
    };
}

void MainWindow::openQuickOpen()
{
    if (!state_ || !state_->session || !state_->result)
        return;
    auto st = state_;
    const auto* v = activeCodeView();
    QuickOpenDialog dlg(fileListProvider(), v ? v->side() : cr::Side::New, this);
    if (dlg.exec() != QDialog::Accepted || st != state_)
        return;
    cr::Location loc;
    loc.file = dlg.path().toStdString();
    loc.line = std::max(1, dlg.line());
    navigateTo(dlg.side(), loc);
}

void MainWindow::pickFilesToCompare()
{
    if (!state_ || !state_->session || !state_->result)
        return;
    auto st = state_;
    QString paths[2];
    for (auto side : {cr::Side::Old, cr::Side::New}) {
        QuickOpenDialog dlg(fileListProvider(), side, this);
        dlg.lockSide(side);
        dlg.setWindowTitle(side == cr::Side::Old ? tr("Compare Files — Base File (left)")
                                                 : tr("Compare Files — Target File (right)"));
        if (dlg.exec() != QDialog::Accepted || st != state_)
            return;
        paths[side == cr::Side::Old ? 0 : 1] = dlg.path();
    }
    compareFiles(paths[0], paths[1]);
}

void MainWindow::compareFiles(const QString& basePath, const QString& targetPath)
{
    if (!state_ || !state_->session || !state_->result || basePath.isEmpty() || targetPath.isEmpty())
        return;
    auto st = state_;
    cr::ChangedFile cf;
    cf.oldPath = basePath.toStdString();
    cf.newPath = targetPath.toStdString();
    cf.status = cf.oldPath == cf.newPath ? 'M' : 'R';
    cr::ReviewOptions options;
    options.ignoreWhitespace = ignoreWhitespace_->isChecked();
    options.semantic = semantic_->isChecked();
    showProgress(tr("Comparing %1 with %2…").arg(QFileInfo(basePath).fileName(), QFileInfo(targetPath).fileName()), 0, 0);

    using Result = std::shared_ptr<cr::ReviewResult>;
    auto* watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, st, basePath, targetPath] {
        watcher->deleteLater();
        if (st != state_)
            return;
        auto res = watcher->result();
        if (!res || res->files.empty()) {
            finishProgress(tr("Nothing to compare"));
            return;
        }
        // The pair's own semantic changes, with their summary in the heading.
        std::map<cr::ChangeKind, int> kinds;
        for (const auto& c : res->changes)
            if (!c.trivial)
                ++kinds[c.kind];
        QStringList summary;
        for (auto kind : kGroupOrder)
            if (auto it = kinds.find(kind); it != kinds.end())
                summary << QStringLiteral("%1 %2").arg(it->second).arg(groupName(kind).toLower());
        const auto& fd = res->files.front();
        QString heading = tr("<b>%1</b> &nbsp;⇄&nbsp; <b>%2</b> &nbsp;<span style='color:gray'>+%3 −%4%5</span>")
                              .arg(esc(basePath), esc(targetPath))
                              .arg(fd.added)
                              .arg(fd.removed)
                              .arg(summary.isEmpty() ? QString() : QStringLiteral(" · ") + esc(summary.join(QStringLiteral(", "))));
        auto* view = new DiffView;
        view->setWordHighlights(highlightOld_, highlightNew_);
        view->setChangeMetaProvider([res](int ch) {
            ChangeMeta m;
            if (ch < 0 || ch >= static_cast<int>(res->changes.size()))
                return m;
            const auto& c = res->changes[static_cast<size_t>(ch)];
            m.badge = Theme::badge(c.kind);
            m.color = Theme::current().changeColor(c.kind);
            m.title = q(c.title);
            return m;
        });
        connect(view, &DiffView::definitionRequested, this, &MainWindow::onDefinitionRequested);
        connect(view, &DiffView::hoverRequested, this, &MainWindow::onHoverRequested);
        connect(view, &DiffView::bookmarkRequested, this, &MainWindow::onBookmarkRequested);
        connect(view, &DiffView::openFileRequested, this, &MainWindow::openWholeFile);
        view->showDiff(fd, heading);
        view->setSideCaptions(tr("Base: %1").arg(basePath), tr("Target: %1").arg(targetPath));
        view->scrollToFirstChange();
        auto before = currentPosition();
        const QString title = QFileInfo(basePath).fileName() + QStringLiteral(" ⇄ ") + QFileInfo(targetPath).fileName();
        tabs_->addTab(view, title);
        QString tip = basePath + QStringLiteral(" ⇄ ") + targetPath;
        for (const auto& c : res->changes)
            if (!c.trivial)
                tip += QStringLiteral("\n• ") + q(c.title);
        tabs_->setTabToolTip(tabs_->indexOf(view), tip);
        tabs_->setCurrentWidget(view);
        pushHistory(before);
        finishProgress(tr("%1 ⇄ %2: %n semantic change(s)", nullptr, static_cast<int>(res->changes.size()))
                           .arg(basePath, targetPath));
    });
    watcher->setFuture(QtConcurrent::run([st, cf, options]() -> Result {
        auto& s = *st->session;
        return std::make_shared<cr::ReviewResult>(cr::computeReview({cf}, s.snapshot(cr::Side::Old), s.snapshot(cr::Side::New),
                                                                     s.project(cr::Side::Old), s.project(cr::Side::New),
                                                                     options, {}, &st->cancel));
    }));
}

void MainWindow::showFilesMenu(QPoint pos)
{
    if (!state_ || !state_->result)
        return;
    const auto& files = state_->result->files;
    auto fileOf = [&](QTreeWidgetItem* it) -> const cr::FileDiff* {
        const int f = it->data(0, RoleFile).toInt();
        return f >= 0 && f < static_cast<int>(files.size()) ? &files[static_cast<size_t>(f)] : nullptr;
    };
    auto items = files_->selectedItems();
    if (items.isEmpty())
        if (auto* it = files_->itemAt(pos))
            items = {it};
    QMenu menu;
    addCopyPathActions(menu, items, fileOf);
    // Interdiff rows are patches, not files of a revision: nothing to compare them with.
    if (!state_->session || (state_->prs && state_->result == state_->prs->interdiff)) {
        if (!menu.isEmpty())
            menu.exec(files_->viewport()->mapToGlobal(pos));
        return;
    }
    if (!menu.isEmpty())
        menu.addSeparator();
    const int copyActions = static_cast<int>(menu.actions().size());
    if (items.size() == 2) {
        // Base of one against the target of the other, both ways when possible.
        const auto* a = fileOf(items[0]);
        const auto* b = fileOf(items[1]);
        for (auto [x, y] : {std::pair{a, b}, std::pair{b, a}})
            if (x && y && !x->oldPath.empty() && !y->newPath.empty()) {
                const QString basePath = q(x->oldPath), targetPath = q(y->newPath);
                menu.addAction(tr("Compare Base %1 with Target %2").arg(QFileInfo(basePath).fileName(), QFileInfo(targetPath).fileName()),
                               this, [this, basePath, targetPath] { compareFiles(basePath, targetPath); });
            }
    } else if (items.size() == 1) {
        const auto* fd = fileOf(items[0]);
        if (fd && !fd->oldPath.empty()) {
            auto* sub = menu.addMenu(tr("Compare Base %1 With Target…").arg(QFileInfo(q(fd->oldPath)).fileName()));
            const QString basePath = q(fd->oldPath);
            for (const auto& other : files)
                if (!other.newPath.empty() && other.newPath != fd->newPath)
                    sub->addAction(q(other.newPath), this, [this, basePath, p = q(other.newPath)] { compareFiles(basePath, p); });
            sub->addSeparator();
            sub->addAction(tr("Other File…"), this, [this, basePath] {
                QuickOpenDialog dlg(fileListProvider(), cr::Side::New, this);
                dlg.lockSide(cr::Side::New);
                dlg.setWindowTitle(tr("Compare %1 With Target File").arg(QFileInfo(basePath).fileName()));
                if (dlg.exec() == QDialog::Accepted)
                    compareFiles(basePath, dlg.path());
            });
        }
        if (fd && !fd->newPath.empty()) {
            auto* sub = menu.addMenu(tr("Compare Target %1 With Base…").arg(QFileInfo(q(fd->newPath)).fileName()));
            const QString targetPath = q(fd->newPath);
            for (const auto& other : files)
                if (!other.oldPath.empty() && other.oldPath != fd->oldPath)
                    sub->addAction(q(other.oldPath), this, [this, targetPath, p = q(other.oldPath)] { compareFiles(p, targetPath); });
            sub->addSeparator();
            sub->addAction(tr("Other File…"), this, [this, targetPath] {
                QuickOpenDialog dlg(fileListProvider(), cr::Side::Old, this);
                dlg.lockSide(cr::Side::Old);
                dlg.setWindowTitle(tr("Compare %1 With Base File").arg(QFileInfo(targetPath).fileName()));
                if (dlg.exec() == QDialog::Accepted)
                    compareFiles(dlg.path(), targetPath);
            });
        }
    }
    if (menu.actions().size() == copyActions)
        menu.addAction(tr("Select one or two files to compare"))->setEnabled(false);
    menu.addSeparator();
    menu.addAction(tr("Compare Two Files…"), this, &MainWindow::pickFilesToCompare);
    menu.exec(files_->viewport()->mapToGlobal(pos));
}

void MainWindow::addCopyPathActions(QMenu& menu, const QList<QTreeWidgetItem*>& items,
                                    const std::function<const cr::FileDiff*(QTreeWidgetItem*)>& fileOf)
{
    // Paths of the selected files: the target path, or the base path of a deleted file.
    QStringList relative, absolute, basePaths;
    for (auto* it : items) {
        const auto* fd = fileOf(it);
        if (!fd)
            continue;
        const bool hasNew = !fd->newPath.empty();
        const QString path = q(hasNew ? fd->newPath : fd->oldPath);
        relative << path;
        if (!fd->oldPath.empty() && fd->oldPath != fd->newPath && hasNew)
            basePaths << q(fd->oldPath);
        // Absolute only when the file is on disk: a working tree or a compared directory.
        const QString root = !hasNew && base_.kind == cr::Revision::Kind::Directory ? q(base_.ref)
                             : target_.kind == cr::Revision::Kind::Directory       ? q(target_.ref)
                             : repo_                                              ? q(repo_->root())
                                                                                  : QString();
        const QString abs = root.isEmpty() ? QString() : QDir(root).absoluteFilePath(path);
        if (!abs.isEmpty() && QFileInfo::exists(abs))
            absolute << abs;
    }
    if (relative.isEmpty())
        return;
    auto copy = [](const QStringList& paths) { QGuiApplication::clipboard()->setText(paths.join(QLatin1Char('\n'))); };
    const bool many = relative.size() > 1;
    menu.addAction(many ? tr("Copy Paths") : tr("Copy Path"), this, [copy, relative] { copy(relative); });
    if (absolute.size() == relative.size())
        menu.addAction(many ? tr("Copy Absolute Paths") : tr("Copy Absolute Path"), this, [copy, absolute] { copy(absolute); });
    if (!basePaths.isEmpty())
        menu.addAction(basePaths.size() > 1 ? tr("Copy Base Paths") : tr("Copy Base Path"), this,
                       [copy, basePaths] { copy(basePaths); });
    menu.addAction(many ? tr("Copy File Names") : tr("Copy File Name"), this, [copy, relative] {
        QStringList names;
        for (const auto& p : relative)
            names << QFileInfo(p).fileName();
        copy(names);
    });
}

void MainWindow::showChangesMenu(QPoint pos)
{
    if (changes_->selectedItems().isEmpty())
        if (auto* it = changes_->itemAt(pos))
            changes_->setCurrentItem(it);
    if (changes_->selectedItems().isEmpty())
        return;
    QMenu menu;
    menu.addAction(tr("Copy\tCtrl+C"), this, [this] { copyChanges(false); });
    menu.addAction(tr("Copy with Details\tCtrl+Shift+C"), this, [this] { copyChanges(true); });
    menu.exec(changes_->viewport()->mapToGlobal(pos));
}

QString MainWindow::changeItemText(QTreeWidgetItem* it, bool details) const
{
    QString text = it->text(0);
    if (!details || !state_ || !state_->result)
        return text;
    bool ok = false;
    const int c = it->data(0, RoleChange).toInt(&ok);
    const auto& changes = state_->result->changes;
    if (!ok || c < 0 || c >= static_cast<int>(changes.size())) {
        // A group heading: everything in it.
        for (int i = 0; i < it->childCount(); ++i)
            text += QStringLiteral("\n\n") + changeItemText(it->child(i), true);
        return text;
    }
    const auto& ch = changes[static_cast<size_t>(c)];
    bool isOcc = false;
    const int occ = it->data(0, RoleOccurrence).toInt(&isOcc);
    if (isOcc && occ >= 0 && occ < static_cast<int>(ch.related.size())) {
        // An occurrence: its full locations, and the change it belongs to.
        const auto& item = ch.related[static_cast<size_t>(occ)];
        text = text.trimmed();
        if (item.oldLoc.valid())
            text += QStringLiteral("\n  Before: ") + locText(item.oldLoc);
        if (item.newLoc.valid())
            text += QStringLiteral("\n  After: ") + locText(item.newLoc);
        text += QStringLiteral("\n  In: ") + q(ch.title);
        return text;
    }
    if (ch.oldLoc.valid())
        text += QStringLiteral("\n  Before: ") + locText(ch.oldLoc);
    if (ch.newLoc.valid())
        text += QStringLiteral("\n  After: ") + locText(ch.newLoc);
    if (!ch.detail.empty())
        text += QStringLiteral("\n  ") + q(ch.detail);
    for (int i = 0; i < it->childCount(); ++i)
        text += QStringLiteral("\n    ") + it->child(i)->text(0).trimmed();
    return text;
}

void MainWindow::copyChanges(bool details)
{
    // In tree order, whatever order they were selected in.
    QStringList parts;
    for (QTreeWidgetItemIterator i(changes_, QTreeWidgetItemIterator::Selected); *i; ++i)
        parts << changeItemText(*i, details);
    if (!parts.isEmpty())
        QGuiApplication::clipboard()->setText(parts.join(details ? QStringLiteral("\n\n") : QStringLiteral("\n")));
}

// ------------------------------------------------------------------------------------ search

CodeView* MainWindow::activeCodeView() const
{
    auto* w = tabs_->currentWidget();
    if (auto* d = qobject_cast<DiffView*>(w)) {
        if (lastCodeView_ && (lastCodeView_ == d->view(cr::Side::Old) || lastCodeView_ == d->view(cr::Side::New)))
            return lastCodeView_;
        return d->view(cr::Side::New);
    }
    return qobject_cast<CodeView*>(w);
}

void MainWindow::applySearch()
{
    const QString text = findBar_->text();
    const auto flags = findBar_->flags();
    auto* target = activeCodeView();
    // Both sides of the current diff show the matches; the active side is searched.
    auto* d = qobject_cast<DiffView*>(tabs_->currentWidget());
    for (auto* v : findChildren<CodeView*>()) {
        const bool here = v == target || (d && (v == d->view(cr::Side::Old) || v == d->view(cr::Side::New)));
        v->setSearch(here ? text : QString(), flags);
    }
    if (!target) {
        findBar_->setMatchInfo(0, false);
        return;
    }
    bool found = false;
    if (!text.isEmpty()) {
        // Incremental: keep the current match if it still matches.
        auto c = target->textCursor();
        c.setPosition(c.selectionStart());
        target->setTextCursor(c);
        found = target->findNext(false);
    }
    findBar_->setMatchInfo(target->searchMatchCount(), found);
}

void MainWindow::findInView(bool backward)
{
    if (!findBar_->isVisible() || findBar_->text().isEmpty()) {
        auto* v = activeCodeView();
        findBar_->activate(v ? v->textCursor().selectedText() : QString());
        return;
    }
    auto* v = activeCodeView();
    if (!v)
        return;
    const bool found = v->findNext(backward);
    findBar_->setMatchInfo(v->searchMatchCount(), found);
}

void MainWindow::closeFindBar()
{
    findBar_->hide();
    for (auto* v : findChildren<CodeView*>())
        v->setSearch({}, {});
    if (auto* v = activeCodeView())
        v->setFocus();
}

void MainWindow::startFileSearch()
{
    if (!state_ || !state_->session || !state_->result)
        return;
    if (searchCancel_)
        *searchCancel_ = true;
    searchCancel_ = std::make_shared<std::atomic<bool>>(false);
    searchResults_->clear();

    SearchQuery query;
    query.text = searchEdit_->text();
    query.caseSensitive = searchCase_->isChecked();
    query.wholeWord = searchWord_->isChecked();
    query.regex = searchRegex_->isChecked();
    if (query.text.isEmpty())
        return;
    const int scope = searchScope_->currentIndex();
    std::vector<SearchTarget> changed;
    if (scope == 0) {
        SearchTarget oldT{cr::Side::Old, state_->session->snapshot(cr::Side::Old).root, {}};
        SearchTarget newT{cr::Side::New, state_->session->snapshot(cr::Side::New).root, {}};
        for (const auto& f : state_->result->files) {
            if (!f.newPath.empty())
                newT.files.push_back(f.newPath);
            else if (!f.oldPath.empty())
                oldT.files.push_back(f.oldPath);
        }
        changed = {newT, oldT};
    }
    searchInfo_->setText(tr("Searching…"));

    struct Outcome {
        std::vector<SearchHit> hits;
        bool truncated = false;
        QString error;
    };
    auto st = state_;
    auto cancel = searchCancel_;
    auto* watcher = new QFutureWatcher<Outcome>(this);
    connect(watcher, &QFutureWatcher<Outcome>::finished, this, [this, watcher, st, cancel] {
        watcher->deleteLater();
        if (st != state_ || *cancel)
            return;
        auto out = watcher->result();
        if (!out.error.isEmpty()) {
            searchInfo_->setText(tr("<span style='color:#cf222e'>%1</span>").arg(esc(out.error)));
            return;
        }
        const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        QTreeWidgetItem* group = nullptr;
        QString groupKey;
        int files = 0;
        for (const auto& h : out.hits) {
            const QString key = QString::number(static_cast<int>(h.side)) + h.path;
            if (!group || key != groupKey) {
                group = new QTreeWidgetItem(searchResults_, {h.path + (h.side == cr::Side::Old ? tr(" (base)") : QString())});
                group->setData(0, RoleOccurrence, 0);
                QFont bold = group->font(0);
                bold.setBold(true);
                group->setFont(0, bold);
                group->setExpanded(true);
                groupKey = key;
                ++files;
            }
            auto* it = new QTreeWidgetItem(group, {QStringLiteral("%1: %2").arg(h.line, 5).arg(h.text.trimmed())});
            it->setFont(0, mono);
            it->setData(0, RoleChange, static_cast<int>(h.side));
            it->setData(0, RoleFile, h.path);
            it->setData(0, RoleOccurrence, h.line);
            it->setToolTip(0, h.path + QLatin1Char(':') + QString::number(h.line));
        }
        for (int i = 0; i < searchResults_->topLevelItemCount(); ++i) {
            auto* g = searchResults_->topLevelItem(i);
            g->setText(0, g->text(0) + QStringLiteral("  (%1)").arg(g->childCount()));
        }
        searchInfo_->setText(tr("%1 in %2%3")
                                 .arg(tr("%n match(es)", nullptr, static_cast<int>(out.hits.size())))
                                 .arg(tr("%n file(s)", nullptr, files))
                                 .arg(out.truncated ? tr(" (stopped at the limit)") : QString()));
    });
    watcher->setFuture(QtConcurrent::run([st, cancel, query, scope, changed] {
        std::vector<SearchTarget> targets = changed;
        if (scope != 0) {
            const auto side = scope == 1 ? cr::Side::New : cr::Side::Old;
            targets.push_back({side, st->session->snapshot(side).root, *filesOf(*st, side)});
        }
        Outcome out;
        out.hits = searchFiles(targets, query, *cancel, 5000, out.truncated, &out.error);
        return out;
    }));
}

// ------------------------------------------------------------------------------------ bookmarks

void MainWindow::onBookmarkRequested(CodeView* v, int line, BookmarkAction action)
{
    if (!v || v->path().isEmpty() || line < 0)
        return;
    const QString path = v->path();
    const auto side = QFileInfo(path).isAbsolute() ? cr::Side::New : v->side();
    int index = bookmarks_.indexOf(side, path, line);
    if (action == BookmarkAction::Remove || (action == BookmarkAction::Toggle && index >= 0)) {
        bookmarks_.remove(index);
        statusLabel_->setText(tr("Bookmark removed"));
    } else if (index < 0) {
        Bookmark b;
        b.side = side;
        b.path = path;
        b.line = line;
        const auto lines = fileLines(side, path);
        if (static_cast<size_t>(line) < lines.size())
            b.text = q(lines[static_cast<size_t>(line)]);
        b.revision = q(side == cr::Side::Old ? base_.display() : target_.display());
        b.created = QDateTime::currentDateTime();
        bookmarks_.add(b);
        index = bookmarks_.indexOf(side, path, line);
        statusLabel_->setText(tr("Bookmarked %1:%2").arg(path).arg(line + 1));
    }
    if (action == BookmarkAction::EditComment && index >= 0)
        editBookmarkComment(index); // repopulates
    else
        populateBookmarks();
}

void MainWindow::bookmarkAtCursor(BookmarkAction action)
{
    auto* v = activeCodeView();
    const int line = v ? v->lineForRow(v->cursorRow()) : -1;
    if (line < 0) {
        statusLabel_->setText(tr("Put the cursor on a code line to bookmark it"));
        return;
    }
    onBookmarkRequested(v, line, action);
}

void MainWindow::editBookmarkComment(int index)
{
    if (index < 0 || index >= bookmarks_.items().size())
        return;
    const auto& b = bookmarks_.items()[index];
    bool ok = false;
    const QString text = QInputDialog::getMultiLineText(
        this, tr("Comment"),
        tr("<b>%1:%2</b>%3<br><code>%4</code>")
            .arg(esc(b.path))
            .arg(b.line + 1)
            .arg(b.side == cr::Side::Old ? tr(" (base)") : QString(), esc(b.text.trimmed().left(120))),
        b.comment, &ok);
    if (ok)
        bookmarks_.setComment(index, text.trimmed());
    populateBookmarks();
}

void MainWindow::populateBookmarks()
{
    bookmarkList_->clear();
    const auto dim = palette().color(QPalette::PlaceholderText);
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    const auto& items = bookmarks_.items();
    for (int i = 0; i < items.size(); ++i) {
        const auto& b = items[i];
        QString where = QFileInfo(b.path).fileName() + QLatin1Char(':') + QString::number(b.line + 1);
        if (b.side == cr::Side::Old)
            where += tr(" (base)");
        auto* it = new QTreeWidgetItem(bookmarkList_, {where, b.comment.section(QLatin1Char('\n'), 0, 0), b.text.trimmed()});
        it->setData(0, RoleOccurrence, i);
        it->setFont(2, mono);
        QString tip = QStringLiteral("<b>%1:%2</b>").arg(esc(b.path)).arg(b.line + 1);
        if (!b.revision.isEmpty())
            tip += tr("<br>made on %1").arg(esc(b.revision));
        if (!b.comment.isEmpty())
            tip += QStringLiteral("<br><br>") + esc(b.comment).replace(QLatin1Char('\n'), QStringLiteral("<br>"));
        if (b.stale) {
            tip += tr("<br><i>This line isn't in the current revisions any more.</i>");
            for (int c = 0; c < 3; ++c)
                it->setForeground(c, dim);
            it->setText(0, QStringLiteral("⚠ ") + where);
        }
        for (int c = 0; c < 3; ++c)
            it->setToolTip(c, tip);
    }
    bookmarkList_->resizeColumnToContents(0);
    bookmarksDock_->setWindowTitle(items.isEmpty() ? tr("Bookmarks") : tr("Bookmarks (%1)").arg(items.size()));
    for (auto* v : findChildren<CodeView*>())
        v->refreshMarks();
}

void MainWindow::gotoBookmark(int index)
{
    if (index < 0 || index >= bookmarks_.items().size())
        return;
    const auto& b = bookmarks_.items()[index];
    bookmarkCursor_ = index;
    if (b.stale) {
        statusLabel_->setText(tr("%1:%2 isn't in the current revisions").arg(b.path).arg(b.line + 1));
        return;
    }
    cr::Location loc;
    loc.file = b.path.toStdString();
    loc.line = b.line + 1;
    loc.external = QFileInfo(b.path).isAbsolute();
    navigateTo(b.side, loc);
    statusLabel_->setText(b.comment.isEmpty() ? tr("Bookmark %1 of %2").arg(index + 1).arg(bookmarks_.items().size())
                                              : b.comment.section(QLatin1Char('\n'), 0, 0));
}

void MainWindow::stepBookmark(bool forward)
{
    const int n = static_cast<int>(bookmarks_.items().size());
    for (int k = 1; k <= n; ++k) {
        const int i = ((bookmarkCursor_ < 0 ? (forward ? -1 : 0) : bookmarkCursor_) + (forward ? k : -k) + n * 2) % n;
        if (!bookmarks_.items()[i].stale) {
            gotoBookmark(i);
            return;
        }
    }
    statusLabel_->setText(tr("No bookmarks"));
}

} // namespace gui
