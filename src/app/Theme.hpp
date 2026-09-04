#pragma once

#include <QColor>
#include <QString>

namespace cad::app {

/// The application's visual language.
///
/// A deliberate design system rather than stock Qt: the brief asks for the
/// clarity and polish of software designed by Apple, and stock Qt widgets do
/// not read that way. Colours are defined once here and applied through a
/// stylesheet, so the whole application stays consistent and a light theme is
/// a data change rather than a code change.
struct Theme {
    // Canvas
    QColor background;
    QColor foreground;
    QColor grid;
    QColor gridMajor;

    // Interaction
    QColor selection;
    QColor highlight;
    QColor snapMarker;
    QColor crosshair;
    QColor missingReference;
    /// The paper of a layout sheet. Deliberately near-white even on the dark
    /// canvas: paper space is a picture of a printed page, and showing it in
    /// any other colour makes the sheet unreadable as one.
    QColor sheet;
    /// Behind text a field produces, as other CAD software shades it.
    ///
    /// Translucent and low-contrast on purpose: it has to be legible enough to
    /// notice and faint enough that a drawing full of area labels does not
    /// become a page of grey boxes. It says "this is derived", nothing more.
    QColor fieldBackground;

    // Chrome
    QColor panel;
    /// Banding for alternate rows in lists and tables.
    ///
    /// Part of the design system rather than left to Qt, which falls back to
    /// the platform palette — a near-white band on a dark panel, against which
    /// white labels disappear entirely. A stripe should help the eye track a
    /// row, not fight the text sitting on it, so this is a few percent lighter
    /// than `panel` and nothing more.
    QColor panelAlternate;
    QColor panelBorder;
    QColor text;
    QColor textMuted;
    QColor accent;
};

[[nodiscard]] const Theme& currentTheme();

/// The application stylesheet for the current theme.
[[nodiscard]] QString applicationStyleSheet();

}  // namespace cad::app
