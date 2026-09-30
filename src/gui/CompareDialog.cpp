#include "CompareDialog.hpp"

#include "GitHubClient.hpp"

#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QStackedWidget>
#include <QToolButton>
#include <QHeaderView>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace gui {

namespace {

QString q(const std::string& s)
{
    return QString::fromStdString(s);
}

// A line edit with a "…" button next to it.
QWidget* withButton(QLineEdit* edit, const QString& text, const QString& tip, QToolButton** button)
{
    auto* w = new QWidget;
    auto* l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->addWidget(edit, 1);
    *button = new QToolButton;
    (*button)->setText(text);
    (*button)->setToolTip(tip);
    l->addWidget(*button);
    return w;
}

QString settingsKey(const QString& repo, const char* what)
{
    return QStringLiteral("compare/") + QString(repo).replace(QLatin1Char('/'), QLatin1Char('\\')) +
           QLatin1Char('/') + QLatin1String(what);
}

} // namespace

CompareDialog::CompareDialog(const QString& repoPath, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("New Comparison — C++ Reviewer"));
    setMinimumWidth(640);
    auto* outer = new QVBoxLayout(this);
    tabs_ = new QTabWidget;
    outer->addWidget(tabs_);
    sessionsPage_ = buildSessionsPage();
    tabs_->addTab(sessionsPage_, tr("Recent &Sessions"));
    auto* newPage = new QWidget;
    tabs_->addTab(newPage, tr("&New Comparison"));
    auto* layout = new QVBoxLayout(newPage);

    // Repository.
    auto* repoForm = new QFormLayout;
    repoEdit_ = new QLineEdit;
    repoEdit_->setPlaceholderText(tr("Path to a git repository"));
    QToolButton* browseRepo = nullptr;
    repoForm->addRow(tr("&Repository:"), withButton(repoEdit_, tr("…"), tr("Choose a directory"), &browseRepo));
    repoInfo_ = new QLabel;
    repoInfo_->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
    repoForm->addRow(QString(), repoInfo_);
    layout->addLayout(repoForm);

    // Mode.
    auto* modes = new QHBoxLayout;
    modes->addWidget(new QLabel(tr("Compare:")));
    revisionsMode_ = new QRadioButton(tr("Two &revisions"));
    prMode_ = new QRadioButton(tr("&Pull request"));
    prsMode_ = new QRadioButton(tr("Two p&ull requests"));
    prsMode_->setToolTip(tr("E.g. the same change in two repositories: compare what the two pull requests do"));
    dirsMode_ = new QRadioButton(tr("Two &directories"));
    auto* group = new QButtonGroup(this);
    for (auto* b : {revisionsMode_, prMode_, prsMode_, dirsMode_}) {
        group->addButton(b);
        modes->addWidget(b);
    }
    modes->addStretch(1);
    layout->addLayout(modes);
    revisionsMode_->setChecked(true);

    pages_ = new QStackedWidget;
    layout->addWidget(pages_);

    // Page 0: two revisions.
    auto* revPage = new QWidget;
    auto* revLayout = new QVBoxLayout(revPage);
    revLayout->setContentsMargins(0, 0, 0, 0);
    auto* revForm = new QFormLayout;
    baseEdit_ = new QLineEdit;
    targetEdit_ = new QLineEdit;
    baseEdit_->setPlaceholderText(tr("e.g. HEAD~1, main, v1.2, a sha"));
    targetEdit_->setPlaceholderText(tr("e.g. HEAD, WORKTREE (uncommitted), INDEX (staged)"));
    QToolButton* browseBase = nullptr;
    QToolButton* browseTarget = nullptr;
    revForm->addRow(tr("&Base (before):"), withButton(baseEdit_, tr("Browse…"), tr("Pick from the log, branches and tags"), &browseBase));
    revForm->addRow(tr("&Target (after):"), withButton(targetEdit_, tr("Browse…"), tr("Pick from the log, branches and tags"), &browseTarget));
    revLayout->addLayout(revForm);
    auto* presets = new QHBoxLayout;
    presets->addWidget(new QLabel(tr("Presets:")));
    for (auto [id, label] : {std::pair{"worktree", QT_TR_NOOP("Uncommitted")}, std::pair{"index", QT_TR_NOOP("Staged")},
                             std::pair{"last", QT_TR_NOOP("Last commit")},
                             std::pair{"branch", QT_TR_NOOP("Branch vs. default branch")}}) {
        auto* b = new QPushButton(tr(label));
        b->setAutoDefault(false);
        const QString preset = QLatin1String(id);
        connect(b, &QPushButton::clicked, this, [this, preset] { applyPreset(preset); });
        presets->addWidget(b);
    }
    presets->addStretch(1);
    revLayout->addLayout(presets);
    pages_->addWidget(revPage);

    // Page 1: pull request.
    github_ = new GitHubClient(this);
    auto* prPage = new QWidget;
    auto* prForm = new QFormLayout(prPage);
    prForm->setContentsMargins(0, 0, 0, 0);
    linkEdit_ = new QLineEdit;
    linkEdit_->setPlaceholderText(tr("Paste a link, e.g. https://github.com/owner/repo/pull/123"));
    linkEdit_->setClearButtonEnabled(true);
    prForm->addRow(tr("&Link:"), linkEdit_);
    auto* prRow = new QHBoxLayout;
    prLabel_ = new QLabel(tr("No pull request chosen"));
    prLabel_->setWordWrap(true);
    auto* choosePr = new QPushButton(tr("Choose from List…"));
    choosePr->setAutoDefault(false);
    prRow->addWidget(prLabel_, 1);
    prRow->addWidget(choosePr);
    prForm->addRow(QString(), prRow);
    pages_->addWidget(prPage);

    // Page 2: directories.
    auto* dirPage = new QWidget;
    auto* dirForm = new QFormLayout(dirPage);
    dirForm->setContentsMargins(0, 0, 0, 0);
    oldDirEdit_ = new QLineEdit;
    newDirEdit_ = new QLineEdit;
    QToolButton* browseOld = nullptr;
    QToolButton* browseNew = nullptr;
    dirForm->addRow(tr("&Old (base):"), withButton(oldDirEdit_, tr("…"), tr("Choose a directory"), &browseOld));
    dirForm->addRow(tr("&New (target):"), withButton(newDirEdit_, tr("…"), tr("Choose a directory"), &browseNew));
    pages_->addWidget(dirPage);

    // Page 3: two pull requests.
    auto* prsPage = new QWidget;
    auto* prsForm = new QFormLayout(prsPage);
    prsForm->setContentsMargins(0, 0, 0, 0);
    linkAEdit_ = new QLineEdit;
    linkBEdit_ = new QLineEdit;
    mappingEdit_ = new QLineEdit;
    linkAEdit_->setPlaceholderText(tr("https://github.com/owner/repo/pull/123"));
    linkBEdit_->setPlaceholderText(tr("the same change elsewhere, e.g. in the open-source repository"));
    mappingEdit_->setPlaceholderText(tr("automatic; or A-prefix=B-prefix, e.g. src/lib/=lib/"));
    for (auto* e : {linkAEdit_, linkBEdit_, mappingEdit_})
        e->setClearButtonEnabled(true);
    prsForm->addRow(tr("Pull request &A:"), linkAEdit_);
    prsForm->addRow(tr("Pull request &B:"), linkBEdit_);
    prsForm->addRow(tr("Path &mapping:"), mappingEdit_);
    auto* prsNote = new QLabel(tr("Fetched into the repository above when one of its remotes points to the pull "
                                  "request's repository, otherwise downloaded (just the needed commits) into the cache."));
    prsNote->setWordWrap(true);
    prsNote->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
    prsForm->addRow(QString(), prsNote);
    pages_->addWidget(prsPage);

    layout->addStretch(1);
    error_ = new QLabel;
    error_->setStyleSheet(QStringLiteral("color: #cf222e;"));
    error_->setWordWrap(true);
    outer->addWidget(error_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    ok_ = buttons->button(QDialogButtonBox::Ok);
    outer->addWidget(buttons);
    connect(tabs_, &QTabWidget::currentChanged, this, &CompareDialog::updateButtons);
    connect(buttons, &QDialogButtonBox::accepted, this, &CompareDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &CompareDialog::reject);

    connect(browseRepo, &QToolButton::clicked, this, [this] {
        auto dir = QFileDialog::getExistingDirectory(this, tr("Git Repository"), repoEdit_->text());
        if (!dir.isEmpty())
            openRepo(dir);
    });
    connect(repoEdit_, &QLineEdit::editingFinished, this, [this] { openRepo(repoEdit_->text()); });
    connect(browseBase, &QToolButton::clicked, this, [this] { pickRevision(cr::Side::Old); });
    connect(browseTarget, &QToolButton::clicked, this, [this] { pickRevision(cr::Side::New); });
    connect(baseEdit_, &QLineEdit::textEdited, this, [this] { picked_[0].reset(); });
    connect(targetEdit_, &QLineEdit::textEdited, this, [this] { picked_[1].reset(); });
    connect(choosePr, &QPushButton::clicked, this, [this] {
        if (!repo_)
            return;
        PullRequestDialog dlg(*repo_, this);
        if (dlg.exec() != QDialog::Accepted)
            return;
        pr_ = dlg.choice();
        linkEdit_->clear();
        prLabel_->setText(tr("#%1 %2  (%3, base %4)")
                              .arg(pr_->number)
                              .arg(pr_->title, pr_->remote, pr_->baseRef.isEmpty() ? tr("its target branch") : pr_->baseRef));
    });
    connect(linkEdit_, &QLineEdit::textChanged, this, &CompareDialog::setLink);
    for (auto [button, edit] : {std::pair{browseOld, oldDirEdit_}, std::pair{browseNew, newDirEdit_}})
        connect(button, &QToolButton::clicked, this, [this, edit = edit] {
            auto dir = QFileDialog::getExistingDirectory(this, tr("Directory"), edit->text());
            if (!dir.isEmpty())
                edit->setText(dir);
        });
    connect(group, &QButtonGroup::buttonToggled, this, [this] { updateState(); });

    openRepo(repoPath);

    // Recent sessions first; but a pull request link on the clipboard is most likely what to review.
    tabs_->setCurrentWidget(sessions_.isEmpty() ? newPage : sessionsPage_);
    const QString clip = QApplication::clipboard()->text().trimmed();
    if (repo_ && cr::parsePullRequestUrl(clip.toStdString())) {
        tabs_->setCurrentWidget(newPage);
        prMode_->setChecked(true);
        linkEdit_->setText(clip);
    }
    updateButtons();
}

