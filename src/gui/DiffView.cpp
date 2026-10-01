#include "DiffView.hpp"

#include "Theme.hpp"

#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSplitter>
#include <QTextBlock>
#include <QVBoxLayout>

namespace gui {

// Thin bar next to the diff showing where the changes are; click to jump.
class OverviewBar : public QWidget {
public:
    OverviewBar(CodeView* left, CodeView* right, QWidget* parent) : QWidget(parent), left_(left), right_(right)
    {
        setFixedWidth(14);
        setCursor(Qt::PointingHandCursor);
        connect(right_->verticalScrollBar(), &QScrollBar::valueChanged, this, qOverload<>(&QWidget::update));
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), palette().color(QPalette::AlternateBase));
        const auto& rows = right_->rows();
        const auto& lrows = left_->rows();
        const int n = static_cast<int>(rows.size());
        if (n == 0)
            return;
        const double scale = static_cast<double>(height()) / n;
        const auto& theme = Theme::current();
        for (int r = 0; r < n; ++r) {
            auto tag = rows[r].line >= 0 ? rows[r].tag : cr::LineTag::None;
            if (tag == cr::LineTag::None && r < lrows.size() && lrows[r].line >= 0)
                tag = lrows[r].tag;
            if (tag == cr::LineTag::None && rows[r].line < 0 && r < lrows.size())
                tag = lrows[r].tag;
            if (rows[r].fold) {
                QColor done = theme.changeColor(cr::ChangeKind::Added);
                done.setAlpha(90);
                p.fillRect(2, static_cast<int>(r * scale), width() - 4, std::max(2, static_cast<int>(scale)), done);
                continue;
            }
            if (tag == cr::LineTag::None)
                continue;
            QColor c = theme.lineBackground(tag).darker(theme.dark ? 60 : 140);
            if (tag == cr::LineTag::Deleted)
                c = QColor(0xcf, 0x22, 0x2e);
            else if (tag == cr::LineTag::Inserted)
                c = QColor(0x2d, 0xa4, 0x4e);
            else if (tag == cr::LineTag::Moved)
                c = QColor(0x09, 0x69, 0xda);
            else if (tag == cr::LineTag::RenameOnly)
                c = QColor(0x82, 0x50, 0xdf);
            else
                c = QColor(0xd4, 0xa7, 0x2c);
            int y = static_cast<int>(r * scale);
            p.fillRect(2, y, width() - 4, std::max(2, static_cast<int>(scale)), c);
        }
        // Visible region.
        const int top = right_->topRow();
        const int visible = right_->viewport()->height() / std::max(1, right_->fontMetrics().height());
        QColor frame = palette().color(QPalette::Text);
        frame.setAlpha(60);
        p.fillRect(0, static_cast<int>(top * scale), width(), std::max(4, static_cast<int>(visible * scale)), frame);
    }

    void mousePressEvent(QMouseEvent* e) override { jump(e->pos().y()); }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (e->buttons() & Qt::LeftButton)
            jump(e->pos().y());
    }

private:
    void jump(int y)
    {
        const int n = static_cast<int>(right_->rows().size());
        if (n == 0)
            return;
        const int visible = right_->viewport()->height() / std::max(1, right_->fontMetrics().height());
        int row = static_cast<int>(static_cast<double>(y) / height() * n);
        right_->verticalScrollBar()->setValue(std::max(0, row - visible / 2));
    }

    CodeView* left_;
    CodeView* right_;
};

DiffView::DiffView(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    title_ = new QLabel(this);
    title_->setTextFormat(Qt::RichText);
    title_->setContentsMargins(6, 4, 6, 2);
    title_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(title_);

    left_ = new CodeView(this);
    right_ = new CodeView(this);
    left_->setSource(cr::Side::Old, {});
    right_->setSource(cr::Side::New, {});

    auto makeSide = [this](CodeView* v, QLabel*& caption) {
        auto* w = new QWidget(this);
        auto* l = new QVBoxLayout(w);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(0);
        caption = new QLabel(w);
        caption->setContentsMargins(6, 2, 6, 2);
        caption->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
        l->addWidget(caption);
        l->addWidget(v);
        return w;
    };
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(makeSide(left_, leftCaption_));
    splitter->addWidget(makeSide(right_, rightCaption_));
    splitter->setChildrenCollapsible(false);

    overview_ = new OverviewBar(left_, right_, this);
    auto* row = new QHBoxLayout;
    row->setSpacing(0);
    row->addWidget(splitter, 1);
    row->addWidget(overview_);
    layout->addLayout(row, 1);

    // Both sides have the same number of rows, so scroll positions map 1:1.
    auto sync = [this](QScrollBar* from, QScrollBar* to) {
        connect(from, &QScrollBar::valueChanged, this, [this, to](int v) {
            if (syncing_)
                return;
            syncing_ = true;
            to->setValue(v);
            syncing_ = false;
            overview_->update();
        });
    };
    sync(left_->verticalScrollBar(), right_->verticalScrollBar());
    sync(right_->verticalScrollBar(), left_->verticalScrollBar());
    sync(left_->horizontalScrollBar(), right_->horizontalScrollBar());
    sync(right_->horizontalScrollBar(), left_->horizontalScrollBar());

    for (auto* v : {left_, right_}) {
        connect(v, &CodeView::definitionRequested, this, &DiffView::definitionRequested);
        connect(v, &CodeView::hoverRequested, this, &DiffView::hoverRequested);
        connect(v, &CodeView::changeActivated, this, &DiffView::changeActivated);
        connect(v, &CodeView::foldActivated, this, [this](int h) {
            if (h < 0 || h >= static_cast<int>(hunks_.size()))
                return;
            revealed_.insert(hunks_[static_cast<size_t>(h)].key);
            refreshFolding();
        });
        connect(v, &CodeView::hunkReviewRequested, this, &DiffView::setHunkReviewed);
        connect(v, &CodeView::bookmarkRequested, this, &DiffView::bookmarkRequested);
        connect(v, &CodeView::openFileRequested, this, &DiffView::openFileRequested);
        v->setOpenFileEnabled(true);
    }
}

