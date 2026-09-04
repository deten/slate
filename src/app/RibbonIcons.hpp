#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

namespace cad::app {

/// A small line drawing of what a command does, by command name.
///
/// Drawn rather than loaded. Every one of these is a *geometric* operation --
/// a line, an arc, a mirrored pair, a trimmed corner -- so the honest picture of
/// it is the geometry itself, and geometry is what this application already
/// knows how to draw. A folder of PNGs would need one file per command per
/// theme per screen density, and would still be a drawing of a drawing.
///
/// They share a deliberate vocabulary so a row of them reads as a set:
///
/// - one weight of stroke, rounded at the ends, on a 24-unit grid;
/// - existing geometry in the foreground colour, and the *result* of the
///   command -- the copy, the new segment, the trimmed-away part -- in the
///   accent colour, so what a command adds is what stands out;
/// - no fills, no shadows, no perspective. A ribbon is read at a glance and at
///   sixteen pixels, where all three become noise.
///
/// An unknown command gets its initial in a rounded square, which is honest
/// about being a placeholder rather than pretending to be a picture.
[[nodiscard]] QIcon ribbonIcon(const QString& command, const QString& label, int size,
                               const QColor& stroke, const QColor& accent);

}  // namespace cad::app
