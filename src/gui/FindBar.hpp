#pragma once

#include <QTextDocument>
#include <QWidget>

class QLabel;
class QLineEdit;
class QToolButton;

namespace gui {

// Incremental search bar shown under the code views (Ctrl+F).
class FindBar : public QWidget {
    Q_OBJECT
public:
    explicit FindBar(QWidget* parent = nullptr);

    QString text() const;
    QTextDocument::FindFlags flags() const;
    // Shows the bar, optionally with a new search text, and focuses it.
    void activate(const QString& initial);
    void setMatchInfo(int matches, bool found);

signals:
    void searchChanged();        // text or options changed
    void findRequested(bool backward);
    void closed();

protected:
    void keyPressEvent(QKeyEvent* e) override;

private:
    QLineEdit* edit_;
    QToolButton* caseButton_;
    QToolButton* wordButton_;
    QLabel* info_;
};

} // namespace gui
