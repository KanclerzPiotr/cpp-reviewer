#pragma once

#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <functional>

namespace gui {

// C++ syntax highlighting plus per-line extra ranges (e.g. intraline diff) supplied by the view.
class CppHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    struct Extra {
        int start, length;
        QColor background;
    };
    using ExtraProvider = std::function<QVector<Extra>(int blockNumber)>;

    explicit CppHighlighter(QTextDocument* doc);
    void setExtraProvider(ExtraProvider p) { extras_ = std::move(p); }
    // Rebuilds the formats for the current Theme and re-highlights.
    void applyTheme();

protected:
    void highlightBlock(const QString& text) override;

private:
    struct Rule {
        QRegularExpression re;
        QTextCharFormat fmt;
    };
    QVector<Rule> rules_;
    QTextCharFormat comment_, string_, preproc_;
    QRegularExpression commentStart_, commentEnd_;
    ExtraProvider extras_;
};

} // namespace gui
