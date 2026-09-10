#include "ScenePainter.hpp"

#include "ImageCache.hpp"

#include <QFontDatabase>
#include <QFontMetricsF>
#include <QSet>
#include <QPainter>
#include <QPainterPath>
#include <QScopeGuard>
#include <QStringList>
#include <algorithm>
#include <cmath>

namespace cad::app {
namespace {

constexpr double kPointMarkerPixels = 4.0;

/// Draw one raster image into its parallelogram on screen.
///
/// The image is placed by a transform rather than by scaling a copy, so a
/// rotated or mirrored placement — which the entity represents exactly, since
/// DXF stores two edge vectors and not an angle — comes out right without a
/// separate case for it.
void paintImage(QPainter& painter, const render::SceneImage& image,
                const render::Viewport& viewport, const Theme& theme,
                const ScenePaintOptions& options) {
    const render::ScreenPoint topLeft = viewport.worldToScreen(image.topLeft);
    const render::ScreenPoint topRight = viewport.worldToScreen(image.topLeft + image.across);
    const render::ScreenPoint bottomLeft = viewport.worldToScreen(image.topLeft + image.down);

    const QPointF origin{topLeft.x, topLeft.y};
    const QPointF acrossEdge{topRight.x - topLeft.x, topRight.y - topLeft.y};
    const QPointF downEdge{bottomLeft.x - topLeft.x, bottomLeft.y - topLeft.y};

    const double widthPixels = std::hypot(acrossEdge.x(), acrossEdge.y());
    const double heightPixels = std::hypot(downEdge.x(), downEdge.y());
    if (widthPixels < 1.0 || heightPixels < 1.0) {
        return;  // Smaller than a pixel on screen; there is nothing to show.
    }

    const QImage* decoded =
        options.images == nullptr
            ? nullptr
            : options.images->image(QString::fromStdString(image.path),
                                    static_cast<int>(std::ceil(widthPixels)),
                                    static_cast<int>(std::ceil(heightPixels)),
                                    ImageCache::Adjustment{image.brightness, image.contrast},
                                    image.page);

    if (decoded == nullptr || decoded->isNull()) {
        // A file that resolved but would not decode. The scene had no way to
        // know that — only a decoder does — so the placeholder is drawn here,
        // and it matches the one a missing file gets.
        QPen pen{options.monochrome ? options.monochromeColor : theme.missingReference,
                 1.0 * options.lineWidthScale};
        pen.setStyle(Qt::DashLine);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPolygon(QPolygonF{{origin, origin + acrossEdge,
                                       origin + acrossEdge + downEdge, origin + downEdge}});
        return;
    }

    painter.save();
    const auto restore = qScopeGuard([&painter] { painter.restore(); });

    if (!image.clip.empty()) {
        QPainterPath clipPath;
        clipPath.addPolygon(toPolygon(viewport, image.clip));
        clipPath.closeSubpath();
        if (image.clipInverted) {
            // Keep the outside: the whole viewport, minus the boundary.
            QPainterPath everything;
            everything.addRect(painter.viewport());
            clipPath = everything.subtracted(clipPath);
        }
        painter.setClipPath(clipPath, Qt::IntersectClip);
    }

    // Fade is transparency towards the background, which is what
    // `setOpacity` composites — a faded underlay is meant to read as
    // background a drafter traces over, not as a washed-out picture.
    if (image.fade > 0) {
        painter.setOpacity(std::max(0.0, 1.0 - image.fade / 100.0));
    }

    // Maps the decoded image's own pixel rectangle onto the quad on screen.
    // Built from the decoded size rather than the entity's, because the cache
    // may have decoded at a reduced level and the two must not disagree.
    const double sourceWidth = decoded->width();
    const double sourceHeight = decoded->height();
    if (sourceWidth <= 0.0 || sourceHeight <= 0.0) {
        return;
    }
    const QTransform placement{acrossEdge.x() / sourceWidth, acrossEdge.y() / sourceWidth,
                               downEdge.x() / sourceHeight,  downEdge.y() / sourceHeight,
                               origin.x(),                   origin.y()};
    painter.setTransform(placement, /*combine=*/true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(QPointF{0.0, 0.0}, *decoded);
}

/// Whether a font family is one the system can actually draw with.
///
/// A drawing names fonts freely, and most of them are not on this machine — an
/// other CAD software style routinely names `txt.shx`, which is a *shape file*, not a
/// system font at all. Handing that to Qt does not fail: it silently substitutes
/// something, and the substitute for an unknown name is frequently a symbol
/// face, so a note reading "SITE PLAN" comes out as "SITE PLA—¬".
///
/// Asking first, and keeping the entity's own font when the answer is no, is the
/// difference between text in the wrong typeface and text in the wrong alphabet.
[[nodiscard]] bool fontIsAvailable(const QString& family) {
    if (family.isEmpty() || family.endsWith(".shx", Qt::CaseInsensitive)) {
        return false;
    }
    // The list is fixed for the session and asking is not free, so it is built
    // once — this runs per run, per text item, per frame.
    static const QSet<QString> families = [] {
        QSet<QString> known;
        for (const QString& name : QFontDatabase::families()) {
            known.insert(name.toLower());
        }
        return known;
    }();
    return families.contains(family.toLower());
}

/// Draw a text item that carries inline formatting.
///
/// The unformatted path draws a whole line with one font, which is both simpler
/// and what almost every label needs. This one exists because MText can change
/// weight, size and colour *within* a line, and drawing such a note in one font
/// silently discards the distinctions its author made — a bold heading and its
/// body come out identical.
///
/// Returns false when there is nothing formatted to draw, so the caller can
/// fall back.
bool paintFormattedText(QPainter& painter, const render::DrawItem& item,
                        const std::vector<render::SceneTextRun>& runs, double pixels,
                        const QColor& color, const render::Viewport& viewport,
                        const Theme& theme, const ScenePaintOptions& options) {
    if (runs.empty()) {
        return false;
    }

    // Grouped into lines first, because justification is per line and a line
    // is a run of runs rather than a string.
    std::vector<std::vector<const render::SceneTextRun*>> lines{{}};
    for (const render::SceneTextRun& run : runs) {
        lines.back().push_back(&run);
        if (run.endsLine) {
            lines.emplace_back();
        }
    }
    if (lines.size() > 1 && lines.back().empty()) {
        lines.pop_back();
    }

    const QFont base = painter.font();
    const auto fontFor = [&](const render::SceneTextRun& run) {
        QFont font = base;
        if (const QString family = QString::fromStdString(run.font); fontIsAvailable(family)) {
            font.setFamily(family);
        }
        font.setPixelSize(std::max(1, static_cast<int>(std::lround(pixels * run.heightFactor))));
        font.setBold(run.bold);
        font.setItalic(run.italic);
        font.setUnderline(run.underline);
        font.setOverline(run.overline);
        font.setStrikeOut(run.strikethrough);
        if (run.widthFactor > 0.0 && run.widthFactor != 1.0) {
            font.setStretch(std::clamp(static_cast<int>(std::lround(run.widthFactor * 100.0)),
                                       static_cast<int>(QFont::UltraCondensed),
                                       static_cast<int>(QFont::UltraExpanded)));
        }
        return font;
    };

    // Measured before anything is drawn: the block's height decides where the
    // first baseline goes, and a line's width decides where it starts.
    std::vector<double> widths(lines.size(), 0.0);
    std::vector<double> lineHeights(lines.size(), pixels);
    double maxAscent = 0.0;
    double maxDescent = 0.0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        double tallest = 0.0;
        for (const render::SceneTextRun* run : lines[i]) {
            const QFontMetricsF metrics{fontFor(*run)};
            widths[i] += metrics.horizontalAdvance(QString::fromStdString(run->text));
            tallest = std::max(tallest, metrics.height());
            if (i == 0) {
                maxAscent = std::max(maxAscent, metrics.ascent());
            }
            if (i + 1 == lines.size()) {
                maxDescent = std::max(maxDescent, metrics.descent());
            }
        }
        lineHeights[i] = tallest > 0.0 ? tallest : pixels;
    }

    const double lineStep = item.lineSpacing > 0.0 ? viewport.worldToPixels(item.lineSpacing)
                                                   : lineHeights.front();
    const double blockHeight = static_cast<double>(lines.size() - 1) * lineStep;

    double baseline = 0.0;
    if (item.verticalAlignment == 1) {
        baseline = -maxDescent - blockHeight;
    } else if (item.verticalAlignment == 2) {
        baseline = maxAscent / 2.0 - blockHeight / 2.0;
    } else if (item.verticalAlignment == 3) {
        baseline = maxAscent;
    }

    const auto leftOf = [&](double width) {
        if (item.horizontalAlignment == 1 || item.horizontalAlignment == 4) {
            return -width / 2.0;
        }
        return item.horizontalAlignment == 2 ? -width : 0.0;
    };

    if (item.fromField && options.showFieldBackground) {
        const double widest = *std::max_element(widths.begin(), widths.end());
        if (widest > 0.0) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(theme.fieldBackground);
            painter.drawRect(QRectF{leftOf(widest), baseline - maxAscent, widest,
                                    maxAscent + maxDescent + blockHeight});
            painter.setBrush(Qt::NoBrush);
        }
    }

