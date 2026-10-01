#include "CodeView.hpp"

#include "CppHighlighter.hpp"
#include "Theme.hpp"

#include <QFontDatabase>
#include <QMenu>
#include <QPainter>
#include <QScrollBar>
#include <QTextBlock>
#include <QToolTip>

namespace gui {

namespace {

class Gutter : public QWidget {
public:
    explicit Gutter(CodeView* view) : QWidget(view), view_(view) { setMouseTracking(true); }
    QSize sizeHint() const override { return {view_->lineNumberAreaWidth(), 0}; }

protected:
    void paintEvent(QPaintEvent* e) override { view_->lineNumberAreaPaint(e); }
    void mousePressEvent(QMouseEvent* e) override { view_->gutterClicked(e->pos()); }
    bool event(QEvent* e) override
    {
        if (e->type() == QEvent::ToolTip) {
            auto* he = static_cast<QHelpEvent*>(e);
            QString text;
            if (view_->gutterToolTip(he->pos(), text))
                QToolTip::showText(he->globalPos(), text, this);
            else
                QToolTip::hideText();
            return true;
        }
        return QWidget::event(e);
    }

private:
    CodeView* view_;
};

constexpr int kBadgeWidth = 18;

} // namespace

CodeView::BookmarkProvider CodeView::bookmarks_;

void CodeView::setBookmarkProvider(BookmarkProvider p)
{
    bookmarks_ = std::move(p);
}

std::optional<QString> CodeView::bookmarkAt(int row) const
{
    if (!bookmarks_ || path_.isEmpty() || row < 0 || row >= rows_.size() || rows_[row].line < 0)
        return std::nullopt;
    return bookmarks_(side_, path_, rows_[row].line);
}

void CodeView::setSearch(const QString& text, QTextDocument::FindFlags flags)
{
    search_ = text;
    searchFlags_ = flags;
    updateExtraSelections();
}

bool CodeView::findNext(bool backward)
{
    if (search_.isEmpty())
        return false;
    auto flags = searchFlags_;
    if (backward)
        flags |= QTextDocument::FindBackward;
    auto isCode = [this](const QTextCursor& c) {
        const int row = c.blockNumber();
        return row >= 0 && row < rows_.size() && rows_[row].line >= 0;
    };
    QTextCursor from = textCursor();
    if (backward) // don't find the current match again
        from.setPosition(from.selectionStart());
    for (int pass = 0; pass < 2; ++pass) {
        for (QTextCursor c = document()->find(search_, from, flags); !c.isNull(); c = document()->find(search_, c, flags)) {
            if (!isCode(c))
                continue;
            setTextCursor(c);
            centerCursor();
            return true;
        }
        // Wrap around.
        from = QTextCursor(document());
        if (backward)
            from.movePosition(QTextCursor::End);
    }
    return false;
}

CodeView::CodeView(QWidget* parent) : QPlainTextEdit(parent)
{
    setReadOnly(true);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);

    gutter_ = new Gutter(this);
    highlighter_ = new CppHighlighter(document());
    highlighter_->setExtraProvider([this](int block) {
        QVector<CppHighlighter::Extra> out;
        if (block < 0 || block >= rows_.size())
            return out;
        const auto color = Theme::current().intraline(oldSide_);
        for (auto [start, len] : rows_[block].spans)
            out.push_back({start, len, color});
        return out;
    });

    highlighter_->setPlainProvider(
        [this](int block) { return block >= 0 && block < rows_.size() && (rows_[block].fold || rows_[block].context); });

    connect(this, &QPlainTextEdit::blockCountChanged, this, &CodeView::updateGutterWidth);
    connect(this, &QPlainTextEdit::updateRequest, this, &CodeView::updateGutter);