QWidget* CompareDialog::buildSessionsPage()
{
    auto* page = new QWidget;
    auto* l = new QVBoxLayout(page);
    sessionList_ = new QTreeWidget;
    sessionList_->setHeaderLabels({tr("Review"), tr("Repository"), tr("Last used")});
    sessionList_->setRootIsDecorated(false);
    sessionList_->setUniformRowHeights(true);
    sessionList_->header()->setStretchLastSection(false);
    sessionList_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    sessionList_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    sessionList_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    sessionList_->setMinimumHeight(260);
    l->addWidget(sessionList_, 1);
    auto* row = new QHBoxLayout;
    latestButton_ = new QPushButton(tr("Resume with &Latest Changes"));
    latestButton_->setToolTip(tr("Fetch the pull request(s) again instead of reopening the saved commits"));
    latestButton_->setAutoDefault(false);
    auto* removeButton = new QPushButton(tr("&Remove"));
    removeButton->setAutoDefault(false);
    row->addWidget(latestButton_);
    row->addStretch(1);
    row->addWidget(removeButton);
    l->addLayout(row);

    sessions_ = SessionStore::load();
    for (int i = 0; i < sessions_.size(); ++i) {
        const auto& r = sessions_[i];
        const QString repo = r.kind == SessionRecord::Kind::Directories ? QString() : QFileInfo(r.repo).fileName();
        auto* it = new QTreeWidgetItem(sessionList_, {r.title(), repo, r.used.toString(QStringLiteral("yyyy-MM-dd HH:mm"))});
        it->setData(0, Qt::UserRole, i);
        QString tip = r.title();
        if (!r.repo.isEmpty())
            tip += QStringLiteral("\n") + r.repo;
        if (!r.file.isEmpty())
            tip += tr("\nat %1:%2").arg(r.file).arg(r.line);
        if (!r.tabs.isEmpty())
            tip += tr("\n%n open tab(s)", nullptr, static_cast<int>(r.tabs.size()));
        for (int c = 0; c < 3; ++c)
            it->setToolTip(c, tip);
    }
    if (sessionList_->topLevelItemCount() > 0)
        sessionList_->setCurrentItem(sessionList_->topLevelItem(0));
    connect(sessionList_, &QTreeWidget::itemDoubleClicked, this, &CompareDialog::accept);
    connect(sessionList_, &QTreeWidget::currentItemChanged, this, &CompareDialog::updateButtons);
    connect(latestButton_, &QPushButton::clicked, this, [this] {
        choice_.latest = true;
        accept();
    });
    connect(removeButton, &QPushButton::clicked, this, [this] {
        auto* it = sessionList_->currentItem();
        if (!it)
            return;
        SessionStore::remove(sessions_[it->data(0, Qt::UserRole).toInt()].key());
        delete it;
        updateButtons();
    });
    return page;
}

