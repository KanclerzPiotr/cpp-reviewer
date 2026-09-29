#include "RevisionDialog.hpp"

#include "GitHubClient.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace gui {

namespace {

QString q(const std::string& s)
{
    return QString::fromStdString(s);
}

enum { RoleRef = Qt::UserRole + 1, RoleSha };

} // namespace

RevisionDialog::RevisionDialog(const cr::GitRepo& repo, const QString& title, QWidget* parent)
    : QDialog(parent), repo_(repo)
{
    setWindowTitle(title);
    resize(900, 560);
    auto* layout = new QVBoxLayout(this);
    tabs_ = new QTabWidget(this);
    layout->addWidget(tabs_);

    // --- Commits
    auto* commits = new QWidget;
    auto* cl = new QVBoxLayout(commits);
    auto* top = new QHBoxLayout;
    refCombo_ = new QComboBox;
    refCombo_->addItem(tr("All branches"), QString());
    refCombo_->addItem(QStringLiteral("HEAD"), QStringLiteral("HEAD"));
    for (const auto& b : repo_.localBranches())
        refCombo_->addItem(q(b), q(b));
    for (const auto& b : repo_.remoteBranches())
        refCombo_->addItem(q(b), q(b));
    refCombo_->setCurrentIndex(1);
    filter_ = new QLineEdit;
    filter_->setPlaceholderText(tr("Filter by message, author or hash…"));
    filter_->setClearButtonEnabled(true);
    top->addWidget(new QLabel(tr("History of:")));
    top->addWidget(refCombo_);
    top->addWidget(filter_, 1);
    cl->addLayout(top);
    log_ = new QTreeWidget;
    log_->setHeaderLabels({tr("Commit"), tr("Message"), tr("Refs"), tr("Author"), tr("Date")});
    log_->setRootIsDecorated(false);
    log_->setUniformRowHeights(true);
    log_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    log_->header()->setStretchLastSection(false);
    cl->addWidget(log_, 1);
    more_ = new QPushButton(tr("Load more"));
    cl->addWidget(more_, 0, Qt::AlignRight);
    tabs_->addTab(commits, tr("Commits"));

    // --- Branches & tags
    refs_ = new QTreeWidget;
    refs_->setHeaderLabels({tr("Reference")});
    auto addGroup = [this](const QString& name, const std::vector<std::string>& items) {
        auto* g = new QTreeWidgetItem(refs_, {name});
        g->setFlags(Qt::ItemIsEnabled);
        for (const auto& r : items) {
            auto* it = new QTreeWidgetItem(g, {q(r)});
            it->setData(0, RoleRef, q(r));
        }
        g->setExpanded(true);
    };
    addGroup(tr("Local branches"), repo_.localBranches());
    addGroup(tr("Remote branches"), repo_.remoteBranches());
    addGroup(tr("Tags"), repo_.tags());
    tabs_->addTab(refs_, tr("Branches && Tags"));

    // --- Working tree / index / expression
    auto* other = new QWidget;
    auto* ol = new QVBoxLayout(other);
    workTree_ = new QRadioButton(tr("Working tree (uncommitted changes, including untracked files)"));
    index_ = new QRadioButton(tr("Index (staged changes)"));
    custom_ = new QRadioButton(tr("Revision expression:"));
    customEdit_ = new QLineEdit;
    customEdit_->setPlaceholderText(QStringLiteral("HEAD~3, v1.2.0, origin/main@{yesterday}, 1a2b3c4…"));
    workTree_->setChecked(true);
    ol->addWidget(workTree_);
    ol->addWidget(index_);
    auto* ch = new QHBoxLayout;
    ch->addWidget(custom_);
    ch->addWidget(customEdit_, 1);
    ol->addLayout(ch);
    ol->addStretch();
    connect(customEdit_, &QLineEdit::textEdited, custom_, [this] { custom_->setChecked(true); });
    tabs_->addTab(other, tr("Working Tree && Other"));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(log_, &QTreeWidget::itemDoubleClicked, this, &QDialog::accept);
    connect(refs_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it) {
        if (!it->data(0, RoleRef).toString().isEmpty())
            accept();
    });
    connect(refCombo_, &QComboBox::currentIndexChanged, this, [this] { loadLog(false); });
    connect(more_, &QPushButton::clicked, this, [this] { loadLog(true); });
    connect(filter_, &QLineEdit::textChanged, this, &RevisionDialog::applyFilter);
    loadLog(false);
}

void RevisionDialog::loadLog(bool append)
{
    if (!append) {
        log_->clear();
        loaded_ = 0;
    }
    const auto ref = refCombo_->currentData().toString().toStdString();
    const auto commits = repo_.log(ref, 300, loaded_);
    for (const auto& c : commits) {
        auto* it = new QTreeWidgetItem(log_, {q(c.shortSha), q(c.subject), q(c.refs), q(c.author), q(c.date)});
        it->setData(0, RoleSha, q(c.sha));
        QFont mono = it->font(0);
        mono.setFamily(QStringLiteral("monospace"));
        it->setFont(0, mono);
    }
    loaded_ += static_cast<int>(commits.size());
    more_->setEnabled(commits.size() == 300);
    log_->resizeColumnToContents(0);
    log_->resizeColumnToContents(2);
    if (!append && log_->topLevelItemCount() > 0)
        log_->setCurrentItem(log_->topLevelItem(0));
    applyFilter();
}