    hoverTimer_.setSingleShot(true);
    hoverTimer_.setInterval(450);
    connect(&hoverTimer_, &QTimer::timeout, this, [this] {
        QTextCursor word;
        if (!wordCursorAt(hoverPos_, word))
            return;
        emit hoverRequested(this, word.blockNumber(), word.selectionStart() - word.block().position(),
                            viewport()->mapToGlobal(hoverPos_));
    });
    flashTimer_.setSingleShot(true);
    connect(&flashTimer_, &QTimer::timeout, this, [this] {
        flashRow_ = -1;
        updateExtraSelections();
    });
    updateGutterWidth();
}

void CodeView::setContent(const QStringList& texts, QVector<RowData> rows, bool oldSide)
{
    rows_ = std::move(rows);
    oldSide_ = oldSide;
    lineToRow_.clear();
    for (int r = 0; r < rows_.size(); ++r) {
        int l = rows_[r].line;
        if (l < 0)
            continue;
        if (l >= lineToRow_.size())
            lineToRow_.resize(l + 1, -1);
        lineToRow_[l] = r;
    }

    setPlainText(texts.join(QLatin1Char('\n')));
    applyRowFormats();
    flashRow_ = -1;
    updateExtraSelections();
    updateGutterWidth();
}

void CodeView::refreshTheme()
{
    applyRowFormats();
    highlighter_->applyTheme();
    updateExtraSelections();
    gutter_->update();
    viewport()->update();
}

void CodeView::refreshMarks()
{
    gutter_->update();
    viewport()->update();
}

// Row backgrounds live in the block format so they survive re-highlighting.
void CodeView::applyRowFormats()
{
    const auto& theme = Theme::current();
    QTextCursor c(document());
    c.beginEditBlock();
    QTextBlock b = document()->begin();
    for (int r = 0; b.isValid() && r < rows_.size(); ++r, b = b.next()) {
        const auto& row = rows_[r];
        QColor bg;
        Qt::BrushStyle style = Qt::SolidPattern;
        if (row.fold) {
            bg = theme.filler();
        } else if (row.line < 0) {
            bg = theme.filler();
            style = Qt::BDiagPattern;
        } else {
            bg = theme.lineBackground(row.tag);
        }
        QTextBlockFormat fmt;
        if (bg.isValid())
            fmt.setBackground(QBrush(bg, style));
        else if (!b.blockFormat().background().isOpaque())
            continue;
        c.setPosition(b.position());
        c.setBlockFormat(fmt);
    }
    c.endEditBlock();
}

int CodeView::rowForLine(int line) const
{
    if (line < 0)
        return -1;
    if (line >= lineToRow_.size())
        return rows_.isEmpty() ? -1 : static_cast<int>(rows_.size()) - 1;
    // Lines not present (e.g. outside a fragment) map to the nearest following row.
    for (int l = line; l < lineToRow_.size(); ++l)
        if (lineToRow_[l] >= 0)
            return lineToRow_[l];
    return -1;
}

int CodeView::lineForRow(int row) const
{
    return row >= 0 && row < rows_.size() ? rows_[row].line : -1;
}

void CodeView::scrollToRow(int row, bool flash)
{
    if (row < 0)
        return;
    QTextBlock b = document()->findBlockByNumber(row);
    if (!b.isValid())
        return;
    QTextCursor c(b);
    setTextCursor(c);
    // Center the row.
    const int visibleRows = viewport()->height() / std::max(1, fontMetrics().height());
    verticalScrollBar()->setValue(std::max(0, row - visibleRows / 3));
    if (flash)
        flashRow(row);
}

void CodeView::setHighlightWord(const QString& word)
{
    if (word == highlightWord_)
        return;
    highlightWord_ = word;
    updateExtraSelections();
}

void CodeView::flashRow(int row)
{
    if (row < 0)
        return;
    flashRow_ = row;
    updateExtraSelections();
    flashTimer_.start(1500);
}

int CodeView::topRow() const
{
    return firstVisibleBlock().blockNumber();
}