void DiffView::setReviewedProvider(std::function<bool(const cr::Hunk&)> reviewed)
{
    reviewed_ = std::move(reviewed);
    auto isReviewed = [this](int h) {
        return reviewed_ && h >= 0 && h < static_cast<int>(hunks_.size()) && reviewed_(hunks_[static_cast<size_t>(h)]);
    };
    left_->setHunkReviewedProvider(isReviewed);
    right_->setHunkReviewedProvider(isReviewed);
}

bool DiffView::hidden(int hunk) const
{
    const auto& h = hunks_[static_cast<size_t>(hunk)];
    return foldReviewed_ && reviewed_ && !revealed_.contains(h.key) && reviewed_(h);
}

void DiffView::setHunkReviewed(int hunk, bool reviewed)
{
    if (hunk < 0 || hunk >= static_cast<int>(hunks_.size()))
        return;
    const auto key = hunks_[static_cast<size_t>(hunk)].key;
    if (reviewed)
        revealed_.remove(key);
    emit hunkReviewToggled(key, reviewed); // the owner updates its marks and calls refreshFolding()
}

int DiffView::rowOfHunk(int hunk) const
{
    const auto& rows = right_->rows();
    for (int r = 0; r < rows.size(); ++r)
        if (rows[r].hunk == hunk)
            return r;
    return -1;
}

void DiffView::refreshFolding()
{
    if (!fd_)
        return;
    // Keep the first visible line (or the hunk being folded) in place.
    const int top = right_->topRow();
    const int cursor = right_->cursorRow();
    auto anchor = [&](int row) {
        const auto& rows = right_->rows();
        const auto& lrows = left_->rows();
        for (int r = row; r < rows.size(); ++r) {
            if (rows[r].line >= 0)
                return std::pair{cr::Side::New, rows[r].line};
            if (r < lrows.size() && lrows[r].line >= 0)
                return std::pair{cr::Side::Old, lrows[r].line};
            if (rows[r].fold) {
                const auto& h = hunks_[static_cast<size_t>(rows[r].hunk)];
                return h.newBegin >= 0 ? std::pair{cr::Side::New, h.newBegin + newOffset_}
                                       : std::pair{cr::Side::Old, h.oldBegin + oldOffset_};
            }
        }
        return std::pair{cr::Side::New, -1};
    };
    const auto topAnchor = anchor(top);
    const auto cursorAnchor = anchor(cursor);
    render();
    // A line inside a folded hunk maps to the fold row, otherwise to the line's own row.
    auto rowOf = [&](std::pair<cr::Side, int> a) {
        if (a.second < 0)
            return -1;
        const int base = a.first == cr::Side::New ? newOffset_ : oldOffset_;
        for (size_t k = 0; k < hunks_.size(); ++k) {
            const auto& h = hunks_[k];
            const int b = a.first == cr::Side::New ? h.newBegin : h.oldBegin;
            const int e = a.first == cr::Side::New ? h.newEnd : h.oldEnd;
            if (b >= 0 && a.second - base >= b && a.second - base < e && hidden(static_cast<int>(k)))
                return rowOfHunk(static_cast<int>(k));
        }
        return view(a.first)->rowForLine(a.second);
    };
    const int topRow = rowOf(topAnchor);
    const int cursorRow = rowOf(cursorAnchor);
    if (cursorRow >= 0)
        for (auto* v : {left_, right_})
            v->setTextCursor(QTextCursor(v->document()->findBlockByNumber(cursorRow)));
    if (topRow >= 0)
        for (auto* v : {left_, right_})
            v->verticalScrollBar()->setValue(topRow);
}