void RevisionDialog::applyFilter()
{
    const QString f = filter_->text().trimmed();
    for (int i = 0; i < log_->topLevelItemCount(); ++i) {
        auto* it = log_->topLevelItem(i);
        bool match = f.isEmpty();
        for (int c = 0; c < 4 && !match; ++c)
            match = it->text(c).contains(f, Qt::CaseInsensitive) || it->data(0, RoleSha).toString().startsWith(f);
        it->setHidden(!match);
    }
}

std::optional<cr::Revision> RevisionDialog::selectedRevision() const
{
    switch (tabs_->currentIndex()) {
    case 0: {
        auto* it = log_->currentItem();
        if (!it)
            return std::nullopt;
        auto sha = it->data(0, RoleSha).toString().toStdString();
        auto label = it->text(0) + QStringLiteral(" ") + it->text(1).left(50);
        return cr::Revision::commit(sha, sha, label.toStdString());
    }
    case 1: {
        auto* it = refs_->currentItem();
        if (!it || it->data(0, RoleRef).toString().isEmpty())
            return std::nullopt;
        return cr::parseRevisionSpec(repo_, it->data(0, RoleRef).toString().toStdString());
    }
    default:
        if (workTree_->isChecked())
            return cr::Revision::workingTree();
        if (index_->isChecked())
            return cr::Revision::index();
        return cr::parseRevisionSpec(repo_, customEdit_->text().trimmed().toStdString());
    }
}

// ---------------------------------------------------------------------------------------------

PullRequestDialog::PullRequestDialog(const cr::GitRepo& repo, QWidget* parent)
    : QDialog(parent), repo_(repo), client_(new GitHubClient(this))
{
    setWindowTitle(tr("Review Pull Request"));
    resize(820, 480);
    auto* layout = new QVBoxLayout(this);

    auto* top = new QHBoxLayout;
    remote_ = new QComboBox;
    for (const auto& r : repo_.remotes())
        remote_->addItem(q(r));
    int origin = remote_->findText(QStringLiteral("origin"));
    if (origin >= 0)
        remote_->setCurrentIndex(origin);
    auto* refreshButton = new QPushButton(tr("Refresh"));
    top->addWidget(new QLabel(tr("Remote:")));
    top->addWidget(remote_);
    top->addWidget(refreshButton);
    top->addStretch();
    layout->addLayout(top);

    list_ = new QTreeWidget;
    list_->setHeaderLabels({tr("#"), tr("Title"), tr("Author"), tr("Base ← Head"), tr("Updated")});
    list_->setRootIsDecorated(false);
    list_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    list_->header()->setStretchLastSection(false);
    layout->addWidget(list_, 1);

    status_ = new QLabel;
    status_->setWordWrap(true);
    layout->addWidget(status_);

    auto* form = new QFormLayout;
    number_ = new QSpinBox;
    number_->setRange(1, 10000000);
    base_ = new QLineEdit;
    base_->setPlaceholderText(tr("base branch, e.g. main"));
    form->addRow(tr("Pull request number:"), number_);
    form->addRow(tr("Base branch:"), base_);
    layout->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Review"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(refreshButton, &QPushButton::clicked, this, &PullRequestDialog::refresh);
    connect(remote_, &QComboBox::currentIndexChanged, this, &PullRequestDialog::refresh);
    connect(list_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* it) {
        if (!it)
            return;
        number_->setValue(it->text(0).toInt());
        base_->setText(it->data(0, Qt::UserRole).toString());
    });
    connect(list_, &QTreeWidget::itemDoubleClicked, this, &QDialog::accept);
    refresh();
}

void PullRequestDialog::refresh()
{
    list_->clear();
    const auto remote = remote_->currentText().toStdString();
    const auto def = repo_.remoteDefaultBranch(remote);
    if (base_->text().isEmpty())
        base_->setText(def.empty() ? QStringLiteral("main") : q(def));
    const auto slug = q(cr::gitHubSlugFromUrl(repo_.remoteUrl(remote)));
    if (slug.isEmpty()) {
        status_->setText(tr("Remote '%1' is not hosted on GitHub; enter the pull request number manually "
                            "(fetched from refs/pull/N/head).")
                             .arg(remote_->currentText()));
        return;
    }
    status_->setText(tr("Loading pull requests of %1…").arg(slug));
    client_->listPullRequests(slug, [this, slug](QVector<PullRequestInfo> prs, QString error) {
        if (!error.isEmpty()) {
            status_->setText(tr("Could not list pull requests of %1: %2. Set GITHUB_TOKEN for private repositories, "
                                "or enter the number manually.")
                                 .arg(slug, error));
            return;
        }
        for (const auto& pr : prs) {
            auto* it = new QTreeWidgetItem(list_, {QString::number(pr.number),
                                                   (pr.draft ? tr("[draft] ") : QString()) + pr.title, pr.author,
                                                   pr.baseRef + QStringLiteral(" ← ") + pr.headRef, pr.updated});
            it->setData(0, Qt::UserRole, pr.baseRef);
            it->setData(1, Qt::UserRole, pr.title);
        }
        list_->resizeColumnToContents(0);
        list_->resizeColumnToContents(3);
        status_->setText(tr("%n open pull request(s) in %1.", nullptr, static_cast<int>(prs.size())).arg(slug));
    });
}

PullRequestChoice PullRequestDialog::choice() const
{
    PullRequestChoice c;
    c.remote = remote_->currentText();
    c.number = number_->value();
    c.baseRef = base_->text().trimmed();
    if (auto* it = list_->currentItem(); it && it->text(0).toInt() == c.number)
        c.title = it->data(1, Qt::UserRole).toString();
    return c;
}

} // namespace gui
