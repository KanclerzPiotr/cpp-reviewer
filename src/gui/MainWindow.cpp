#include "MainWindow.hpp"

#include "DiffView.hpp"
#include "RevisionDialog.hpp"
#include "Theme.hpp"
#include "core/Diff.hpp"

#include <QActionGroup>
#include <QApplication>
#include <QStyle>
#include <QStyleFactory>
#include <QCheckBox>
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
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTextBlock>
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
    restoreState(settings.value(QStringLiteral("windowState")).toByteArray());
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

    tb->addAction(tr("Open…"), this, [this] {
        auto dir = QFileDialog::getExistingDirectory(this, tr("Open Git Repository"));
        if (!dir.isEmpty() && openRepository(dir))
            startReview();
    });
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
    setCentralWidget(tabs_);
    connect(diff_, &DiffView::definitionRequested, this, &MainWindow::onDefinitionRequested);
    connect(diff_, &DiffView::hoverRequested, this, &MainWindow::onHoverRequested);
    connect(diff_, &DiffView::changeActivated, this, &MainWindow::openChangeComparison);

    // Files dock.
    files_ = new QTreeWidget;
    files_->setHeaderLabels({tr(""), tr("File"), tr("+/−"), tr("Δ")});
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
    filterRow->addWidget(changeFilter_, 1);
    filterRow->addWidget(hideTrivial_);
    cl->addLayout(filterRow);
    changes_ = new QTreeWidget;
    changes_->setHeaderHidden(true);
    changes_->setUniformRowHeights(true);
    changes_->setToolTip(tr("Click to show, double-click to compare with the original code"));
    cl->addWidget(changes_, 1);
    connect(changeFilter_, &QLineEdit::textChanged, this, &MainWindow::filterChanges);
    connect(hideTrivial_, &QCheckBox::toggled, this, &MainWindow::filterChanges);
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
    view->addSeparator();
    view->addAction(tr("&Clear Highlights"), QKeySequence(Qt::Key_Escape), this, [this] { setWordHighlight({}, {}); });
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