int CodeView::lineNumberAreaWidth() const
{
    int maxLine = 1;
    for (const auto& r : rows_)
        maxLine = std::max(maxLine, r.line + 1);
    int digits = QString::number(maxLine).size();
    return 8 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * std::max(3, digits) + kBadgeWidth;
}

void CodeView::updateGutterWidth()
{
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void CodeView::updateGutter(const QRect& rect, int dy)
{
    if (dy)
        gutter_->scroll(0, dy);
    else
        gutter_->update(0, rect.y(), gutter_->width(), rect.height());
}

void CodeView::resizeEvent(QResizeEvent* e)
{
    QPlainTextEdit::resizeEvent(e);
    QRect cr = contentsRect();
    gutter_->setGeometry(QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
}

void CodeView::lineNumberAreaPaint(QPaintEvent* e)
{
    QPainter p(gutter_);
    const auto& theme = Theme::current();
    p.fillRect(e->rect(), palette().color(QPalette::AlternateBase));
    QTextBlock block = firstVisibleBlock();
    int row = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    const int w = gutter_->width();
    const int h = fontMetrics().height();
    QFont badgeFont = font();
    badgeFont.setBold(true);

    while (block.isValid() && top <= e->rect().bottom()) {
        if (block.isVisible() && bottom >= e->rect().top() && row < rows_.size()) {
            const auto& r = rows_[row];
            if (r.line >= 0) {
                QColor bg = theme.lineBackground(r.tag);
                if (bg.isValid())
                    p.fillRect(QRect(0, top, w - kBadgeWidth, bottom - top), bg.darker(r.tag == cr::LineTag::None ? 100 : 108));
                p.setPen(palette().color(QPalette::PlaceholderText));
                p.setFont(font());
                if (const int shown = r.display >= 0 ? r.display : r.line + 1; shown > 0)
                    p.drawText(0, top, w - kBadgeWidth - 4, h, Qt::AlignRight | Qt::AlignVCenter, QString::number(shown));
            }
            if (auto mark = bookmarkAt(row)) {
                const QColor c = palette().color(QPalette::Link);
                p.setPen(Qt::NoPen);
                p.setBrush(c);
                const int d = std::min(7, bottom - top - 4);
                p.drawEllipse(QRect(1, top + (bottom - top - d) / 2, d, d));
                if (!mark->isEmpty()) // has a comment: also a bar at the edge
                    p.fillRect(QRect(0, top, 1, bottom - top), c);
                p.setBrush(Qt::NoBrush);
            }
            if (r.fold) {
                p.setPen(theme.changeColor(cr::ChangeKind::Added));
                p.setFont(badgeFont);
                p.drawText(0, top, w - 4, h, Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("✓"));
            }
            // Badge on the first row of a run belonging to one semantic change.
            if (r.change >= 0 && changeMeta_) {
                const auto meta = changeMeta_(r.change);
                p.fillRect(QRect(w - kBadgeWidth + 2, top, 4, bottom - top), meta.color);
                if (row == 0 || rows_[row - 1].change != r.change) {
                    p.setPen(meta.color);
                    p.setFont(badgeFont);
                    p.drawText(w - kBadgeWidth + 6, top, kBadgeWidth - 6, h, Qt::AlignLeft | Qt::AlignVCenter, meta.badge);
                }
            }
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++row;
    }
}

static int rowAtGutterPos(const CodeView* v, QPoint pos)
{
    QTextCursor c = v->cursorForPosition(QPoint(0, pos.y()));
    return c.blockNumber();
}

void CodeView::gutterClicked(QPoint pos)
{
    int row = rowAtGutterPos(this, pos);
    if (row >= 0 && row < rows_.size() && rows_[row].fold)
        emit foldActivated(rows_[row].hunk);
    else if (row >= 0 && row < rows_.size() && rows_[row].change >= 0)
        emit changeActivated(rows_[row].change);
}

bool CodeView::gutterToolTip(QPoint pos, QString& text) const
{
    int row = rowAtGutterPos(this, pos);
    if (row >= 0 && row < rows_.size() && rows_[row].fold) {
        text = tr("Reviewed hunk (click to show)");
        return true;
    }
    if (auto mark = bookmarkAt(row)) {
        text = mark->isEmpty() ? tr("Bookmark") : tr("Comment:\n%1").arg(*mark);
        if (rows_[row].change >= 0 && changeMeta_)
            text += QStringLiteral("\n\n") + changeMeta_(rows_[row].change).title;
        return true;
    }
    if (row < 0 || row >= rows_.size() || rows_[row].change < 0 || !changeMeta_)
        return false;
    text = changeMeta_(rows_[row].change).title + QStringLiteral("\n(click to open)");
    return true;
}

bool CodeView::wordCursorAt(QPoint viewportPos, QTextCursor& word) const
{
    QTextCursor c = cursorForPosition(viewportPos);
    QRect r = cursorRect(c);
    if (std::abs(r.center().y() - viewportPos.y()) > r.height())
        return false;
    // Must actually be over a character, not past the end of the line.
    QTextBlock b = c.block();
    const QString text = b.text();
    int pos = c.positionInBlock();
    auto isWord = [](QChar ch) { return ch.isLetterOrNumber() || ch == QLatin1Char('_'); };
    if (pos < text.size() && isWord(text[pos])) {
    } else if (pos > 0 && pos <= text.size() && isWord(text[pos - 1])) {
        --pos;
    } else if (text.trimmed().startsWith(QLatin1Char('#'))) {
        // #include lines: allow hovering anywhere in the file name.
    } else {
        return false;
    }
    int start = pos, end = pos;
    while (start > 0 && isWord(text[start - 1]))
        --start;
    while (end < text.size() && isWord(text[end]))
        ++end;
    word = QTextCursor(b);
    word.setPosition(b.position() + start);
    word.setPosition(b.position() + std::max(end, start + 1), QTextCursor::KeepAnchor);
    return true;
}

void CodeView::updateExtraSelections()
{
    QList<QTextEdit::ExtraSelection> sels;
    if (flashRow_ >= 0) {
        QTextEdit::ExtraSelection s;
        s.format.setBackground(Theme::current().flash());
        s.format.setProperty(QTextFormat::FullWidthSelection, true);
        s.cursor = QTextCursor(document()->findBlockByNumber(flashRow_));
        sels.push_back(s);
    }
    if (!highlightWord_.isEmpty()) {
        QTextCharFormat fmt;
        QColor bg = Theme::current().changeColor(cr::ChangeKind::SymbolRenamed);
        bg.setAlpha(Theme::current().dark ? 150 : 90);
        fmt.setBackground(bg);
        fmt.setFontWeight(QFont::Bold);
        const auto flags = QTextDocument::FindWholeWords | QTextDocument::FindCaseSensitively;
        QTextCursor c(document());
        for (int n = 0; n < 5000; ++n) {
            c = document()->find(highlightWord_, c, flags);
            if (c.isNull())
                break;
            QTextEdit::ExtraSelection s;
            s.format = fmt;
            s.cursor = c;
            sels.push_back(s);
        }
    }
    searchMatches_ = 0;
    if (!search_.isEmpty()) {
        QTextCharFormat fmt;
        QColor bg = Theme::current().flash();
        bg.setAlpha(Theme::current().dark ? 170 : 200);
        fmt.setBackground(bg);
        QTextCursor c(document());
        for (int n = 0; n < 5000; ++n) {
            c = document()->find(search_, c, searchFlags_);
            if (c.isNull())
                break;
            const int row = c.blockNumber();
            if (row < 0 || row >= rows_.size() || rows_[row].line < 0)
                continue;
            ++searchMatches_;
            QTextEdit::ExtraSelection s;
            s.format = fmt;
            s.cursor = c;
            sels.push_back(s);
        }
    }
    if (!linkWord_.isNull() && linkWord_.hasSelection()) {
        QTextEdit::ExtraSelection s;
        s.format.setFontUnderline(true);
        s.format.setForeground(palette().color(QPalette::Link));
        s.cursor = linkWord_;
        sels.push_back(s);
    }
    setExtraSelections(sels);
}

void CodeView::mouseMoveEvent(QMouseEvent* e)
{
    QPlainTextEdit::mouseMoveEvent(e);
    hoverPos_ = e->pos();
    if (e->buttons() == Qt::NoButton)
        hoverTimer_.start();
    else
        hoverTimer_.stop();

    QTextCursor word;
    if ((e->modifiers() & Qt::ControlModifier) && wordCursorAt(e->pos(), word)) {
        if (linkWord_.isNull() || linkWord_.selectionStart() != word.selectionStart()) {
            linkWord_ = word;
            viewport()->setCursor(Qt::PointingHandCursor);
            updateExtraSelections();
        }
    } else if (!linkWord_.isNull()) {
        linkWord_ = QTextCursor();
        viewport()->setCursor(Qt::IBeamCursor);
        updateExtraSelections();
    }
}

void CodeView::mousePressEvent(QMouseEvent* e)
{
    hoverTimer_.stop();
    QToolTip::hideText();
    if (e->button() == Qt::LeftButton && (e->modifiers() & Qt::ControlModifier)) {
        QTextCursor word;
        if (wordCursorAt(e->pos(), word)) {
            requestAt(word, e->modifiers() & Qt::ShiftModifier);
            e->accept();
            return;
        }
    }
    if (e->button() == Qt::BackButton || e->button() == Qt::ForwardButton) {
        e->ignore(); // let the main window handle history navigation
        return;
    }
    QPlainTextEdit::mousePressEvent(e);
}

void CodeView::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() == Qt::BackButton || e->button() == Qt::ForwardButton) {
        e->ignore();
        return;
    }
    QPlainTextEdit::mouseReleaseEvent(e);
}

// Comments of bookmarked lines are drawn after the end of the line.
void CodeView::paintEvent(QPaintEvent* e)
{
    QPlainTextEdit::paintEvent(e);
    if (!bookmarks_ || path_.isEmpty())
        return;
    QPainter p(viewport());
    QFont f = font();
    f.setItalic(true);
    p.setFont(f);
    QColor fg = palette().color(QPalette::Link);
    QColor bg = fg;
    bg.setAlpha(40);
    const QFontMetrics fm(f);
    for (QTextBlock b = firstVisibleBlock(); b.isValid(); b = b.next()) {
        const QRectF geo = blockBoundingGeometry(b).translated(contentOffset());
        if (geo.top() > e->rect().bottom())
            break;
        auto mark = bookmarkAt(b.blockNumber());
        if (!mark || mark->isEmpty())
            continue;
        QTextCursor end(b);
        end.movePosition(QTextCursor::EndOfBlock);
        const int x = cursorRect(end).right() + fm.horizontalAdvance(QStringLiteral("    "));
        const QString text = QStringLiteral("💬 ") + mark->section(QLatin1Char('\n'), 0, 0) +
                             (mark->contains(QLatin1Char('\n')) ? QStringLiteral(" …") : QString());
        QRect r(x, static_cast<int>(geo.top()), fm.horizontalAdvance(text) + 12, static_cast<int>(geo.height()));
        p.fillRect(r, bg);
        p.setPen(fg);
        p.drawText(r.adjusted(6, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, text);
    }
}

void CodeView::mouseDoubleClickEvent(QMouseEvent* e)
{
    const int row = cursorForPosition(e->pos()).blockNumber();
    if (e->button() == Qt::LeftButton && row >= 0 && row < rows_.size() && rows_[row].fold) {
        emit foldActivated(rows_[row].hunk);
        e->accept();
        return;
    }
    QPlainTextEdit::mouseDoubleClickEvent(e);
}

void CodeView::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_F12) {
        QTextCursor c = textCursor();
        c.select(QTextCursor::WordUnderCursor);
        requestAt(c, e->modifiers() & Qt::ControlModifier);
        return;
    }
    if (e->key() == Qt::Key_Control) {
        QPoint pos = viewport()->mapFromGlobal(QCursor::pos());
        QTextCursor word;
        if (viewport()->rect().contains(pos) && wordCursorAt(pos, word)) {
            linkWord_ = word;
            viewport()->setCursor(Qt::PointingHandCursor);
            updateExtraSelections();
        }
    }
    QPlainTextEdit::keyPressEvent(e);
}