    for (std::size_t i = 0; i < lines.size(); ++i) {
        double dx = leftOf(widths[i]);
        for (const render::SceneTextRun* run : lines[i]) {
            const QFont font = fontFor(*run);
            painter.setFont(font);
            // A run's own colour, unless the whole plot is being forced to one:
            // monochrome means ink, and a red note printed grey is still a note.
            painter.setPen(run->hasColor && !options.monochrome
                               ? displayColor(run->rgb, 255, theme)
                               : color);
            const QString text = QString::fromStdString(run->text);
            painter.drawText(QPointF{dx, baseline}, text);
            dx += QFontMetricsF{font}.horizontalAdvance(text);
        }
        baseline += lineStep;
    }
    painter.setFont(base);
    return true;
}

}  // namespace

QColor displayColor(std::uint32_t rgb, std::uint8_t alpha, const Theme& theme) {
    QColor color = QColor::fromRgb(static_cast<QRgb>(rgb));
    if (rgb == 0xFFFFFF && theme.background.lightness() > 128) {
        color = theme.foreground;
    } else if (rgb == 0x000000 && theme.background.lightness() <= 128) {
        color = theme.foreground;
    }
    color.setAlpha(alpha);
    return color;
}

QPolygonF toPolygon(const render::Viewport& viewport, const std::vector<Vec3>& points) {
    QPolygonF polygon;
    polygon.reserve(static_cast<int>(points.size()));
    for (const Vec3& point : points) {
        const render::ScreenPoint screen = viewport.worldToScreen(point);
        polygon.append(QPointF{screen.x, screen.y});
    }
    return polygon;
}