void MainWindow::setTheme(const QString& mode)
{
    QSettings().setValue(QStringLiteral("theme"), mode);
    applyAppTheme(mode);
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

void MainWindow::setRevisions(cr::Revision base, cr::Revision target, bool start)
{
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
    struct Fetched {
        QString error;
        std::string head, base;
    };
    showProgress(tr("Fetching pull request #%1 from %2…").arg(pr.number).arg(pr.remote), 0, 0);
    auto repo = *repo_;
    auto* watcher = new QFutureWatcher<Fetched>(this);
    connect(watcher, &QFutureWatcher<Fetched>::finished, this, [this, watcher, pr] {
        watcher->deleteLater();
        auto f = watcher->result();
        if (!f.error.isEmpty()) {
            finishProgress(tr("Fetching the pull request failed"));
            QMessageBox::warning(this, tr("Pull Request"), f.error);
            return;
        }
        auto title = QStringLiteral("PR #%1").arg(pr.number);
        if (!pr.title.isEmpty())
            title += QStringLiteral(": ") + pr.title.left(40);
        setRevisions(cr::Revision::commit(f.base, f.base, ("merge-base with " + pr.remote + "/" + pr.baseRef).toStdString()),
                     cr::Revision::commit(f.head, f.head, title.toStdString()));
    });
    watcher->setFuture(QtConcurrent::run([repo, pr]() {
        Fetched f;
        std::string err;
        const auto remote = pr.remote.toStdString();
        f.head = repo.fetchPullRequest(remote, pr.number, &err);
        if (f.head.empty()) {
            f.error = tr("Could not fetch pull/%1/head from %2:\n%3").arg(pr.number).arg(pr.remote, q(err));
            return f;
        }
        auto baseHead = repo.fetchBranch(remote, pr.baseRef.toStdString(), &err);
        if (baseHead.empty()) {
            f.error = tr("Could not fetch base branch %1:\n%2").arg(pr.baseRef, q(err));
            return f;
        }
        f.base = repo.mergeBase(baseHead, f.head);
        if (f.base.empty())
            f.error = tr("No merge base between %1 and the pull request.").arg(pr.baseRef);
        return f;
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

void MainWindow::startReview()
{
    if (!haveRevisions_)
        return;
    if (state_)
        state_->cancel = true;

    auto st = std::make_shared<State>();
    st->session = std::make_shared<cr::ReviewSession>(repo_, base_, target_);
    if (!compileDbOverride_.isEmpty())
        st->session->setCompileDatabasePath(compileDbOverride_.toStdString());
    state_ = st;
    lineCache_.clear();
    back_.clear();
    forward_.clear();
    backAction_->setEnabled(false);
    forwardAction_->setEnabled(false);
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
    diff_->showDiff(cr::FileDiff{}, tr("Computing review…"));
    updateTitle();

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
            finishProgress(tr("Review failed"));
            diff_->showDiff(cr::FileDiff{}, tr("<b>Review failed:</b> %1").arg(esc(out.error)));
            return;
        }
        st->result = out.result;
        const auto dbPath = q(st->session->compileDatabasePath());
        dbLabel_->setText(dbPath.isEmpty() ? tr("⚠ no compile_commands.json (fallback flags)")
                                           : tr("compile_commands: %1").arg(QFileInfo(dbPath).dir().dirName() +
                                                                              QStringLiteral("/") +
                                                                              QFileInfo(dbPath).fileName()));
        dbLabel_->setToolTip(dbPath.isEmpty()
                                 ? tr("Without a compilation database includes and macros may not resolve. "
                                      "Configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON or set one in the Review menu.")
                                 : dbPath);
        diff_->setSideCaptions(tr("Base: %1").arg(q(base_.display())), tr("Target: %1").arg(q(target_.display())));
        populateFiles();
        populateChanges();
        int changes = 0;
        for (const auto& c : st->result->changes)
            changes += c.trivial ? 0 : 1;
        finishProgress(tr("%n file(s) changed", nullptr, static_cast<int>(st->result->files.size())) +
                       tr(", %n semantic change(s)", nullptr, changes));
        if (files_->topLevelItemCount() > 0)
            files_->setCurrentItem(files_->topLevelItem(0));
        else
            diff_->showDiff(cr::FileDiff{}, tr("No differences."));
        startIndexing(st);
        emit reviewFinished();
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

void MainWindow::startIndexing(const std::shared_ptr<State>& st)
{
    if (!semantic_->isChecked())
        return;
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
            if (guard && guard->state_ == st && !st->cancel)
                guard->statusLabel_->setText(tr("Symbol index ready — Ctrl+Click or F12 to go to definition"));
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
        QColor c = f.change.status == 'A' ? theme.changeColor(cr::ChangeKind::Added)
                 : f.change.status == 'D' ? theme.changeColor(cr::ChangeKind::Removed)
                 : f.change.status == 'R' ? theme.changeColor(cr::ChangeKind::Moved)
                                          : theme.changeColor(cr::ChangeKind::Modified);
        it->setForeground(0, c);
        QFont bold = it->font(0);
        bold.setBold(true);
        it->setFont(0, bold);
        it->setToolTip(1, path);
        if (f.oldErrors || f.newErrors)
            it->setToolTip(3, tr("libclang reported %1 error(s) in base and %2 in target; semantic results may be incomplete")
                                  .arg(f.oldErrors)
                                  .arg(f.newErrors));
    }
}

void MainWindow::populateChanges()
{
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
            addRelatedItems(it, c);
        }
        // Plain modifications/additions/removals can be numerous; keep them collapsed.
        group->setExpanded(kind != cr::ChangeKind::Modified && kind != cr::ChangeKind::Added &&
                           kind != cr::ChangeKind::Removed);
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
                        !(hideTrivial && state_->result->changes[static_cast<size_t>(c)].trivial);
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
    // Start at the first change.
    for (size_t r = 0; r < fd.rows.size(); ++r)
        if (fd.rows[r].kind != cr::RowKind::Equal) {
            diff_->view(cr::Side::New)->scrollToRow(static_cast<int>(r), false);
            diff_->view(cr::Side::Old)->scrollToRow(static_cast<int>(r), false);
            break;
        }
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
    auto slice = [](const std::vector<std::string>& lines, const cr::Location& l) {
        int b = std::clamp(l.line - 1, 0, static_cast<int>(lines.size()));
        int e = std::clamp(std::max(l.line, l.endLine), b, static_cast<int>(lines.size()));
        return std::vector<std::string>(lines.begin() + b, lines.begin() + e);
    };
    cr::FileDiff fd;
    fd.oldPath = oldLoc.file;
    fd.newPath = newLoc.file;
    fd.oldLines = slice(fileLines(cr::Side::Old, q(oldLoc.file)), oldLoc);
    fd.newLines = slice(fileLines(cr::Side::New, q(newLoc.file)), newLoc);
    cr::diffLines(fd, ignoreWhitespace_->isChecked());
    // Apply the rename map so that rename-only lines are recognizable here too.
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
    QString title = QStringLiteral("<b>%1</b><br><span style='color:gray'>%2 &nbsp;⇄&nbsp; %3</span>")
                        .arg(esc(heading), esc(locText(oldLoc)), esc(locText(newLoc)));
    view->showDiff(fd, title, oldLoc.line - 1, newLoc.line - 1);
    view->setSideCaptions(tr("Before: %1").arg(locText(oldLoc)), tr("After: %1").arg(locText(newLoc)));
    auto before = currentPosition();
    tabs_->addTab(view, tabTitle);
    tabs_->setTabToolTip(tabs_->indexOf(view), heading);
    tabs_->setCurrentWidget(view);
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

} // namespace gui