bool DiffView::markHunkAtCursorReviewed()
{
    if (!reviewed_)
        return false;
    const auto& rows = right_->rows();
    const auto& lrows = left_->rows();
    CodeView* v = left_->hasFocus() ? left_ : right_;
    const int cur = std::clamp(v->cursorRow(), 0, std::max(0, static_cast<int>(rows.size()) - 1));
    // The hunk under the cursor, or the next one still to be reviewed.
    int hunk = -1;
    for (int r = cur; r < rows.size() && hunk < 0; ++r) {
        int h = rows[r].hunk >= 0 ? rows[r].hunk : (r < lrows.size() ? lrows[r].hunk : -1);
        if (h >= 0 && !reviewed_(hunks_[static_cast<size_t>(h)]))
            hunk = h;
    }
    if (hunk < 0)
        return false;
    setHunkReviewed(hunk, true);
    const int r = rowOfHunk(hunk);
    if (r >= 0) {
        left_->setTextCursor(QTextCursor(left_->document()->findBlockByNumber(r)));
        right_->setTextCursor(QTextCursor(right_->document()->findBlockByNumber(r)));
        right_->setFocus();
    }
    gotoHunk(true);
    return true;
}

void DiffView::scrollToFirstChange()
{
    const auto& rows = right_->rows();
    for (int r = 0; r < rows.size(); ++r)
        if (rows[r].hunk >= 0 && !rows[r].fold) {
            left_->scrollToRow(r, false);
            right_->scrollToRow(r, false);
            return;
        }
}

bool DiffView::reveal(cr::Side side, int line)
{
    for (size_t k = 0; k < hunks_.size(); ++k) {
        const auto& h = hunks_[k];
        const int b = side == cr::Side::New ? h.newBegin : h.oldBegin;
        const int e = side == cr::Side::New ? h.newEnd : h.oldEnd;
        if (b >= 0 && line >= b && line < e && hidden(static_cast<int>(k))) {
            revealed_.insert(h.key);
            refreshFolding();
            return true;
        }
    }
    return false;
}

void DiffView::setChangeMetaProvider(std::function<ChangeMeta(int)> p)
{
    left_->setChangeMetaProvider(p);
    right_->setChangeMetaProvider(std::move(p));
}

void DiffView::setSideCaptions(const QString& oldCaption, const QString& newCaption)
{
    leftCaption_->setText(oldCaption);
    rightCaption_->setText(newCaption);
}

namespace {

int charIndex(const std::string& line, int byteOffset)
{
    byteOffset = std::clamp(byteOffset, 0, static_cast<int>(line.size()));
    return static_cast<int>(QString::fromUtf8(line.data(), byteOffset).size());
}

} // namespace

void DiffView::showDiff(const cr::FileDiff& fd, const QString& title, int oldOffset, int newOffset)
{
    fd_ = fd;
    oldOffset_ = oldOffset;
    newOffset_ = newOffset;
    hunks_ = cr::diffHunks(*fd_);
    oldPath_ = QString::fromStdString(fd.oldPath);
    newPath_ = QString::fromStdString(fd.newPath);
    // Generated text (an interdiff) isn't a file of either revision: no navigation or bookmarks.
    left_->setSource(cr::Side::Old, fd.synthetic ? QString() : oldPath_);
    right_->setSource(cr::Side::New, fd.synthetic ? QString() : newPath_);
    title_->setText(title);
    render();
}

