#include "Theme.hpp"

namespace cad::app {
namespace {

/// A dark canvas is the professional default: drafters work in one for hours,
/// and light-on-dark line work is markedly less fatiguing than the reverse.
const Theme kDarkTheme{
    /* background       */ QColor{0x1C, 0x1C, 0x1E},
    /* foreground       */ QColor{0xF5, 0xF5, 0xF7},
    /* grid             */ QColor{0x2C, 0x2C, 0x2E},
    /* gridMajor        */ QColor{0x3A, 0x3A, 0x3C},
    /* selection        */ QColor{0x0A, 0x84, 0xFF},
    /* highlight        */ QColor{0x64, 0xD2, 0xFF},
    /* snapMarker       */ QColor{0x30, 0xD1, 0x58},
    /* crosshair        */ QColor{0x63, 0x63, 0x66},
    /* missingReference */ QColor{0xFF, 0x45, 0x3A},
    /* sheet            */ QColor{0xE8, 0xE8, 0xE4},
    /* fieldBackground  */ QColor{0x8E, 0x8E, 0x93, 0x4D},
    /* panel            */ QColor{0x24, 0x24, 0x26},
    /* panelAlternate   */ QColor{0x2A, 0x2A, 0x2D},
    /* panelBorder      */ QColor{0x35, 0x35, 0x38},
    /* text             */ QColor{0xE8, 0xE8, 0xEA},
    /* textMuted        */ QColor{0xA0, 0xA0, 0xA8},
    /* accent           */ QColor{0x0A, 0x84, 0xFF},
};

}  // namespace

const Theme& currentTheme() {
    return kDarkTheme;
}

QString applicationStyleSheet() {
    const Theme& t = currentTheme();

    const auto rgb = [](const QColor& c) {
        return QString{"rgb(%1,%2,%3)"}.arg(c.red()).arg(c.green()).arg(c.blue());
    };

    // Generous padding, restrained borders and a single accent colour. The aim
    // is calm, legible chrome that stays out of the way of the drawing.
    return QString{R"(
        QMainWindow, QWidget {
            background: %1;
            color: %2;
            font-size: 13px;
        }
        QToolBar {
            background: %3;
            border: none;
            border-bottom: 1px solid %4;
            padding: 6px 8px;
            spacing: 4px;
        }
        /* The ribbon rides in a toolbar so that it spans the window above the
           docks, but it is not a row of buttons: the padding and the rule that
           make a toolbar read as one would put a light band above and below a
           ribbon that already draws its own edges. */
        QToolBar#ribbonBar {
            background: transparent;
            border: none;
            padding: 0px;
            margin: 0px;
            spacing: 0px;
        }
        QToolBar QToolButton {
            background: transparent;
            color: %2;
            border: none;
            border-radius: 6px;
            padding: 6px 12px;
        }
        QToolBar QToolButton:hover   { background: %4; }
        QToolBar QToolButton:pressed { background: %5; color: white; }
        QToolBar QToolButton:disabled { color: %6; }

        /* Ribbon buttons. Tighter padding than the toolbar's, because a
           ribbon fits three rows in the height a toolbar gives one. */
        QToolButton {
            background: transparent;
            color: %2;
            border: none;
            border-radius: 5px;
            padding: 3px 9px;
        }
        QToolButton:hover   { background: %4; }
        QToolButton:pressed { background: %5; color: white; }

        QStatusBar {
            background: %3;
            border-top: 1px solid %4;
            color: %6;
            padding: 2px 8px;
        }
        QStatusBar::item { border: none; }

        QDockWidget {
            titlebar-close-icon: none;
            titlebar-normal-icon: none;
            color: %2;
        }
        QDockWidget::title {
            background: %3;
            padding: 8px 10px;
            border-bottom: 1px solid %4;
            font-weight: 600;
        }

        /* alternate-background-color is set explicitly. Left to Qt it comes
           from the platform palette -- a near-white band on a dark panel, and
           white labels sitting on it disappear completely. */
        QListWidget, QPlainTextEdit, QTreeWidget, QTreeView {
            background: %3;
            alternate-background-color: %7;
            color: %2;
            border: none;
            outline: none;
            padding: 4px;
        }
        QListWidget::item, QTreeWidget::item, QTreeView::item {
            padding: 6px 8px;
            border-radius: 5px;
            color: %2;
        }
        QTreeWidget::item:!enabled, QTreeView::item:!enabled { color: %6; }
        QListWidget::item:selected, QTreeWidget::item:selected,
        QTreeView::item:selected {
            background: %5;
            color: white;
        }
        /* The header is chrome, not content: quieter than the rows it labels,
           with a rule beneath so the two do not run together. */
        QHeaderView::section {
            background: %3;
            color: %6;
            border: none;
            border-bottom: 1px solid %4;
            padding: 6px 8px;
            font-weight: 600;
        }

        QLineEdit {
            background: %1;
            border: 1px solid %4;
            border-radius: 6px;
            padding: 7px 10px;
            selection-background-color: %5;
        }
        QLineEdit:focus { border: 1px solid %5; }

        /* Tabs: the ribbon's and the layout bar's. Unstyled, Qt's default
           palette renders these almost invisible on a dark background --
           dark text on a dark tab -- which is what makes an unstyled dark
           theme look broken rather than merely plain. */
        QTabWidget::pane {
            background: %3;
            border: none;
            border-top: 1px solid %4;
        }
        QTabBar { background: transparent; qproperty-drawBase: 0; }
        QTabBar::tab {
            background: transparent;
            color: %6;
            padding: 6px 16px;
            margin-right: 2px;
            border: none;
            border-top-left-radius: 6px;
            border-top-right-radius: 6px;
        }
        QTabBar::tab:hover { background: %4; color: %2; }
        QTabBar::tab:selected {
            background: %3;
            color: %2;
            font-weight: 600;
            border-bottom: 2px solid %5;
        }

        QMenuBar { background: %3; border-bottom: 1px solid %4; }
        QMenuBar::item { padding: 6px 10px; background: transparent; }
        QMenuBar::item:selected { background: %4; border-radius: 5px; }
        QMenu {
            background: %3;
            border: 1px solid %4;
            border-radius: 8px;
            padding: 5px;
        }
        QMenu::item { padding: 6px 24px 6px 12px; border-radius: 5px; }
        QMenu::item:selected { background: %5; color: white; }
        QMenu::separator { height: 1px; background: %4; margin: 5px 8px; }

        QScrollBar:vertical, QScrollBar:horizontal {
            background: transparent;
            width: 10px;
            height: 10px;
        }
        QScrollBar::handle {
            background: %4;
            border-radius: 5px;
            min-height: 24px;
            min-width: 24px;
        }
        QScrollBar::handle:hover { background: %6; }
        QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
        QScrollBar::add-page, QScrollBar::sub-page { background: none; }
    )"}
        .arg(rgb(t.background), rgb(t.text), rgb(t.panel), rgb(t.panelBorder), rgb(t.accent),
             rgb(t.textMuted), rgb(t.panelAlternate));
}

}  // namespace cad::app
