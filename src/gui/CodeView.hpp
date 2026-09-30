#pragma once

#include "core/Model.hpp"
#include "core/Review.hpp"

#include <QPlainTextEdit>
#include <QTextDocument>
#include <QTimer>
#include <functional>
#include <optional>

namespace gui {

class CppHighlighter;

// One visual row of a code view. Diff views contain filler rows (line == -1) to keep
// both sides aligned.
struct RowData {
    int line = -1; // 0-based line in the file, -1 for filler rows
    cr::LineTag tag = cr::LineTag::None;
    int change = -1; // semantic change index
    QVector<QPair<int, int>> spans; // intraline highlights (start, length) in QString characters
    int display = -1;  // line number to show instead of line + 1 (0: none), for generated text
    int hunk = -1;     // index of the diff hunk the row belongs to
    bool fold = false; // placeholder for a hunk hidden as reviewed (line == -1)
};

struct ChangeMeta {
    QString badge;
    QColor color;
    QString title;
};

enum class BookmarkAction { Toggle, EditComment, Remove };

// Read-only source view with line numbers, diff backgrounds, change badges and
// IDE-like navigation gestures (Ctrl+click, F12, hover).
class CodeView : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit CodeView(QWidget* parent = nullptr);

    void setContent(const QStringList& texts, QVector<RowData> rows, bool oldSide);
    void setSource(cr::Side side, const QString& path, bool external = false)
    {
        side_ = side;
        path_ = path;
        external_ = external;
    }
    // Re-applies colors after the theme changed.
    void refreshTheme();
    // Repaints bookmarks after they changed.
    void refreshMarks();
    void setChangeMetaProvider(std::function<ChangeMeta(int)> p) { changeMeta_ = std::move(p); }
    // Enables "mark hunk as reviewed" actions; tells whether a hunk is marked.
    void setHunkReviewedProvider(std::function<bool(int)> p) { hunkReviewed_ = std::move(p); }

    cr::Side side() const { return side_; }
    const QString& path() const { return path_; }
    bool isExternal() const { return external_; }
    const QVector<RowData>& rows() const { return rows_; }

    int rowForLine(int line) const;   // -1 if the line isn't shown
    int lineForRow(int row) const;    // -1 for fillers
    // Highlights every whole-word occurrence of `word` (empty clears); kept across setContent().
    void setHighlightWord(const QString& word);
    // Find-bar matches: highlights all of them (empty clears); findNext() selects the next one
    // after the cursor, wrapping around. Placeholder rows are skipped.
    void setSearch(const QString& text, QTextDocument::FindFlags flags);
    bool findNext(bool backward);
    int searchMatchCount() const { return searchMatches_; }

    // Bookmarks of all views: returns the comment ("" for a plain bookmark) of a bookmarked line
    // (0-based) of `path` on `side`, or nullopt.
    using BookmarkProvider = std::function<std::optional<QString>(cr::Side side, const QString& path, int line)>;
    static void setBookmarkProvider(BookmarkProvider p);
    void setOpenFileEnabled(bool on) { openFileEnabled_ = on; }
    void scrollToRow(int row, bool flash);
    void flashRow(int row);
    void scrollToLine(int line, bool flash) { scrollToRow(rowForLine(line), flash); }
    int topRow() const;
    int cursorRow() const { return textCursor().blockNumber(); }

    void lineNumberAreaPaint(QPaintEvent* e);
    int lineNumberAreaWidth() const;
    void gutterClicked(QPoint pos);
    bool gutterToolTip(QPoint pos, QString& text) const;

signals:
    void definitionRequested(gui::CodeView* view, int row, int column, bool declaration);
    void hoverRequested(gui::CodeView* view, int row, int column, QPoint globalPos);
    void changeActivated(int change);
    void foldActivated(int hunk);
    void hunkReviewRequested(int hunk, bool reviewed);
    void bookmarkRequested(gui::CodeView* view, int line, gui::BookmarkAction action);
    void openFileRequested(cr::Side side, const QString& path, int line);

protected:
    void resizeEvent(QResizeEvent* e) override;
    void paintEvent(QPaintEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;

private:
    void updateGutterWidth();
    void applyRowFormats();
    void updateGutter(const QRect& rect, int dy);
    void updateExtraSelections();
    void requestAt(const QTextCursor& c, bool declaration);
    bool wordCursorAt(QPoint viewportPos, QTextCursor& word) const;

    QWidget* gutter_ = nullptr;
    CppHighlighter* highlighter_ = nullptr;
    QVector<RowData> rows_;
    QVector<int> lineToRow_;
    bool oldSide_ = false;
    cr::Side side_ = cr::Side::New;
    QString path_;
    bool external_ = false;
    std::function<ChangeMeta(int)> changeMeta_;
    std::function<bool(int)> hunkReviewed_;
    std::optional<QString> bookmarkAt(int row) const;
    static BookmarkProvider bookmarks_;
    bool openFileEnabled_ = false;
    QString search_;
    QTextDocument::FindFlags searchFlags_;
    int searchMatches_ = 0;

    QTimer hoverTimer_;
    QPoint hoverPos_;
    QString highlightWord_;
    QTextCursor linkWord_;   // identifier underlined while Ctrl is held
    int flashRow_ = -1;
    QTimer flashTimer_;
};

} // namespace gui