void CompareDialog::updateButtons()
{
    const bool resume = tabs_->currentWidget() == sessionsPage_;
    auto* it = sessionList_->currentItem();
    ok_->setText(resume ? tr("&Resume") : tr("&Compare"));
    ok_->setEnabled(!resume || it);
    const SessionRecord* r = it ? &sessions_[it->data(0, Qt::UserRole).toInt()] : nullptr;
    latestButton_->setEnabled(r && (!r->prLink.isEmpty() || r->kind == SessionRecord::Kind::PullRequests));
}

void CompareDialog::setLink(const QString& text)
{
    if (!repo_)
        return;
    QString note;
    auto pr = pullRequestFromUrl(*repo_, text, &note);
    if (!pr) {
        if (!text.trimmed().isEmpty()) {
            pr_.reset();
            prLabel_->setText(tr("Not a pull request link."));
        }
        return;
    }
    pr_ = *pr;
    prLabel_->setText(note);
    // Title and target branch; without them the base comes from refs/pull/N/merge.
    github_->getPullRequest(pr->host, pr->slug, pr->number, [this, number = pr->number, note](PullRequestInfo info, QString error) {
        if (!pr_ || pr_->number != number)
            return;
        if (!error.isEmpty()) {
            prLabel_->setText(note + QLatin1Char(' ') + tr("(details unavailable: %1)").arg(error));
            return;
        }
        pr_->title = info.title;
        pr_->baseRef = info.baseRef;
        const bool viaRemote = !pr_->remote.contains(QStringLiteral("://"));
        const QString source = viaRemote ? tr("from remote '%1'").arg(pr_->remote) : tr("⚠ from the link (no remote points to it)");
        prLabel_->setText(QStringLiteral("<b>#%1 %2</b><br>%3 ← %4<br>%5")
                              .arg(number)
                              .arg(info.title.toHtmlEscaped(), info.baseRef.toHtmlEscaped(), info.headRef.toHtmlEscaped(), source));
        prLabel_->setToolTip(note);
        adjustSize();
    });
}

