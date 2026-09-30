#pragma once

#include "CodeView.hpp"
#include "core/Review.hpp"
#include "core/Reviewed.hpp"

#include <QLabel>
#include <QSet>
#include <QWidget>

#include <optional>

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

    // Enables folding: hunks for which `reviewed` returns true are collapsed into one row
    // (until expanded by the user).
    void setReviewedProvider(std::function<bool(const cr::Hunk&)> reviewed);
    // Re-applies folding after review marks changed, keeping the scroll position.
    void refreshFolding();
    void setFoldingEnabled(bool on)
    {
        foldReviewed_ = on;
        refreshFolding();
    }
    // Marks the hunk at the cursor (or the next unreviewed one) as reviewed and moves on to the
    // next hunk. Returns false if there's nothing left to mark below the cursor.
    bool markHunkAtCursorReviewed();
    void scrollToFirstChange();
    const std::vector<cr::Hunk>& hunks() const { return hunks_; }

signals:
    void definitionRequested(gui::CodeView* view, int row, int column, bool declaration);
    void hoverRequested(gui::CodeView* view, int row, int column, QPoint globalPos);
    void changeActivated(int change);
    void hunkReviewToggled(quint64 key, bool reviewed);
    void bookmarkRequested(gui::CodeView* view, int line, gui::BookmarkAction action);
    void openFileRequested(cr::Side side, const QString& path, int line);

private:
    void render();
    bool hidden(int hunk) const;
    void setHunkReviewed(int hunk, bool reviewed);
    int rowOfHunk(int hunk) const;
    // Expands a folded hunk containing `line` (0-based, without offset) of `side`.
    bool reveal(cr::Side side, int line);

    CodeView* left_;
    CodeView* right_;
    QLabel* title_;
    QLabel* leftCaption_;
    QLabel* rightCaption_;
    OverviewBar* overview_;
    QString oldPath_, newPath_;
    bool syncing_ = false;

    std::optional<cr::FileDiff> fd_;
    int oldOffset_ = 0, newOffset_ = 0;
    std::vector<cr::Hunk> hunks_;
    std::function<bool(const cr::Hunk&)> reviewed_;
    QSet<quint64> revealed_; // reviewed hunks the user expanded again
    bool foldReviewed_ = true;
};

} // namespace gui
