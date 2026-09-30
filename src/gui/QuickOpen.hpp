#pragma once

#include "core/Model.hpp"

#include <QDialog>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class QLabel;
class QLineEdit;
class QListWidget;
class QRadioButton;

namespace gui {

// Ctrl+P: pick any file of either revision by typing parts of its path ("file:123" jumps to a line).
class QuickOpenDialog : public QDialog {
    Q_OBJECT
public:
    using Files = std::shared_ptr<const std::vector<std::string>>;
    // `files(side, done)` delivers the file list of a revision, possibly asynchronously.
    using FilesFn = std::function<void(cr::Side side, std::function<void(Files)> done)>;

    QuickOpenDialog(FilesFn files, cr::Side side, QWidget* parent = nullptr);

    cr::Side side() const;
    QString path() const { return path_; }
    int line() const { return line_; } // 1-based, 0 if not given

protected:
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    void load();
    void refilter();
    void acceptCurrent();

    FilesFn filesFn_;
    Files files_;
    QLineEdit* edit_;
    QListWidget* list_;
    QRadioButton* target_;
    QRadioButton* base_;
    QLabel* info_;
    QString path_;
    int line_ = 0;
    int generation_ = 0;
};

} // namespace gui