void CompareDialog::openRepo(const QString& path)
{
    repo_.reset();
    picked_[0].reset();
    picked_[1].reset();
    pr_.reset();
    prLabel_->setText(tr("No pull request chosen"));
    if (!linkEdit_->text().isEmpty())
        QMetaObject::invokeMethod(this, [this] { setLink(linkEdit_->text()); }, Qt::QueuedConnection);
    if (!path.isEmpty())
        repo_ = cr::GitRepo::open(QDir(path).absolutePath().toStdString());
    if (!repo_) {
        repoEdit_->setText(path);
        repoInfo_->setText(path.isEmpty() ? tr("Choose a repository, or compare two directories")
                                          : tr("⚠ not inside a git work tree"));
        if (!path.isEmpty() && !dirsMode_->isChecked() && !prsMode_->isChecked())
            dirsMode_->setChecked(true);
        updateState();
        return;
    }
    const QString root = q(repo_->root());
    repoEdit_->setText(root);
    const auto branch = repo_->currentBranch();
    repoInfo_->setText(branch.empty() ? tr("detached HEAD") : tr("on branch %1").arg(q(branch)));
    // Last comparison in this repository, or uncommitted changes / the last commit.
    QSettings s;
    const QString base = s.value(settingsKey(root, "base")).toString();
    const QString target = s.value(settingsKey(root, "target")).toString();
    if (!base.isEmpty() && !target.isEmpty()) {
        baseEdit_->setText(base);
        targetEdit_->setText(target);
    } else if (auto head = cr::parseRevisionSpec(*repo_, "HEAD"); head && !repo_->diffWorkingTree(head->sha).empty()) {
        applyPreset(QStringLiteral("worktree"));
    } else {
        applyPreset(QStringLiteral("last"));
    }
    if (dirsMode_->isChecked())
        revisionsMode_->setChecked(true);
    updateState();
}

