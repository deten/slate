#include "RibbonIcons.hpp"

#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QtMath>
#include <functional>
#include <unordered_map>

namespace cad::app {
namespace {

/// Everything is drawn on this grid and scaled to the size asked for, so one
/// icon looks the same at sixteen pixels as at thirty-two.
constexpr qreal kGrid = 24.0;

struct Ink {
    QPainter* painter;
    QPen solid;
    QPen accent;
    QPen faint;

    void line(qreal x1, qreal y1, qreal x2, qreal y2) const {
        painter->setPen(solid);
        painter->drawLine(QPointF{x1, y1}, QPointF{x2, y2});
    }
    void newLine(qreal x1, qreal y1, qreal x2, qreal y2) const {
        painter->setPen(accent);
        painter->drawLine(QPointF{x1, y1}, QPointF{x2, y2});
    }
    void ghost(qreal x1, qreal y1, qreal x2, qreal y2) const {
        painter->setPen(faint);
        painter->drawLine(QPointF{x1, y1}, QPointF{x2, y2});
    }
    void rect(qreal x, qreal y, qreal w, qreal h, bool isNew = false) const {
        painter->setPen(isNew ? accent : solid);
        painter->drawRect(QRectF{x, y, w, h});
    }
    void circle(qreal cx, qreal cy, qreal r, bool isNew = false) const {
        painter->setPen(isNew ? accent : solid);
        painter->drawEllipse(QPointF{cx, cy}, r, r);
    }
    void arc(qreal cx, qreal cy, qreal r, int startDeg, int spanDeg, bool isNew = false) const {
        painter->setPen(isNew ? accent : solid);
        painter->drawArc(QRectF{cx - r, cy - r, r * 2, r * 2}, startDeg * 16, spanDeg * 16);
    }
    /// A filled node, for the points a command works from.
    void node(qreal x, qreal y, bool isNew = false) const {
        painter->setPen(Qt::NoPen);
        painter->setBrush(isNew ? accent.color() : solid.color());
        painter->drawEllipse(QPointF{x, y}, 1.6, 1.6);
        painter->setBrush(Qt::NoBrush);
    }
    void path(const QPainterPath& p, bool isNew = false) const {
        painter->setPen(isNew ? accent : solid);
        painter->drawPath(p);
    }
    /// The arrow every "move this somewhere" command shares.
    void arrow(qreal x1, qreal y1, qreal x2, qreal y2) const {
        painter->setPen(accent);
        painter->drawLine(QPointF{x1, y1}, QPointF{x2, y2});
        // Two strokes back from the tip, each turned by a fixed angle from the
        // shaft. The first attempt divided the spread by four and drew both
        // barbs almost along the shaft, which at sixteen pixels made every
        // arrow in the ribbon a dot.
        const qreal angle = std::atan2(y2 - y1, x2 - x1);
        constexpr qreal kBarb = 3.6;
        constexpr qreal kSpread = 0.45;
        for (const qreal side : {-1.0, 1.0}) {
            painter->drawLine(QPointF{x2, y2},
                              QPointF{x2 - kBarb * std::cos(angle + side * kSpread),
                                      y2 - kBarb * std::sin(angle + side * kSpread)});
        }
    }
};

using Drawing = std::function<void(const Ink&)>;

/// The letter shape used by the annotation commands, so text tools read as a
/// family rather than as three unrelated pictures.
void letterA(const Ink& ink, qreal x, qreal y, qreal h) {
    ink.line(x, y + h, x + h * 0.42, y);
    ink.line(x + h * 0.42, y, x + h * 0.84, y + h);
    ink.line(x + h * 0.16, y + h * 0.66, x + h * 0.68, y + h * 0.66);
}

[[nodiscard]] const std::unordered_map<QString, Drawing>& drawings() {
    static const std::unordered_map<QString, Drawing> table = {
        // ---------------------------------------------------------- drawing
        {"LINE", [](const Ink& i) { i.newLine(4, 19, 20, 5); i.node(4, 19); i.node(20, 5); }},
        {"PLINE",
         [](const Ink& i) {
             i.newLine(3, 18, 9, 9);
             i.newLine(9, 9, 15, 15);
             i.newLine(15, 15, 21, 5);
             i.node(3, 18);
             i.node(9, 9);
             i.node(15, 15);
             i.node(21, 5);
         }},
        // A measured span: a line between two ticks, with the ticks accented
        // because they are the thing being picked.
        {"DIST",
         [](const Ink& i) {
             i.line(4, 16, 20, 16);
             i.newLine(4, 12, 4, 20);
             i.newLine(20, 12, 20, 20);
         }},
        // An enclosed region, with the enclosure accented rather than the
        // outline: the area is the answer, not the boundary.
        {"AREA",
         [](const Ink& i) {
             i.rect(4, 6, 16, 13);
             i.ghost(4, 10, 20, 10);
             i.ghost(4, 14, 20, 14);
             i.ghost(4, 18, 20, 18);
         }},
        {"ID", [](const Ink& i) { i.line(6, 12, 18, 12); i.line(12, 6, 12, 18); i.node(12, 12); }},
        {"CIRCLE", [](const Ink& i) { i.circle(12, 12, 8, true); i.node(12, 12); }},
        {"ARC", [](const Ink& i) { i.arc(12, 15, 9, 0, 180, true); i.node(3, 15); i.node(21, 15); }},
        {"RECTANG", [](const Ink& i) { i.rect(4, 6, 16, 12, true); i.node(4, 6); i.node(20, 18); }},
        {"POLYGON",
         [](const Ink& i) {
             QPainterPath p;
             for (int k = 0; k < 6; ++k) {
                 const qreal a = qDegreesToRadians(60.0 * k - 90.0);
                 const QPointF point{12 + 8 * std::cos(a), 12 + 8 * std::sin(a)};
                 if (k == 0) {
                     p.moveTo(point);
                 } else {
                     p.lineTo(point);
                 }
             }
             p.closeSubpath();
             i.path(p, true);
         }},
        {"POINT",
         [](const Ink& i) {
             i.ghost(12, 4, 12, 20);
             i.ghost(4, 12, 20, 12);
             i.node(12, 12, true);
         }},
        {"ELLIPSE",
         [](const Ink& i) {
             i.painter->setPen(i.accent);
             i.painter->drawEllipse(QRectF{3, 7, 18, 10});
             i.node(12, 12);
         }},
        {"HATCH",
         [](const Ink& i) {
             i.rect(4, 5, 16, 14);
             // Three strokes, not seven. At sixteen pixels a closer spacing
             // stops being hatching and becomes a filled blue rectangle.
             i.painter->setPen(i.accent);
             i.painter->setClipRect(QRectF{4.5, 5.5, 15, 13});
             for (qreal x = -4; x < 22; x += 7.5) {
                 i.painter->drawLine(QPointF{x, 20}, QPointF{x + 14, 4});
             }
             i.painter->setClipping(false);
         }},

        // --------------------------------------------------------- modifying
        {"MOVE",
         [](const Ink& i) {
             // One shape and a long arrow, rather than two shapes and a short
             // one. An arrow of five units cannot carry a head at ribbon size:
             // the barbs are three and a half, so the whole thing collapsed
             // into a dot between two squares -- and two squares is what Copy
             // already means.
             i.rect(3, 12, 9, 9);
             i.arrow(10, 13, 21, 4);
         }},
        {"COPY",
         [](const Ink& i) {
             i.rect(3, 11, 9, 9);
             i.rect(12, 4, 9, 9, true);
         }},
        {"ROTATE",
         [](const Ink& i) {
             i.arc(12, 12, 8, 20, 290, true);
             i.node(12, 12);
             i.painter->setPen(i.accent);
             i.painter->drawLine(QPointF{19, 8}, QPointF{20, 3});
             i.painter->drawLine(QPointF{19, 8}, QPointF{15, 6});
         }},
        {"MIRROR",
         [](const Ink& i) {
             i.ghost(12, 3, 12, 21);
             QPainterPath left;
             left.moveTo(9, 7);
             left.lineTo(3, 12);
             left.lineTo(9, 17);
             left.closeSubpath();
             i.path(left);
             QPainterPath right;
             right.moveTo(15, 7);
             right.lineTo(21, 12);
             right.lineTo(15, 17);
             right.closeSubpath();
             i.path(right, true);
         }},
        {"SCALE",
         [](const Ink& i) {
             i.rect(3, 13, 7, 7);
             i.rect(3, 5, 15, 15, true);
             i.arrow(9, 14, 19, 4);
         }},
        {"STRETCH",
         [](const Ink& i) {
             QPainterPath p;
             p.moveTo(3, 17);
             p.lineTo(9, 8);
             p.lineTo(15, 8);
             p.lineTo(21, 17);
             i.path(p);
             i.arrow(15, 12, 21, 12);
             i.node(9, 8);
             i.node(15, 8);
         }},
        {"OFFSET",
         [](const Ink& i) {
             QPainterPath inner;
             inner.moveTo(7, 18);
             inner.lineTo(7, 9);
             inner.lineTo(16, 9);
             i.path(inner);
             QPainterPath outer;
             outer.moveTo(3, 21);
             outer.lineTo(3, 5);
             outer.lineTo(20, 5);
             i.path(outer, true);
         }},
        {"TRIM",
         [](const Ink& i) {
             // The cutting edge crosses, the kept part runs up to it, and the
             // trimmed-away part is gone rather than dashed. Drawing the
             // remainder as a dashed continuation made the whole icon a plus
             // sign at ribbon size -- the eye read one long horizontal through
             // one long vertical, which is the one thing it must not say.
             i.ghost(15, 4, 15, 20);
             i.newLine(3, 12, 15, 12);
             i.painter->setPen(i.accent);
             i.painter->drawLine(QPointF{18, 9}, QPointF{22, 13});
             i.painter->drawLine(QPointF{22, 9}, QPointF{18, 13});
         }},
        {"EXTEND",
         [](const Ink& i) {
             i.ghost(20, 3, 20, 21);
             i.line(3, 12, 12, 12);
             i.arrow(12, 12, 19, 12);
         }},
        {"FILLET",
         [](const Ink& i) {
             QPainterPath p;
             p.moveTo(4, 20);
             p.lineTo(4, 12);
             p.arcTo(QRectF{4, 4, 16, 16}, 180, -90);
             p.lineTo(20, 4);
             i.path(p, true);
             i.ghost(4, 12, 4, 4);
             i.ghost(4, 4, 12, 4);
         }},
        {"CHAMFER",
         [](const Ink& i) {
             QPainterPath p;
             p.moveTo(4, 20);
             p.lineTo(4, 10);
             p.lineTo(14, 4);
             p.lineTo(20, 4);
             i.path(p, true);
             i.ghost(4, 10, 4, 4);
             i.ghost(4, 4, 14, 4);
         }},
        {"BREAK",
         [](const Ink& i) {
             i.line(3, 12, 9, 12);
             i.line(15, 12, 21, 12);
             i.painter->setPen(i.accent);
             i.painter->drawLine(QPointF{10, 16}, QPointF{13, 8});
         }},
        {"JOIN",
         [](const Ink& i) {
             i.newLine(3, 12, 21, 12);
             i.node(3, 12);
             i.node(21, 12);
             i.node(12, 12, true);
         }},
        {"ERASE",
         [](const Ink& i) {
             i.ghost(4, 6, 16, 18);
             QPainterPath p;
             p.moveTo(8, 20);
             p.lineTo(4, 16);
             p.lineTo(14, 6);
             p.lineTo(18, 10);
             p.closeSubpath();
             i.path(p, true);
             i.line(9, 11, 13, 15);
         }},
        {"EXPLODE",
         [](const Ink& i) {
             i.rect(9, 9, 6, 6);
             for (const QPointF to : {QPointF{4, 4}, QPointF{20, 4}, QPointF{4, 20},
                                      QPointF{20, 20}}) {
                 i.arrow(12 + (to.x() - 12) * 0.35, 12 + (to.y() - 12) * 0.35, to.x(), to.y());
             }
         }},
        {"UNDO", [](const Ink& i) {
             i.arc(12, 13, 8, 20, 160, true);
             i.painter->setPen(i.accent);
             i.painter->drawLine(QPointF{4, 13}, QPointF{4, 8});
             i.painter->drawLine(QPointF{4, 13}, QPointF{9, 13});
         }},
        {"REDO", [](const Ink& i) {
             i.arc(12, 13, 8, 0, 160, true);
             i.painter->setPen(i.accent);
             i.painter->drawLine(QPointF{20, 13}, QPointF{20, 8});
             i.painter->drawLine(QPointF{20, 13}, QPointF{15, 13});
         }},

        // ------------------------------------------------------------- file
        //
        // The one group here that is not a geometric operation, and so the one
        // group that has to borrow a picture rather than draw the truth: a
        // sheet, a folder, a disk, a printer. They are on the quick-access
        // strip beside Undo and Redo, where the eye reads the whole row at
        // once, so they keep the same stroke and the same accent-for-the-result
        // rule as everything else.
        {"MENU", [](const Ink& i) {
             i.line(4, 7, 20, 7);
             i.line(4, 12, 20, 12);
             i.line(4, 17, 20, 17);
         }},
        {"NEW", [](const Ink& i) {
             QPainterPath sheet;
             sheet.moveTo(6, 3);
             sheet.lineTo(14, 3);
             sheet.lineTo(19, 8);
             sheet.lineTo(19, 21);
             sheet.lineTo(6, 21);
             sheet.closeSubpath();
             i.path(sheet, true);
             // The folded corner, in the foreground colour: the sheet is what
             // is new, the fold is just paper.
             i.line(14, 3, 14, 8);
             i.line(14, 8, 19, 8);
         }},
        {"OPEN", [](const Ink& i) {
             QPainterPath back;
             back.moveTo(3, 20);
             back.lineTo(3, 5);
             back.lineTo(10, 5);
             back.lineTo(12, 8);
             back.lineTo(19, 8);
             back.lineTo(19, 12);
             i.path(back);
             QPainterPath front;
             front.moveTo(3, 20);
             front.lineTo(7, 12);
             front.lineTo(21, 12);
             front.lineTo(17, 20);
             front.closeSubpath();
             i.path(front, true);
         }},
        {"SAVE", [](const Ink& i) {
             QPainterPath body;
             body.moveTo(4, 4);
             body.lineTo(17, 4);
             body.lineTo(20, 7);
             body.lineTo(20, 20);
             body.lineTo(4, 20);
             body.closeSubpath();
             i.path(body);
             i.rect(8, 4, 8, 5);
             i.rect(7, 13, 10, 7, true);
         }},
        {"PLOT", [](const Ink& i) {
             i.rect(7, 3, 10, 5);
             QPainterPath body;
             body.moveTo(4, 8);
             body.lineTo(20, 8);
             body.lineTo(20, 16);
             body.lineTo(4, 16);
             body.closeSubpath();
             i.path(body);
             // What comes out is the point of the command, so it is the part
             // in the accent colour.
             i.rect(7, 13, 10, 8, true);
         }},

        // --------------------------------------------------------- clipboard
        //
        // Borrowed pictures too, and deliberately the ones every application
        // uses: a drafter looking for paste is looking for a clipboard, not for
        // a diagram of what pasting does.
        {"COPYCLIP", [](const Ink& i) {
             i.rect(4, 4, 12, 14);
             i.rect(9, 8, 12, 14, true);
         }},
        {"CUTCLIP", [](const Ink& i) {
             i.line(7, 4, 16, 16);
             i.line(17, 4, 8, 16);
             i.circle(6, 19, 3, true);
             i.circle(18, 19, 3, true);
         }},
        {"PASTECLIP", [](const Ink& i) {
             // The board, with its clip, and the sheet coming onto it.
             i.rect(4, 5, 13, 16);
             i.rect(8, 3, 5, 4);
             i.rect(11, 11, 10, 11, true);
         }},
        {"PASTEORIG", [](const Ink& i) {
             i.rect(4, 5, 13, 16);
             i.rect(8, 3, 5, 4);
             i.rect(11, 11, 10, 11, true);
             i.node(16, 16, true);
         }},

        // -------------------------------------------------------- annotation
        {"TEXT", [](const Ink& i) { letterA(i, 7, 5, 14); }},
        {"MTEXT",
         [](const Ink& i) {
             letterA(i, 3, 5, 11);
             i.painter->setPen(i.faint);
             for (qreal y = 8; y <= 18; y += 5) {
                 i.painter->drawLine(QPointF{15, y}, QPointF{21, y});
             }
         }},
        {"TEXTEDIT",
         [](const Ink& i) {
             letterA(i, 3, 6, 12);
             QPainterPath pencil;
             pencil.moveTo(14, 19);
             pencil.lineTo(15, 15);
             pencil.lineTo(21, 9);
             pencil.lineTo(23, 11);
             pencil.lineTo(17, 17);
             pencil.closeSubpath();
             i.path(pencil, true);
         }},
        {"LEADER",
         [](const Ink& i) {
             // The arrow is the accent: what a leader does is *point*, and the
             // words at the other end are any words.
             i.arrow(14, 16, 4, 21);
             i.line(14, 16, 19, 8);
             i.line(19, 8, 22, 8);
             i.painter->setPen(i.faint);
             i.painter->drawLine(QPointF{13, 5}, QPointF{22, 5});
             i.painter->drawLine(QPointF{16, 2}, QPointF{22, 2});
         }},
        {"UPDATEFIELD",
         [](const Ink& i) {
             i.rect(3, 7, 12, 10);
             i.arc(16, 12, 6, 300, 300, true);
             i.node(16, 6, true);
         }},
        {"DIMLINEAR",
         [](const Ink& i) {
             i.ghost(4, 4, 4, 12);
             i.ghost(20, 4, 20, 12);
             i.newLine(4, 16, 20, 16);
             i.node(4, 16, true);
             i.node(20, 16, true);
         }},
        {"DIMALIGNED",
         [](const Ink& i) {
             i.ghost(4, 3, 8, 7);
             i.ghost(17, 12, 21, 16);
             i.newLine(6, 5, 19, 14);
             i.node(6, 5, true);
             i.node(19, 14, true);
         }},
        {"DIMRADIUS",
         [](const Ink& i) {
             i.arc(12, 12, 9, 30, 300);
             i.arrow(12, 12, 19, 6);
             i.node(12, 12);
         }},
        {"DIMDIAMETER",
         [](const Ink& i) {
             i.circle(12, 12, 8);
             i.arrow(6, 17, 18, 7);
         }},
        {"DIMANGULAR",
         [](const Ink& i) {
             i.line(4, 19, 20, 19);
             i.line(4, 19, 18, 6);
             i.arc(4, 19, 9, 0, 43, true);
         }},
        {"DIMORDINATE",
         [](const Ink& i) {
             i.ghost(4, 20, 20, 20);
             i.ghost(4, 20, 4, 4);
             i.newLine(13, 20, 13, 8);
             i.newLine(13, 8, 19, 8);
             i.node(13, 20, true);
         }},

        // ------------------------------------------------------------ blocks
        {"BLOCK",
         [](const Ink& i) {
             i.rect(5, 5, 14, 14, true);
             for (const QPointF corner :
                  {QPointF{5, 5}, QPointF{19, 5}, QPointF{5, 19}, QPointF{19, 19}}) {
                 i.node(corner.x(), corner.y());
             }
         }},
        {"INSERT",
         [](const Ink& i) {
             i.ghost(3, 20, 21, 20);
             i.rect(7, 4, 11, 11, true);
             i.arrow(12, 15, 12, 19);
         }},
        {"REFERENCES",
         [](const Ink& i) {
             i.rect(3, 6, 11, 11);
             i.rect(10, 10, 11, 11, true);
         }},
        {"EXTERNALREFERENCES",
         [](const Ink& i) {
             i.rect(3, 6, 11, 11);
             i.rect(10, 10, 11, 11, true);
         }},

        // -------------------------------------------------------------- view
        {"ZOOMEXTENTS",
         [](const Ink& i) {
             i.rect(3, 5, 18, 14);
             i.painter->setPen(i.accent);
             for (const QPointF c : {QPointF{7, 9}, QPointF{17, 9}, QPointF{7, 15},
                                     QPointF{17, 15}}) {
                 const qreal dx = c.x() < 12 ? -3 : 3;
                 const qreal dy = c.y() < 12 ? -3 : 3;
                 i.painter->drawLine(c, QPointF{c.x() + dx, c.y()});
                 i.painter->drawLine(c, QPointF{c.x(), c.y() + dy});
             }
         }},
        {"ZOOM",
         [](const Ink& i) {
             i.circle(11, 11, 6, true);
             i.line(15, 15, 21, 21);
             i.painter->setPen(i.accent);
             i.painter->drawLine(QPointF{8, 11}, QPointF{14, 11});
             i.painter->drawLine(QPointF{11, 8}, QPointF{11, 14});
         }},
        {"ZOOMCENTER",
         [](const Ink& i) {
             i.circle(12, 12, 7);
             i.node(12, 12, true);
             i.ghost(12, 2, 12, 6);
             i.ghost(12, 18, 12, 22);
             i.ghost(2, 12, 6, 12);
             i.ghost(18, 12, 22, 12);
         }},
        {"PAN",
         [](const Ink& i) {
             // A hand, not a four-way arrow. Four arrows of eight units each
             // carry heads too small to see at ribbon size, and what is left is
             // a plus -- which in a drawing program says crosshair, not drag.
             QPainterPath hand;
             hand.moveTo(7, 12);
             hand.lineTo(7, 17);
             hand.quadTo(7, 21, 11.5, 21);
             hand.lineTo(14, 21);
             hand.quadTo(18, 21, 18, 16.5);
             hand.lineTo(18, 10);
             i.path(hand, true);
             // Three fingers and a thumb, as short strokes rather than shapes.
             i.newLine(10, 12, 10, 6);
             i.newLine(13.5, 11, 13.5, 5);
             i.newLine(17, 12, 17, 7);
             i.painter->setPen(i.accent);
             QPainterPath thumb;
             thumb.moveTo(7, 14);
             thumb.quadTo(4, 15, 4.5, 18);
             i.painter->drawPath(thumb);
         }},
        {"LAYOUT",
         [](const Ink& i) {
             i.rect(3, 4, 18, 16);
             i.ghost(3, 8, 21, 8);
             i.rect(6, 11, 12, 6, true);
         }},
        {"SPLINE",
         [](const Ink& i) {
             QPainterPath s;
             s.moveTo(3, 18);
             s.cubicTo(8, 2, 12, 26, 21, 6);
             i.path(s);
             i.node(3, 18, true);
             i.node(12, 14, true);
             i.node(21, 6, true);
         }},
        {"LIST",
         [](const Ink& i) {
             i.rect(4, 3, 16, 18);
             i.line(7, 8, 17, 8);
             i.line(7, 12, 17, 12);
             i.line(7, 16, 13, 16);
         }},
        {"MEASURE",
         [](const Ink& i) {
             // A curve with points every so far along it.
             QPainterPath s;
             s.moveTo(2, 16);
             s.cubicTo(8, 4, 16, 20, 22, 8);
             i.path(s);
             i.node(7, 10, true);
             i.node(12, 12, true);
             i.node(17, 13, true);
         }},
        {"DIVIDE",
         [](const Ink& i) {
             i.line(2, 12, 22, 12);
             i.node(7, 12, true);
             i.node(12, 12, true);
             i.node(17, 12, true);
             i.ghost(7, 8, 7, 16);
             i.ghost(17, 8, 17, 16);
         }},
        {"STYLE",
         [](const Ink& i) {
             // An A in two weights: the face is the point.
             QPainterPath a;
             a.moveTo(3, 20);
             a.lineTo(9, 4);
             a.lineTo(15, 20);
             a.moveTo(5.5, 14);
             a.lineTo(12.5, 14);
             i.path(a);
             i.newLine(17, 20, 21, 8);
         }},
        {"DIMSTYLE",
         [](const Ink& i) {
             // A dimension with its arrow and text called out.
             i.line(3, 15, 21, 15);
             i.line(3, 11, 3, 19);
             i.line(21, 11, 21, 19);
             i.node(6, 15, true);
             i.node(18, 15, true);
             i.newLine(9, 9, 15, 9);
         }},
        {"PEDIT",
         [](const Ink& i) {
             // A polyline with a vertex being edited.
             i.line(3, 18, 9, 8);
             i.line(9, 8, 15, 16);
             i.arc(18, 12, 4, 200, 160);
             i.node(9, 8, true);
             i.node(15, 16, true);
         }},
        {"LAYISO",
         [](const Ink& i) {
             // Three stacked sheets, the middle one kept.
             i.ghost(4, 6, 20, 6);
             i.rect(4, 10, 16, 5, true);
             i.ghost(4, 19, 20, 19);
         }},
        {"LAYOFF",
         [](const Ink& i) {
             // A sheet with the light switched off: the bulb crossed.
             i.rect(3, 5, 14, 14);
             i.newLine(15, 6, 22, 13);
             i.newLine(22, 6, 15, 13);
         }},
        {"LAYFRZ",
         [](const Ink& i) {
             // A snowflake over the sheet.
             i.rect(3, 6, 12, 12);
             i.newLine(17, 6, 17, 16);
             i.newLine(12, 11, 22, 11);
             i.newLine(13.5, 7.5, 20.5, 14.5);
             i.newLine(20.5, 7.5, 13.5, 14.5);
         }},
        {"DIMCONTINUE",
         [](const Ink& i) {
             // Two dimensions end to end on one line.
             i.line(2, 12, 22, 12);
             i.line(2, 8, 2, 16);
             i.line(12, 8, 12, 16);
             i.line(22, 8, 22, 16);
             i.newLine(12, 12, 22, 12);
         }},
        {"DRAWORDER",
         [](const Ink& i) {
             // Two overlapping sheets, the front one in the accent.
             i.rect(3, 3, 13, 13);
             i.painter->setPen(i.accent);
             i.painter->setBrush(Qt::NoBrush);
             i.painter->drawRect(QRectF{8.5, 8.5, 12, 12});
         }},
        {"PDFATTACH",
         [](const Ink& i) {
             // A page with a folded corner and a few lines of print.
             i.line(5, 3, 15, 3);
             i.line(15, 3, 19, 7);
             i.line(19, 7, 19, 21);
             i.line(19, 21, 5, 21);
             i.line(5, 21, 5, 3);
             i.line(15, 3, 15, 7);
             i.line(15, 7, 19, 7);
             i.newLine(8, 11, 16, 11);
             i.line(8, 14, 16, 14);
             i.line(8, 17, 13, 17);
         }},
        {"IMAGEATTACH",
         [](const Ink& i) {
             // A picture: a frame, a hill, and a sun.
             i.rect(3, 4, 18, 16);
             i.newLine(5, 18, 11, 11);
             i.line(11, 11, 14, 15);
             i.line(14, 15, 16, 13);
             i.line(16, 13, 19, 18);
             i.painter->drawEllipse(QPointF{15.5, 8.5}, 1.8, 1.8);
         }},
        {"XATTACH",
         [](const Ink& i) {
             // A sheet with another sheet clipped onto its corner.
             i.rect(3, 3, 12, 15);
             i.painter->setPen(i.accent);
             i.painter->drawRect(QRectF{9.5, 9.5, 11, 11});
             i.painter->drawLine(QPointF{12, 15}, QPointF{18, 15});
             i.painter->drawLine(QPointF{15, 12}, QPointF{15, 18});
         }},
        {"ATTSYNC",
         [](const Ink& i) {
             // Two tags, the second being brought into line with the first.
             i.rect(3, 4, 9, 6);
             i.rect(12, 14, 9, 6);
             i.newLine(12, 7, 17, 7);
             i.line(17, 7, 17, 13);
             i.line(15, 11, 17, 13);
             i.line(19, 11, 17, 13);
         }},
        {"HATCHEDIT",
         [](const Ink& i) {
             // A hatched box with its strokes being redrawn: two of them in
             // the accent, the third a pencil.
             i.rect(4, 5, 16, 14);
             i.painter->setPen(i.accent);
             i.painter->setClipRect(QRectF{4.5, 5.5, 15, 13});
             i.painter->drawLine(QPointF{-4, 20}, QPointF{10, 4});
             i.painter->drawLine(QPointF{3.5, 20}, QPointF{17.5, 4});
             i.painter->setClipping(false);
             i.newLine(14, 19, 22, 11);
             i.line(20, 9, 22, 11);
         }},
        {"DIMTEDIT",
         [](const Ink& i) {
             // A dimension whose text block has been slid to the right.
             i.line(2, 7, 2, 20);
             i.line(22, 7, 22, 20);
             i.line(2, 10, 22, 10);
             i.painter->setPen(i.accent);
             i.painter->drawRect(QRectF{13.5, 12.5, 7, 5});
         }},
        {"DIMEDIT",
         [](const Ink& i) {
             // A dimension with its text being written over.
             i.line(2, 7, 2, 20);
             i.line(22, 7, 22, 20);
             i.line(2, 10, 22, 10);
             i.newLine(9, 18, 12, 12);
             i.line(12, 12, 15, 18);
             i.line(10, 16, 14, 16);
         }},
        {"DIMBASELINE",
         [](const Ink& i) {
             // Two dimensions from one origin, the second a line further out.
             i.line(2, 6, 2, 20);
             i.line(12, 10, 12, 20);
             i.line(22, 6, 22, 20);
             i.line(2, 16, 12, 16);
             i.newLine(2, 9, 22, 9);
         }},
        {"ATTDEF",
         [](const Ink& i) {
             // A tag: a label with a hole, and the text line it stands for.
             QPainterPath tag;
             tag.moveTo(3, 12);
             tag.lineTo(9, 5);
             tag.lineTo(21, 5);
             tag.lineTo(21, 19);
             tag.lineTo(9, 19);
             tag.closeSubpath();
             i.path(tag);
             i.node(9, 12, true);
             i.line(13, 10, 18, 10);
             i.line(13, 14, 18, 14);
         }},
        {"ATTEDIT",
         [](const Ink& i) {
             i.rect(3, 5, 14, 14);
             i.line(6, 10, 13, 10);
             i.line(6, 14, 11, 14);
             // The pencil.
             i.newLine(14, 20, 21, 13);
             i.newLine(21, 13, 19, 11);
             i.newLine(19, 11, 12, 18);
         }},
        {"LWDISPLAY",
         [](const Ink& i) {
             // Three lines of growing weight.
             i.line(4, 7, 20, 7);
             QPen middle = i.painter->pen();
             middle.setWidthF(2.0);
             i.painter->setPen(middle);
             i.painter->drawLine(QPointF{4, 12}, QPointF{20, 12});
             QPen heavy{i.accent};
             heavy.setWidthF(3.5);
             i.painter->setPen(heavy);
             i.painter->drawLine(QPointF{4, 18}, QPointF{20, 18});
         }},
        {"VPLAYER",
         [](const Ink& i) {
             // A viewport with one of its layers, a stack of three, crossed out.
             i.rect(3, 4, 18, 16);
             i.line(7, 9, 17, 9);
             i.line(7, 12, 17, 12);
             i.newLine(7, 15, 17, 15);
             i.line(15, 13, 19, 17);
             i.line(19, 13, 15, 17);
         }},
        {"MVIEW",
         [](const Ink& i) {
             // A sheet with a new window cut into it, the model showing through.
             i.rect(3, 4, 18, 16);
             i.rect(6, 7, 12, 10, true);
             i.line(8, 15, 11, 10);
             i.line(11, 10, 13, 13);
             i.line(13, 13, 16, 9);
         }},
        {"PAGESETUP",
         [](const Ink& i) {
             // A sheet with its margins marked out.
             i.rect(4, 3, 16, 18);
             i.ghost(7, 6, 17, 6);
             i.ghost(7, 6, 7, 18);
             i.ghost(17, 6, 17, 18);
             i.ghost(7, 18, 17, 18);
             i.node(17, 18, true);
         }},

        // ------------------------------------------------------------- tools
        {"QSELECT",
         [](const Ink& i) {
             i.ghost(3, 4, 15, 4);
             i.ghost(3, 4, 3, 16);
             i.rect(6, 7, 9, 9, true);
             QPainterPath cursor;
             cursor.moveTo(14, 12);
             cursor.lineTo(22, 16);
             cursor.lineTo(18, 17);
             cursor.lineTo(17, 21);
             cursor.closeSubpath();
             i.path(cursor);
         }},
        {"PURGE",
         [](const Ink& i) {
             QPainterPath bin;
             bin.moveTo(6, 8);
             bin.lineTo(7, 20);
             bin.lineTo(17, 20);
             bin.lineTo(18, 8);
             i.path(bin);
             i.line(4, 8, 20, 8);
             i.line(10, 8, 10, 5);
             i.line(14, 8, 14, 5);
             i.painter->setPen(i.accent);
             i.painter->drawLine(QPointF{10, 5}, QPointF{14, 5});
         }},
        // The layer state toggles, which live in their own panel but sit in the
        // same row and would look like an oversight without pictures.
        {"LAYERON",
         [](const Ink& i) {
             QPainterPath eye;
             eye.moveTo(3, 12);
             eye.quadTo(12, 4, 21, 12);
             eye.quadTo(12, 20, 3, 12);
             i.path(eye);
             i.circle(12, 12, 3, true);
         }},
        {"LAYERFREEZE",
         [](const Ink& i) {
             for (int k = 0; k < 3; ++k) {
                 const qreal a = qDegreesToRadians(60.0 * k + 90.0);
                 i.newLine(12 - 9 * std::cos(a), 12 - 9 * std::sin(a), 12 + 9 * std::cos(a),
                           12 + 9 * std::sin(a));
             }
             i.node(12, 12);
         }},
        {"LAYERLOCK",
         [](const Ink& i) {
             i.rect(6, 11, 12, 9, true);
             i.arc(12, 11, 4, 0, 180);
             i.node(12, 15);
         }},
        {"LAYER",
         [](const Ink& i) {
             for (const qreal dy : {6.0, 11.0, 16.0}) {
                 QPainterPath sheet;
                 sheet.moveTo(12, dy - 3);
                 sheet.lineTo(20, dy);
                 sheet.lineTo(12, dy + 3);
                 sheet.lineTo(4, dy);
                 sheet.closeSubpath();
                 i.path(sheet, dy > 15);
             }
         }},
    };
    return table;
}

}  // namespace

QIcon ribbonIcon(const QString& command, const QString& label, int size, const QColor& stroke,
                 const QColor& accent) {
    QPixmap pixmap{QSize{size, size} * 2};
    pixmap.fill(Qt::transparent);

    QPainter painter{&pixmap};
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.scale(pixmap.width() / kGrid, pixmap.height() / kGrid);

    const auto makePen = [](const QColor& colour, qreal width) {
        QPen pen{colour};
        pen.setWidthF(width);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        return pen;
    };
    QColor faded = stroke;
    faded.setAlphaF(0.42F);

    Ink ink{&painter, makePen(stroke, 1.7), makePen(accent, 1.7), makePen(faded, 1.3)};
    painter.setBrush(Qt::NoBrush);

    const auto found = drawings().find(command.toUpper());
    if (found != drawings().end()) {
        found->second(ink);
    } else {
        // A rounded square with the initial in it. Deliberately plain: a
        // command with no picture yet should look like one, not like a picture
        // that failed to say anything.
        painter.setPen(ink.faint);
        painter.drawRoundedRect(QRectF{3.5, 3.5, 17, 17}, 4, 4);
        painter.setPen(ink.solid);
        QFont font = painter.font();
        font.setPixelSize(11);
        font.setBold(true);
        painter.setFont(font);
        const QString initial = label.isEmpty() ? QStringLiteral("?") : label.left(1).toUpper();
        painter.drawText(QRectF{3.5, 3.5, 17, 17}, Qt::AlignCenter, initial);
    }

    painter.end();
    return QIcon{pixmap};
}

}  // namespace cad::app