void paintScene(QPainter& painter, const render::Scene& scene,
                const render::Viewport& viewport, const Theme& theme,
                const ScenePaintOptions& options) {
    for (const render::DrawItem& item : scene.items) {
        QColor color = displayColor(item.style.rgb, item.style.alpha, theme);
        if (options.monochrome) {
            // Keep the transparency, drop the hue. A drawing authored on a dark
            // background is largely white, and white ink is no ink.
            const int alpha = color.alpha();
            color = options.monochromeColor;
            color.setAlpha(alpha);
        }

        // Viewport contents are clipped to their window. Done here rather than
        // in the scene because a clip region costs the backend almost nothing
        // and cutting the geometry would cost the scene a great deal.
        const bool clippedToBox = item.clip.has_value() && !item.clip->isEmpty();
        const bool clippedToPolygon = item.clipPolygon.size() >= 3;
        const bool clipped = clippedToBox || clippedToPolygon;
        if (clipped) {
            painter.save();
        }
        if (clippedToBox) {
            const render::ScreenPoint low = viewport.worldToScreen(item.clip->min);
            const render::ScreenPoint high = viewport.worldToScreen(item.clip->max);
            painter.setClipRect(QRectF{QPointF{std::min(low.x, high.x), std::min(low.y, high.y)},
                                       QPointF{std::max(low.x, high.x), std::max(low.y, high.y)}});
        }
        if (clippedToPolygon) {
            // A clipped block reference's boundary. Intersected with the
            // window's frame when both apply, which is a clipped background
            // seen through a viewport.
            QPainterPath boundary;
            boundary.addPolygon(toPolygon(viewport, item.clipPolygon));
            boundary.closeSubpath();
            painter.setClipPath(boundary, clippedToBox ? Qt::IntersectClip : Qt::ReplaceClip);
        }
        const auto releaseClip = qScopeGuard([&painter, clipped] {
            if (clipped) {
                painter.restore();
            }
        });

        switch (item.kind) {
            case render::DrawItem::Kind::Geometry: {
                QPen pen{color};
                // Lineweights render as hairlines unless asked for, matching
                // other CAD software: true-width display makes a drawing
                // unreadable when zoomed out. Asked for, a weight is its
                // width in millimetres on the paper, never thinner than the
                // hairline; a plot always asks.
                double width = options.lineWidthScale;
                if (options.pixelsPerMillimetre > 0.0 && item.style.lineweight > 0) {
                    width = std::max(width, item.style.lineweight / 100.0 *
                                                options.pixelsPerMillimetre);
                }
                pen.setWidthF(width);
                pen.setCosmetic(true);
                pen.setJoinStyle(Qt::RoundJoin);
                pen.setCapStyle(Qt::FlatCap);

                if (!item.style.dashPattern.empty()) {
                    QList<qreal> dashes;
                    double total = 0.0;
                    for (const double dash : item.style.dashPattern) {
                        const double pixels = viewport.worldToPixels(std::abs(dash));
                        dashes.append(std::max(0.1, pixels));
                        total += pixels;
                    }
                    // A pattern finer than a few pixels reads as a solid grey
                    // line and costs more to rasterize than a solid one.
                    if (total >= 3.0 && dashes.size() % 2 == 0) {
                        pen.setDashPattern(dashes);
                    }
                }
                painter.setPen(pen);

                for (const render::Polyline2D& run : item.geometry) {
                    const QPolygonF polygon = toPolygon(viewport, run.points);
                    if (run.closed) {
                        painter.drawPolygon(polygon);
                    } else {
                        painter.drawPolyline(polygon);
                    }
                }
                break;
            }

            case render::DrawItem::Kind::Filled: {
                // Even-odd, so an island ring inside an outer ring cuts a hole
                // rather than filling twice. Winding would fill the island.
                QPainterPath path;
                path.setFillRule(item.unionFill ? Qt::WindingFill : Qt::OddEvenFill);
                for (const render::Polyline2D& run : item.geometry) {
                    if (run.points.size() >= 3) {
                        path.addPolygon(toPolygon(viewport, run.points));
                        path.closeSubpath();
                    }
                }
                if (!path.isEmpty()) {
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(color);
                    painter.drawPath(path);
                    painter.setBrush(Qt::NoBrush);
                }
                break;
            }

            case render::DrawItem::Kind::Text: {
                const double pixels = viewport.worldToPixels(item.textHeight);

                // Below a few pixels the glyphs are an unreadable smudge, so a
                // bar is drawn in their place — other CAD software's QTEXT, and what page
                // layout tools call greeking.
                //
                // Drawing *nothing* was worse than a smudge. Zoomed out far
                // enough to see a whole floor plan, every room name and every
                // dimension simply left the drawing, and the annotation a
                // drafter is looking for at that zoom is exactly what vanished.
                // A bar keeps the placement and density of the text visible at
                // a fraction of the cost of rendering it.
                const bool greeked = pixels < options.minimumTextPixels;

                const render::ScreenPoint anchor = viewport.worldToScreen(item.position);
                painter.save();
                painter.translate(anchor.x, anchor.y);
                // Negated: screen Y is inverted relative to world Y.
                painter.rotate(-item.textRotation * 180.0 / kPi);

                QFont font = painter.font();
                font.setPixelSize(std::max(1, static_cast<int>(std::lround(pixels))));
                painter.setFont(font);
                painter.setPen(color);

                // Formatted text takes its own path — but not when greeked: at
                // three pixels a bold run and a plain one are the same bar, and
                // building a font per run to draw it would be pure cost.
                if (!greeked && item.reference != render::DrawItem::kNoReference &&
                    item.reference < scene.textRuns.size() &&
                    paintFormattedText(painter, item, scene.textRuns[item.reference], pixels,
                                       color, viewport, theme, options)) {
                    painter.restore();
                    break;
                }

                // Justification is applied here rather than in the scene
                // because it needs font metrics, which the core cannot reach.
                const QFontMetricsF metrics{font};
                const QStringList lines = QString::fromStdString(item.text).split(QChar{'\n'});

                // Greeked text cannot ask the font anything useful — a font at
                // one pixel reports nonsense — so its metrics are estimated
                // from the height, using the same aspect the core's own text
                // measurement assumes.
                constexpr double kAspect = 0.6;
                const auto advance = [&](const QString& line) {
                    return greeked ? static_cast<double>(line.size()) * pixels * kAspect
                                   : metrics.horizontalAdvance(line);
                };
                const double ascent = greeked ? pixels * 0.8 : metrics.ascent();
                const double descent = greeked ? pixels * 0.2 : metrics.descent();
                const double capHeight = greeked ? pixels * 0.7 : metrics.capHeight();

                const double lineHeight = item.lineSpacing > 0.0
                                              ? viewport.worldToPixels(item.lineSpacing)
                                              : (greeked ? pixels * 1.2 : metrics.height());
                const double blockHeight =
                    static_cast<double>(lines.size() - 1) * lineHeight;

                // Screen Y runs downward, so a larger offset moves the baseline
                // *down*. The anchor names an edge of the whole block, not of
                // the first line: a two-line label attached at its middle
                // straddles the anchor rather than hanging below it.
                double baseline = 0.0;
                if (item.verticalAlignment == 1) {  // bottom of the last line
                    baseline = -descent - blockHeight;
                } else if (item.verticalAlignment == 2) {  // middle of the block
                    baseline = capHeight / 2.0 - blockHeight / 2.0;
                } else if (item.verticalAlignment == 3) {  // top of the first
                    baseline = ascent;
                }

                // Field text is shaded, as other CAD software shades it. The shading is
                // the only thing that says a number is *derived* — that it will
                // move when the drawing does, and that typing over it throws
                // that away. A drafter deciding whether to trust an area on a
                // sheet is asking exactly this question.
                if (item.fromField && options.showFieldBackground) {
                    double widest = 0.0;
                    for (const QString& line : lines) {
                        widest = std::max(widest, advance(line));
                    }
                    if (widest > 0.0) {
                        double left = 0.0;
                        if (item.horizontalAlignment == 1 || item.horizontalAlignment == 4) {
                            left = -widest / 2.0;
                        } else if (item.horizontalAlignment == 2) {
                            left = -widest;
                        }
                        const double top = baseline - ascent;
                        const double height =
                            ascent + descent +
                            static_cast<double>(lines.size() - 1) * lineHeight;
                        painter.setPen(Qt::NoPen);
                        painter.setBrush(theme.fieldBackground);
                        painter.drawRect(QRectF{left, top, widest, height});
                        painter.setBrush(Qt::NoBrush);
                        painter.setPen(color);
                    }
                }

                if (greeked) {
                    // Softer than the glyphs would be: a bar at full strength
                    // reads as a solid object, which is not what is there.
                    QColor bar = color;
                    bar.setAlpha(static_cast<int>(color.alpha() * 0.55));
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(bar);
                }

                for (const QString& line : lines) {
                    // Each line is justified on its own width, as other CAD software
                    // justifies within the column rather than by the longest.
                    const double width = advance(line);
                    double dx = 0.0;
                    if (item.horizontalAlignment == 1 || item.horizontalAlignment == 4) {
                        dx = -width / 2.0;
                    } else if (item.horizontalAlignment == 2) {
                        dx = -width;
                    }
                    if (greeked) {
                        if (!line.isEmpty()) {
                            // At least a pixel tall, or the bar rounds away to
                            // nothing and the text has disappeared again.
                            painter.drawRect(QRectF{dx, baseline - capHeight, width,
                                                    std::max(1.0, capHeight)});
                        }
                    } else {
                        painter.drawText(QPointF{dx, baseline}, line);
                    }
                    baseline += lineHeight;
                }
                if (greeked) {
                    painter.setBrush(Qt::NoBrush);
                }
                painter.restore();
                break;
            }

            case render::DrawItem::Kind::Marker: {
                const render::ScreenPoint p = viewport.worldToScreen(item.position);
                const double size = kPointMarkerPixels * options.lineWidthScale;
                painter.setPen(QPen{color, options.lineWidthScale});
                painter.drawLine(QPointF{p.x - size, p.y}, QPointF{p.x + size, p.y});
                painter.drawLine(QPointF{p.x, p.y - size}, QPointF{p.x, p.y + size});
                break;
            }

            case render::DrawItem::Kind::Image: {
                if (item.reference < scene.images.size()) {
                    paintImage(painter, scene.images[item.reference], viewport, theme, options);
                }
                break;
            }

            case render::DrawItem::Kind::MissingReference: {
                // A missing reference rendering as nothing leaves the user
                // unable to tell that part of the drawing is absent.
                const render::ScreenPoint p = viewport.worldToScreen(item.position);
                QPen pen{options.monochrome ? color : theme.missingReference,
                         1.5 * options.lineWidthScale};
                pen.setStyle(Qt::DashLine);
                painter.setPen(pen);
                const double size = 12.0 * options.lineWidthScale;
                painter.drawRect(QRectF{p.x - size, p.y - size, size * 2, size * 2});
                painter.drawLine(QPointF{p.x - size, p.y - size},
                                 QPointF{p.x + size, p.y + size});
                painter.drawLine(QPointF{p.x + size, p.y - size},
                                 QPointF{p.x - size, p.y + size});
                break;
            }
        }
    }
}

}  // namespace cad::app
