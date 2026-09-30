#include "CppHighlighter.hpp"

#include "Theme.hpp"

namespace gui {

CppHighlighter::CppHighlighter(QTextDocument* doc) : QSyntaxHighlighter(doc)
{
    commentStart_ = QRegularExpression(R"(/\*)");
    commentEnd_ = QRegularExpression(R"(\*/)");
    applyTheme();
}

void CppHighlighter::applyTheme()
{
    rules_.clear();
    comment_ = string_ = preproc_ = QTextCharFormat();
    const bool dark = Theme::current().dark;
    QTextCharFormat keyword, type, number, func;
    keyword.setForeground(dark ? QColor(0xff, 0x7b, 0x72) : QColor(0xcf, 0x22, 0x2e));
    keyword.setFontWeight(QFont::DemiBold);
    type.setForeground(dark ? QColor(0x79, 0xc0, 0xff) : QColor(0x05, 0x50, 0xae));
    number.setForeground(dark ? QColor(0x79, 0xc0, 0xff) : QColor(0x09, 0x69, 0xda));
    func.setForeground(dark ? QColor(0xd2, 0xa8, 0xff) : QColor(0x82, 0x50, 0xdf));
    string_.setForeground(dark ? QColor(0xa5, 0xd6, 0xff) : QColor(0x0a, 0x30, 0x69));
    comment_.setForeground(dark ? QColor(0x8b, 0x94, 0x9e) : QColor(0x6e, 0x77, 0x81));
    comment_.setFontItalic(true);
    preproc_.setForeground(dark ? QColor(0xff, 0xa6, 0x57) : QColor(0x95, 0x38, 0x00));

    const char* keywords[] = {
        "alignas", "alignof", "asm", "auto", "break", "case", "catch", "class", "co_await", "co_return",
        "co_yield", "concept", "const", "consteval", "constexpr", "constinit", "const_cast", "continue",
        "decltype", "default", "delete", "do", "dynamic_cast", "else", "enum", "explicit", "export", "extern",
        "false", "final", "for", "friend", "goto", "if", "inline", "mutable", "namespace", "new", "noexcept",
        "nullptr", "operator", "override", "private", "protected", "public", "register", "reinterpret_cast",
        "requires", "return", "sizeof", "static", "static_assert", "static_cast", "struct", "switch", "template",
        "this", "thread_local", "throw", "true", "try", "typedef", "typeid", "typename", "union", "using",
        "virtual", "volatile", "while"};
    const char* types[] = {"bool", "char", "char8_t", "char16_t", "char32_t", "double", "float", "int", "long",
                           "short", "signed", "unsigned", "void", "wchar_t", "size_t", "ptrdiff_t", "int8_t",
                           "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t"};

    // Order matters: later rules override earlier ones.
    rules_.push_back({QRegularExpression(R"(\b[A-Za-z_]\w*(?=\s*\())"), func});
    for (auto k : keywords)
        rules_.push_back({QRegularExpression(QStringLiteral("\\b%1\\b").arg(k)), keyword});
    for (auto t : types)
        rules_.push_back({QRegularExpression(QStringLiteral("\\b%1\\b").arg(t)), type});
    rules_.push_back({QRegularExpression(R"(\b(0[xX][0-9a-fA-F']+|\d[\d']*(\.\d*)?([eE][+-]?\d+)?)[uUlLfF]*\b)"), number});
    rules_.push_back({QRegularExpression(R"(^\s*#\s*\w+)"), preproc_});
    rules_.push_back({QRegularExpression(R"(<[\w./+-]+>(?=\s*$))"), string_}); // #include <...>
    rules_.push_back({QRegularExpression(R"('(\\.|[^'\\])*')"), string_});
    rules_.push_back({QRegularExpression(R"("(\\.|[^"\\])*")"), string_});
    rules_.push_back({QRegularExpression(R"(//.*$)"), comment_});
    rehighlight();
}

void CppHighlighter::highlightBlock(const QString& text)
{
    if (plain_ && plain_(currentBlock().blockNumber())) {
        setFormat(0, static_cast<int>(text.size()), comment_);
        setCurrentBlockState(previousBlockState()); // a block comment continues past it
        return;
    }
    for (const auto& r : rules_) {
        auto it = r.re.globalMatch(text);
        while (it.hasNext()) {
            auto m = it.next();
            setFormat(static_cast<int>(m.capturedStart()), static_cast<int>(m.capturedLength()), r.fmt);
        }
    }

    // Block comments (state 1 = inside a comment at the end of the block).
    setCurrentBlockState(0);
    int start = 0;
    if (previousBlockState() != 1) {
        auto m = commentStart_.match(text);
        start = m.hasMatch() ? static_cast<int>(m.capturedStart()) : -1;
        // Ignore "/*" that sits inside a // comment or a string.
        if (start > 0 && format(start) == string_)
            start = -1;
    }
    while (start >= 0) {
        auto end = commentEnd_.match(text, start + (previousBlockState() == 1 && start == 0 ? 0 : 2));
        int length;
        if (!end.hasMatch()) {
            setCurrentBlockState(1);
            length = static_cast<int>(text.length()) - start;
        } else {
            length = static_cast<int>(end.capturedEnd()) - start;
        }
        setFormat(start, length, comment_);
        auto next = commentStart_.match(text, start + length);
        start = next.hasMatch() ? static_cast<int>(next.capturedStart()) : -1;
    }

    if (extras_) {
        for (const auto& e : extras_(currentBlock().blockNumber())) {
            for (int i = e.start; i < e.start + e.length && i < text.length(); ++i) {
                QTextCharFormat f = format(i);
                f.setBackground(e.background);
                setFormat(i, 1, f);
            }
        }
    }
}

} // namespace gui
