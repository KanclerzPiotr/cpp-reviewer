#pragma once

#include "CodeView.hpp"
#include "core/Review.hpp"

#include <QLabel>
#include <QWidget>

namespace gui {

class OverviewBar;

// Side-by-side diff of one file (or of two fragments, e.g. a moved function and its origin).
class DiffView : public QWidget {
    Q_OBJECT
public:
    explicit DiffView(QWidget* parent = nullptr);

    // `oldOffset`/`newOffset` are added to line numbers (used for fragments).
    void showDiff(const cr::FileDiff& fd, const QString& title, int oldOffset = 0, int newOffset = 0);
    void setSideCaptions(const QString& oldCaption, const QString& newCaption);
    void setChangeMetaProvider(std::function<ChangeMeta(int)> p);

    CodeView* view(cr::Side s) const { return s == cr::Side::Old ? left_ : right_; }
    void scrollToLine(cr::Side side, int line, bool flash);
    void gotoHunk(bool forward);
    void setWordHighlights(const QString& oldWord, const QString& newWord)
    {
        left_->setHighlightWord(oldWord);
        right_->setHighlightWord(newWord);
    }
    const QString& oldPath() const { return oldPath_; }
    const QString& newPath() const { return newPath_; }
    int rowCount() const;

signals:
    void definitionRequested(gui::CodeView* view, int row, int column, bool declaration);
    void hoverRequested(gui::CodeView* view, int row, int column, QPoint globalPos);
    void changeActivated(int change);

private:
    CodeView* left_;
    CodeView* right_;
    QLabel* title_;
    QLabel* leftCaption_;
    QLabel* rightCaption_;
    OverviewBar* overview_;
    QString oldPath_, newPath_;
    bool syncing_ = false;
};

} // namespace gui
