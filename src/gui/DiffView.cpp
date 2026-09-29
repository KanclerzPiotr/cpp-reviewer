#include "DiffView.hpp"

#include "Theme.hpp"

#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSplitter>
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
    }
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
    oldPath_ = QString::fromStdString(fd.oldPath);
    newPath_ = QString::fromStdString(fd.newPath);
    left_->setSource(cr::Side::Old, oldPath_);
    right_->setSource(cr::Side::New, newPath_);
    title_->setText(title);

    QStringList lt, rt;
    QVector<RowData> lr, rr;
    lt.reserve(static_cast<int>(fd.rows.size()));
    rt.reserve(static_cast<int>(fd.rows.size()));
    std::vector<cr::Span> as, bs;
    for (const auto& row : fd.rows) {
        RowData l, r;
        if (row.oldLine >= 0) {
            const auto& text = fd.oldLines[static_cast<size_t>(row.oldLine)];
            lt << QString::fromStdString(text);
            l.line = row.oldLine + oldOffset;
            const auto& info = fd.oldInfo[static_cast<size_t>(row.oldLine)];
            l.tag = info.tag;
            l.change = info.change;
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