void DiffView::render()
{
    const auto& fd = *fd_;
    const int oldOffset = oldOffset_, newOffset = newOffset_;
    QStringList lt, rt;
    QVector<RowData> lr, rr;
    lt.reserve(static_cast<int>(fd.rows.size()));
    rt.reserve(static_cast<int>(fd.rows.size()));
    std::vector<int> hunkOfRow(fd.rows.size(), -1);
    for (size_t k = 0; k < hunks_.size(); ++k)
        for (int r = hunks_[k].rowBegin; r < hunks_[k].rowEnd; ++r)
            hunkOfRow[static_cast<size_t>(r)] = static_cast<int>(k);
    std::vector<cr::Span> as, bs;
    for (size_t ri = 0; ri < fd.rows.size(); ++ri) {
        const auto& row = fd.rows[ri];
        const int hunk = hunkOfRow[ri];
        if (hunk >= 0 && hidden(hunk)) {
            const auto& h = hunks_[static_cast<size_t>(hunk)];
            const int removed = h.oldBegin >= 0 ? h.oldEnd - h.oldBegin : 0;
            const int added = h.newBegin >= 0 ? h.newEnd - h.newBegin : 0;
            const QString text = tr("    ✓ reviewed: −%1 +%2 lines hidden (double-click to show)").arg(removed).arg(added);
            lt << text;
            rt << text;
            RowData f;
            f.hunk = hunk;
            f.fold = true;
            lr.push_back(f);
            rr.push_back(f);
            ri = static_cast<size_t>(h.rowEnd - 1);
            continue;
        }
        RowData l, r;
        l.hunk = r.hunk = hunk;
        auto outside = [](int line, int b, int e) { return b >= 0 && (line < b || line >= e); };
        l.context = row.oldLine >= 0 && outside(row.oldLine, fd.oldContentBegin, fd.oldContentEnd);
        r.context = row.newLine >= 0 && outside(row.newLine, fd.newContentBegin, fd.newContentEnd);
        if (row.oldLine >= 0) {
            const auto& text = fd.oldLines[static_cast<size_t>(row.oldLine)];
            lt << QString::fromStdString(text);
            l.line = row.oldLine + oldOffset;
            const auto& info = fd.oldInfo[static_cast<size_t>(row.oldLine)];
            l.tag = info.tag;
            l.change = info.change;
            if (fd.synthetic && static_cast<size_t>(row.oldLine) < fd.oldDisplay.size())
                l.display = fd.oldDisplay[static_cast<size_t>(row.oldLine)];
        } else {
            lt << QString();
        }
        if (row.newLine >= 0) {
            const auto& text = fd.newLines[static_cast<size_t>(row.newLine)];
            rt << QString::fromStdString(text);
            r.line = row.newLine + newOffset;
            const auto& info = fd.newInfo[static_cast<size_t>(row.newLine)];
            r.tag = info.tag;
            r.change = info.change;
            if (fd.synthetic && static_cast<size_t>(row.newLine) < fd.newDisplay.size())
                r.display = fd.newDisplay[static_cast<size_t>(row.newLine)];
        } else {
            rt << QString();
        }
        if (row.kind == cr::RowKind::Modified) {
            const auto& a = fd.oldLines[static_cast<size_t>(row.oldLine)];
            const auto& b = fd.newLines[static_cast<size_t>(row.newLine)];
            cr::intralineDiff(a, b, as, bs);
            for (const auto& s : as) {
                int st = charIndex(a, s.begin);
                l.spans.push_back({st, charIndex(a, s.end) - st});
            }
            for (const auto& s : bs) {
                int st = charIndex(b, s.begin);
                r.spans.push_back({st, charIndex(b, s.end) - st});
            }
        }
        lr.push_back(std::move(l));
        rr.push_back(std::move(r));
    }
    if (fd.binary) {
        lt = QStringList{tr("(binary file)")};
        rt = lt;
        lr = rr = QVector<RowData>{RowData{0, cr::LineTag::None, -1, {}}};
    }
    left_->setContent(lt, std::move(lr), true);
    right_->setContent(rt, std::move(rr), false);
    overview_->update();
}

int DiffView::rowCount() const
{
    return static_cast<int>(right_->rows().size());
}

void DiffView::scrollToLine(cr::Side side, int line, bool flash)
{
    // Navigating into a hunk hidden as reviewed shows it again.
    reveal(side, line - (side == cr::Side::New ? newOffset_ : oldOffset_));
    auto* v = view(side);
    int row = v->rowForLine(line);
    if (row < 0)
        return;
    v->scrollToRow(row, flash);
    auto* other = view(side == cr::Side::Old ? cr::Side::New : cr::Side::Old);
    other->verticalScrollBar()->setValue(v->verticalScrollBar()->value());
}

void DiffView::gotoHunk(bool forward)
{
    CodeView* v = right_->hasFocus() ? right_ : (left_->hasFocus() ? left_ : right_);
    const auto& rr = right_->rows();
    const auto& lr = left_->rows();
    auto changed = [&](int r) {
        if (r < rr.size() && rr[r].fold)
            return false; // reviewed
        return (r < rr.size() && rr[r].line >= 0 && rr[r].tag != cr::LineTag::None) ||
               (r < lr.size() && lr[r].line >= 0 && lr[r].tag != cr::LineTag::None) ||
               (r < rr.size() && r < lr.size() && (rr[r].line < 0 || lr[r].line < 0));
    };
    const int n = static_cast<int>(rr.size());
    int cur = v->cursorRow();
    if (forward) {
        int r = cur;
        while (r < n && changed(r)) // leave the current hunk
            ++r;
        while (r < n && !changed(r))
            ++r;
        if (r < n) {
            left_->scrollToRow(r, true);
            right_->scrollToRow(r, true);
        }
    } else {
        int r = cur - 1;
        while (r >= 0 && !changed(r))
            --r;
        while (r > 0 && changed(r - 1))
            --r;
        if (r >= 0) {
            left_->scrollToRow(r, true);
            right_->scrollToRow(r, true);
        }
    }
}

} // namespace gui
