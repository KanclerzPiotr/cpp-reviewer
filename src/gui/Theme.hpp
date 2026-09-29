#pragma once

#include "core/Model.hpp"
#include "core/Review.hpp"

#include <QApplication>
#include <QColor>
#include <QPalette>

namespace gui {

// Colors for diff decorations, picked for the current (light or dark) palette.
struct Theme {
    bool dark = false;

    static const Theme& current() { return instance(); }

    // Follows the application palette; call after changing it.
    static void syncWithPalette()
    {
        instance().dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    }

    QColor lineBackground(cr::LineTag tag) const
    {
        switch (tag) {
        case cr::LineTag::Deleted: return dark ? QColor(0x4b, 0x1e, 0x1e) : QColor(0xff, 0xeb, 0xe9);
        case cr::LineTag::Inserted: return dark ? QColor(0x1d, 0x3b, 0x22) : QColor(0xe6, 0xff, 0xec);
        case cr::LineTag::Modified: return dark ? QColor(0x3d, 0x3a, 0x1c) : QColor(0xff, 0xf8, 0xd6);
        case cr::LineTag::Moved: return dark ? QColor(0x1c, 0x30, 0x4d) : QColor(0xe3, 0xef, 0xff);
        case cr::LineTag::RenameOnly: return dark ? QColor(0x36, 0x26, 0x4a) : QColor(0xf3, 0xea, 0xff);
        case cr::LineTag::None: break;
        }
        return {};
    }

    QColor intraline(bool oldSide) const
    {
        if (oldSide)
            return dark ? QColor(0x8a, 0x2e, 0x2e) : QColor(0xff, 0xb8, 0xb3);
        return dark ? QColor(0x2e, 0x6e, 0x38) : QColor(0xa8, 0xf0, 0xb6);
    }

private:
    static Theme& instance()
    {
        static Theme t = [] {
            Theme th;
            th.dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
            return th;
        }();
        return t;
    }

public:
    QColor filler() const { return dark ? QColor(0x30, 0x30, 0x30) : QColor(0xe4, 0xe4, 0xe4); }
    QColor flash() const { return dark ? QColor(0x80, 0x70, 0x20) : QColor(0xff, 0xe0, 0x66); }

    QColor changeColor(cr::ChangeKind k) const
    {
        using K = cr::ChangeKind;
        switch (k) {
        case K::Added: return QColor(0x2d, 0xa4, 0x4e);
        case K::Removed: return QColor(0xcf, 0x22, 0x2e);
        case K::Modified: return QColor(0xbf, 0x87, 0x00);
        case K::Renamed:
        case K::SymbolRenamed: return QColor(0x82, 0x50, 0xdf);
        case K::Moved:
        case K::Reordered:
        case K::MovedCode:
        case K::MovedLines: return QColor(0x09, 0x69, 0xda);
        case K::SignatureChanged: return QColor(0xd1, 0x5f, 0x04);
        case K::Extracted:
        case K::Inlined: return QColor(0x1a, 0x7f, 0x7a);
        case K::CopiedCode: return QColor(0x9a, 0x67, 0x00);
        }
        return Qt::gray;
    }

    static QString badge(cr::ChangeKind k)
    {
        using K = cr::ChangeKind;
        switch (k) {
        case K::Added: return "A";
        case K::Removed: return "D";
        case K::Modified: return "M";
        case K::Renamed: return "R";
        case K::SymbolRenamed: return "r";
        case K::Moved: return "→";
        case K::Reordered: return "↕";
        case K::MovedCode: return "⇢";
        case K::MovedLines: return "≡";
        case K::SignatureChanged: return "S";
        case K::Extracted: return "X";
        case K::Inlined: return "I";
        case K::CopiedCode: return "C";
        }
        return "?";
    }
};

} // namespace gui
