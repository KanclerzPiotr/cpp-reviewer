#include "QuickOpen.hpp"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QRadioButton>
#include <QRegularExpression>
#include <QVBoxLayout>

#include <algorithm>

namespace gui {

QuickOpenDialog::QuickOpenDialog(FilesFn files, cr::Side side, QWidget* parent)
    : QDialog(parent), filesFn_(std::move(files))
{
    setWindowTitle(tr("Open File"));
    resize(760, 480);
    auto* l = new QVBoxLayout(this);
    edit_ = new QLineEdit;
    edit_->setPlaceholderText(tr("Type parts of a path, e.g. \"lowering hpp\" or \"Foo.cpp:120\""));
    edit_->installEventFilter(this);
    l->addWidget(edit_);
    auto* sides = new QHBoxLayout;
    target_ = new QRadioButton(tr("Target"));
    base_ = new QRadioButton(tr("Base"));
    (side == cr::Side::Old ? base_ : target_)->setChecked(true);
    sides->addWidget(new QLabel(tr("Revision:")));
    sides->addWidget(target_);
    sides->addWidget(base_);
    sides->addStretch(1);
    info_ = new QLabel;
    sides->addWidget(info_);
    l->addLayout(sides);
    list_ = new QListWidget;
    list_->setUniformItemSizes(true);
    l->addWidget(list_, 1);

    connect(edit_, &QLineEdit::textChanged, this, &QuickOpenDialog::refilter);
    connect(target_, &QRadioButton::toggled, this, &QuickOpenDialog::load);
    connect(list_, &QListWidget::itemActivated, this, &QuickOpenDialog::acceptCurrent);
    load();
}

cr::Side QuickOpenDialog::side() const
{
    return base_->isChecked() ? cr::Side::Old : cr::Side::New;
}

void QuickOpenDialog::load()
{
    files_.reset();
    list_->clear();
    info_->setText(tr("Listing files…"));
    const int gen = ++generation_;
    QPointer<QuickOpenDialog> guard(this);
    filesFn_(side(), [guard, gen](Files f) {
        if (!guard || gen != guard->generation_)
            return;
        guard->files_ = std::move(f);
        guard->refilter();
    });
}

void QuickOpenDialog::refilter()
{
    list_->clear();
    if (!files_)
        return;
    QString query = edit_->text().trimmed();
    line_ = 0;
    static const QRegularExpression lineSuffix(QStringLiteral(":(\\d+)$"));
    if (auto m = lineSuffix.match(query); m.hasMatch()) {
        line_ = m.captured(1).toInt();
        query.chop(m.capturedLength());
    }
    const auto terms = query.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    struct Hit {
        int score;
        const std::string* path;
    };
    std::vector<Hit> hits;
    for (const auto& p : *files_) {
        const QString lower = QString::fromStdString(p).toLower();
        const int slash = static_cast<int>(lower.lastIndexOf(QLatin1Char('/')));
        const QStringView name = QStringView(lower).mid(slash + 1);
        int score = 0;
        bool ok = true;
        for (const auto& t : terms) {
            if (!lower.contains(t)) {
                ok = false;
                break;
            }
            score += name.contains(t) ? 10 : 1;
            if (name.startsWith(t))
                score += 5;
        }
        if (ok)
            hits.push_back({score * 1000 - static_cast<int>(p.size()), &p});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
    const size_t shown = std::min<size_t>(hits.size(), 500);
    for (size_t i = 0; i < shown; ++i)
        list_->addItem(QString::fromStdString(*hits[i].path));
    if (list_->count() > 0)
        list_->setCurrentRow(0);
    info_->setText(hits.size() > shown ? tr("%1 of %2 files").arg(shown).arg(hits.size())
                                       : tr("%n file(s)", nullptr, static_cast<int>(hits.size())));
}

void QuickOpenDialog::acceptCurrent()
{
    if (auto* it = list_->currentItem()) {
        path_ = it->text();
        accept();
    }
}

bool QuickOpenDialog::eventFilter(QObject* o, QEvent* e)
{
    // Up/Down/PageUp/PageDown in the text field move through the list.
    if (o == edit_ && e->type() == QEvent::KeyPress) {
        auto* k = static_cast<QKeyEvent*>(e);
        switch (k->key()) {
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
            QCoreApplication::sendEvent(list_, e);
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            acceptCurrent();
            return true;
        default:
            break;
        }
    }
    return QDialog::eventFilter(o, e);
}

} // namespace gui
