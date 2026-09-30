#include "FindBar.hpp"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>

namespace gui {

FindBar::FindBar(QWidget* parent) : QWidget(parent)
{
    auto* l = new QHBoxLayout(this);
    l->setContentsMargins(6, 2, 6, 2);
    l->setSpacing(4);
    l->addWidget(new QLabel(tr("Find:")));
    edit_ = new QLineEdit;
    edit_->setClearButtonEnabled(true);
    edit_->setPlaceholderText(tr("Search in this view (Enter: next, Shift+Enter: previous)"));
    l->addWidget(edit_, 1);
    auto button = [&](const QString& text, const QString& tip, bool checkable) {
        auto* b = new QToolButton;
        b->setText(text);
        b->setToolTip(tip);
        b->setCheckable(checkable);
        b->setAutoRaise(true);
        l->addWidget(b);
        return b;
    };
    caseButton_ = button(QStringLiteral("Aa"), tr("Match case"), true);
    wordButton_ = button(QStringLiteral("W"), tr("Whole words"), true);
    auto* prev = button(QStringLiteral("▲"), tr("Previous match (Shift+F3)"), false);
    auto* next = button(QStringLiteral("▼"), tr("Next match (F3)"), false);
    info_ = new QLabel;
    info_->setMinimumWidth(90);
    l->addWidget(info_);
    auto* close = button(QStringLiteral("✕"), tr("Close (Esc)"), false);

    connect(edit_, &QLineEdit::textChanged, this, &FindBar::searchChanged);
    connect(caseButton_, &QToolButton::toggled, this, &FindBar::searchChanged);
    connect(wordButton_, &QToolButton::toggled, this, &FindBar::searchChanged);
    connect(prev, &QToolButton::clicked, this, [this] { emit findRequested(true); });
    connect(next, &QToolButton::clicked, this, [this] { emit findRequested(false); });
    connect(close, &QToolButton::clicked, this, [this] {
        hide();
        emit closed();
    });
    hide();
}

QString FindBar::text() const
{
    return isVisible() ? edit_->text() : QString();
}

QTextDocument::FindFlags FindBar::flags() const
{
    QTextDocument::FindFlags f;
    if (caseButton_->isChecked())
        f |= QTextDocument::FindCaseSensitively;
    if (wordButton_->isChecked())
        f |= QTextDocument::FindWholeWords;
    return f;
}

void FindBar::activate(const QString& initial)
{
    show();
    if (!initial.isEmpty() && !initial.contains(QLatin1Char('\n')))
        edit_->setText(initial);
    edit_->setFocus();
    edit_->selectAll();
    emit searchChanged();
}

void FindBar::setMatchInfo(int matches, bool found)
{
    if (edit_->text().isEmpty())
        info_->clear();
    else if (matches == 0 || !found)
        info_->setText(tr("<span style='color:#cf222e'>no matches</span>"));
    else
        info_->setText(tr("%n match(es)", nullptr, matches));
}

void FindBar::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        emit findRequested(e->modifiers() & Qt::ShiftModifier);
        return;
    }
    if (e->key() == Qt::Key_Escape) {
        hide();
        emit closed();
        return;
    }
    QWidget::keyPressEvent(e);
}

} // namespace gui