void CodeView::keyReleaseEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Control && !linkWord_.isNull()) {
        linkWord_ = QTextCursor();
        viewport()->setCursor(Qt::IBeamCursor);
        updateExtraSelections();
    }
    QPlainTextEdit::keyReleaseEvent(e);
}

void CodeView::leaveEvent(QEvent* e)
{
    hoverTimer_.stop();
    QPlainTextEdit::leaveEvent(e);
}

void CodeView::requestAt(const QTextCursor& c, bool declaration)
{
    int row = c.blockNumber();
    if (lineForRow(row) < 0)
        return;
    int col = (c.hasSelection() ? c.selectionStart() : c.position()) - c.block().position();
    emit definitionRequested(this, row, col, declaration);
}

void CodeView::contextMenuEvent(QContextMenuEvent* e)
{
    QTextCursor word;
    bool onWord = wordCursorAt(e->pos(), word);
    if (onWord)
        setTextCursor(word);
    QMenu* menu = createStandardContextMenu(e->pos());
    menu->addSeparator();
    auto* def = menu->addAction(tr("Go to Definition"), this, [this, word] { requestAt(word, false); });
    def->setShortcut(QKeySequence(Qt::Key_F12));
    auto* decl = menu->addAction(tr("Go to Declaration"), this, [this, word] { requestAt(word, true); });
    decl->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_F12));
    def->setEnabled(onWord);
    decl->setEnabled(onWord);
    int row = cursorForPosition(e->pos()).blockNumber();
    if (row >= 0 && row < rows_.size() && rows_[row].line >= 0 && !path_.isEmpty()) {
        const int line = rows_[row].line;
        const auto mark = bookmarkAt(row);
        menu->addSeparator();
        menu->addAction(mark ? tr("Remove Bookmark") : tr("Add Bookmark"), this,
                        [this, line] { emit bookmarkRequested(this, line, BookmarkAction::Toggle); })
            ->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_K));
        menu->addAction(mark && !mark->isEmpty() ? tr("Edit Comment…") : tr("Add Comment…"), this,
                        [this, line] { emit bookmarkRequested(this, line, BookmarkAction::EditComment); })
            ->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_K));
        if (openFileEnabled_)
            menu->addAction(tr("Open Whole File in Tab"), this,
                            [this, line] { emit openFileRequested(side_, path_, line); });
    }
    if (hunkReviewed_ && row >= 0 && row < rows_.size() && rows_[row].hunk >= 0) {
        const int hunk = rows_[row].hunk;
        menu->addSeparator();
        if (rows_[row].fold)
            menu->addAction(tr("Show Reviewed Hunk"), this, [this, hunk] { emit foldActivated(hunk); });
        if (hunkReviewed_(hunk))
            menu->addAction(tr("Unmark Hunk as Reviewed"), this, [this, hunk] { emit hunkReviewRequested(hunk, false); });
        else
            menu->addAction(tr("Mark Hunk as Reviewed"), this, [this, hunk] { emit hunkReviewRequested(hunk, true); })
                ->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
    }
    if (row >= 0 && row < rows_.size() && rows_[row].change >= 0) {
        int change = rows_[row].change;
        menu->addAction(tr("Open Semantic Change"), this, [this, change] { emit changeActivated(change); });
    }
    menu->exec(e->globalPos());
    delete menu;
}

} // namespace gui