void CompareDialog::pickRevision(cr::Side side)
{
    if (!repo_)
        return;
    RevisionDialog dlg(*repo_, side == cr::Side::Old ? tr("Choose Base Revision") : tr("Choose Target Revision"), this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    auto rev = dlg.selectedRevision();
    if (!rev)
        return;
    auto* edit = side == cr::Side::Old ? baseEdit_ : targetEdit_;
    using K = cr::Revision::Kind;
    edit->setText(rev->kind == K::WorkingTree ? QStringLiteral("WORKTREE")
                  : rev->kind == K::Index     ? QStringLiteral("INDEX")
                                              : q(rev->ref.empty() ? rev->sha : rev->ref));
    picked_[side == cr::Side::Old ? 0 : 1] = *rev;
}

void CompareDialog::applyPreset(const QString& preset)
{
    if (!repo_)
        return;
    picked_[0].reset();
    picked_[1].reset();
    if (preset == QLatin1String("worktree")) {
        baseEdit_->setText(QStringLiteral("HEAD"));
        targetEdit_->setText(QStringLiteral("WORKTREE"));
    } else if (preset == QLatin1String("index")) {
        baseEdit_->setText(QStringLiteral("HEAD"));
        targetEdit_->setText(QStringLiteral("INDEX"));
    } else if (preset == QLatin1String("last")) {
        baseEdit_->setText(QStringLiteral("HEAD~1"));
        targetEdit_->setText(QStringLiteral("HEAD"));
    } else if (preset == QLatin1String("branch")) {
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
        const auto mb = repo_->mergeBase(def, "HEAD");
        if (mb.empty()) {
            error_->setText(tr("No merge base with %1.").arg(q(def)));
            return;
        }
        picked_[0] = cr::Revision::commit(mb, mb, "merge-base with " + def);
        baseEdit_->setText(q(mb));
        targetEdit_->setText(QStringLiteral("HEAD"));
    }
    error_->clear();
}

void CompareDialog::updateState()
{
    const bool git = repo_.has_value();
    revisionsMode_->setEnabled(git);
    prMode_->setEnabled(git);
    if (!git && (revisionsMode_->isChecked() || prMode_->isChecked()))
        dirsMode_->setChecked(true);
    pages_->setCurrentIndex(revisionsMode_->isChecked() ? 0 : prMode_->isChecked() ? 1 : prsMode_->isChecked() ? 3 : 2);
    error_->clear();
}

void CompareDialog::accept()
{
    const bool latest = choice_.latest;
    choice_ = CompareChoice{};
    if (tabs_->currentWidget() == sessionsPage_) {
        auto* it = sessionList_->currentItem();
        if (!it)
            return;
        choice_.kind = CompareChoice::Kind::Resume;
        choice_.session = sessions_[it->data(0, Qt::UserRole).toInt()];
        choice_.latest = latest;
        QDialog::accept();
        return;
    }
    choice_.repo = repo_ ? q(repo_->root()) : QString();
    if (prsMode_->isChecked()) {
        choice_.kind = CompareChoice::Kind::PullRequests;
        choice_.linkA = linkAEdit_->text().trimmed();
        choice_.linkB = linkBEdit_->text().trimmed();
        choice_.mapping = mappingEdit_->text().trimmed();
        for (const auto& link : {choice_.linkA, choice_.linkB})
            if (!cr::parsePullRequestUrl(link.toStdString())) {
                error_->setText(link.isEmpty() ? tr("Paste both pull request links.") : tr("Not a pull request link: %1").arg(link));
                return;
            }
        if (!choice_.mapping.isEmpty() && !choice_.mapping.contains(QLatin1Char('='))) {
            error_->setText(tr("The path mapping needs the form A-prefix=B-prefix."));
            return;
        }
    } else if (dirsMode_->isChecked()) {
        choice_.kind = CompareChoice::Kind::Directories;
        choice_.oldDir = oldDirEdit_->text().trimmed();
        choice_.newDir = newDirEdit_->text().trimmed();
        if (!QFileInfo(choice_.oldDir).isDir() || !QFileInfo(choice_.newDir).isDir()) {
            error_->setText(tr("Choose two existing directories."));
            return;
        }
    } else if (prMode_->isChecked()) {
        if (!pr_) {
            error_->setText(tr("Choose a pull request."));
            return;
        }
        choice_.kind = CompareChoice::Kind::PullRequest;
        choice_.pr = *pr_;
    } else {
        if (!repo_) {
            error_->setText(tr("Choose a git repository."));
            return;
        }
        auto resolve = [&](int i, QLineEdit* edit) -> std::optional<cr::Revision> {
            if (picked_[i])
                return picked_[i];
            return cr::parseRevisionSpec(*repo_, edit->text().trimmed().toStdString());
        };
        auto base = resolve(0, baseEdit_);
        auto target = resolve(1, targetEdit_);
        if (!base || !target) {
            error_->setText(tr("Unknown revision: %1").arg(!base ? baseEdit_->text() : targetEdit_->text()));
            return;
        }
        choice_.base = *base;
        choice_.target = *target;
        QSettings s;
        s.setValue(settingsKey(choice_.repo, "base"), baseEdit_->text().trimmed());
        s.setValue(settingsKey(choice_.repo, "target"), targetEdit_->text().trimmed());
    }
    if (repo_)
        QSettings().setValue(QStringLiteral("lastRepository"), choice_.repo);
    QDialog::accept();
}

} // namespace gui
