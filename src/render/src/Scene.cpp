#include "cad/render/Scene.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <unordered_map>

#include "cad/db/CurveEntities.hpp"
#include "cad/db/DimensionEntity.hpp"
#include "cad/db/HatchEntity.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/SolidEntity.hpp"
#include "cad/db/Linetype.hpp"
#include "cad/db/FieldObject.hpp"
#include "cad/db/OleFrameEntity.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/db/UnderlayEntity.hpp"

namespace cad::render {
namespace {

[[nodiscard]] double nowMilliseconds() {
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>{Clock::now().time_since_epoch()}.count();
}

/// A closed run from a ring of points, transformed into world space.
/// The outline of one widened polyline segment, as a closed ring.
///
/// A polyline with a width is a filled shape, not a thick line. other CAD software draws
/// it that way and the width is real drawing units -- it scales with the
/// drawing, unlike lineweight, which is a plotting property measured in
/// millimetres on the paper. Rendering a wide polyline as a hairline is not a
/// missing feature so much as a wrong drawing: revision clouds, poché and
/// tapered arrows all arrive this way and all come out as thread.
///
/// The two widths are interpolated along the segment, so a taper is a taper.
/// Each segment becomes its own ring rather than one offset path for the whole
/// polyline: a shared outline needs mitred joins, and a mitre that fails at a
/// sharp corner produces a spike across the drawing. Overlapping rings meet
/// bluntly at a corner, which is what other CAD software's own default does.
/// Where a widened segment's outline begins and ends, for filling the joins.
struct WidenedEnds {
    Vec3 startLeft;
    Vec3 startRight;
    Vec3 endLeft;
    Vec3 endRight;
    bool valid = false;
};

[[nodiscard]] std::vector<Vec3> widenedSegment(const std::vector<Vec3>& centre,
                                               double startWidth, double endWidth,
                                               WidenedEnds* ends = nullptr) {
    std::vector<Vec3> ring;
    if (centre.size() < 2) {
        return ring;
    }

    // Distance along, so the width can be interpolated by position rather than
    // by index: a tessellated arc has uneven steps and interpolating by index
    // would taper in lurches.
    std::vector<double> travelled(centre.size(), 0.0);
    for (std::size_t i = 1; i < centre.size(); ++i) {
        travelled[i] = travelled[i - 1] + centre[i - 1].distanceTo(centre[i]);
    }
    const double total = travelled.back();
    if (total <= 0.0) {
        return ring;
    }

    std::vector<Vec3> left;
    std::vector<Vec3> right;
    left.reserve(centre.size());
    right.reserve(centre.size());

    for (std::size_t i = 0; i < centre.size(); ++i) {
        // The direction the path is going at this point: the average of the
        // segments either side, so an interior point sits on the bisector and
        // the two offsets meet instead of stepping.
        Vec3 direction;
        if (i == 0) {
            direction = centre[1] - centre[0];
        } else if (i + 1 == centre.size()) {
            direction = centre[i] - centre[i - 1];
        } else {
            const Vec3 before = (centre[i] - centre[i - 1]).normalized();
            const Vec3 after = (centre[i + 1] - centre[i]).normalized();
            direction = before + after;
            if (direction.length2() < 1e-12) {
                direction = after;
            }
        }
        const double length = direction.length2();
        if (length < 1e-12) {
            continue;
        }
        const Vec3 normal{-direction.y / length, direction.x / length, 0.0};
        const double fraction = travelled[i] / total;
        const double half = (startWidth + (endWidth - startWidth) * fraction) * 0.5;
        left.push_back(centre[i] + normal * half);
        right.push_back(centre[i] - normal * half);
    }
    if (left.size() < 2) {
        return ring;
    }

    if (ends != nullptr) {
        ends->startLeft = left.front();
        ends->startRight = right.front();
        ends->endLeft = left.back();
        ends->endRight = right.back();
        ends->valid = true;
    }

    ring.reserve(left.size() * 2);
    ring.insert(ring.end(), left.begin(), left.end());
    ring.insert(ring.end(), right.rbegin(), right.rend());
    return ring;
}

/// True when any part of the polyline is drawn with a width.
[[nodiscard]] bool hasWidth(const PolylineEntity& polyline) {
    if (polyline.constantWidth > 0.0) {
        return true;
    }
    return std::any_of(polyline.vertices.begin(), polyline.vertices.end(),
                       [](const PolylineVertex& vertex) {
                           return vertex.startWidth > 0.0 || vertex.endWidth > 0.0;
                       });
}

[[nodiscard]] Polyline2D closedRun(const std::vector<Vec3>& ring, const Mat4& transform) {
    Polyline2D run;
    run.closed = true;
    run.points.reserve(ring.size());
    for (const Vec3& point : ring) {
        run.points.push_back(transform.isIdentity() ? point : transform.transformPoint(point));
    }
    return run;
}

/// An open two-point run, transformed into world space.
[[nodiscard]] Polyline2D segmentRun(const LineSegment& segment, const Mat4& transform) {
    Polyline2D run;
    run.points = {transform.isIdentity() ? segment.start : transform.transformPoint(segment.start),
                  transform.isIdentity() ? segment.end : transform.transformPoint(segment.end)};
    return run;
}

}  // namespace

void SceneBuilder::reindex(const BlockTableRecord& space) {
    std::vector<SpatialIndex::Item> items;

    for (Entity* entity : db_.entitiesIn(space)) {
        // A block reference cannot know its own extents; the database resolves
        // them by traversing the definition.
        if (auto* insert = dynamic_cast<InsertEntity*>(entity)) {
            if (auto* definition = db_.get<BlockTableRecord>(insert->blockHandle)) {
                insert->setBlockExtents(db_.blockExtents(*definition));
            }
        }
        const Box3 bounds = entity->bounds();
        if (!bounds.isEmpty()) {
            items.push_back({entity->handle, bounds});
        }
    }
    index_.rebuild(items);
}

const ResolutionContext& SceneBuilder::contextFor(const Entity& entity) {
    // Cached per layer for the duration of one build. Resolving it per entity
    // means a hash lookup for the layer and another for its linetype on every
    // entity of every frame — measurable at a hundred thousand entities, and
    // pure repetition, since the answer depends only on the layer.
    const auto existing = contextCache_.find(entity.layerHandle);
    if (existing != contextCache_.end()) {
        return existing->second;
    }
    return contextCache_.emplace(entity.layerHandle, db_.resolutionContextFor(entity))
        .first->second;
}

DrawStyle SceneBuilder::resolveStyle(const ResolvedProperties& resolved) const {
    DrawStyle style;
    style.rgb = resolved.rgb;
    style.lineweight = resolved.lineweight;
    style.alpha = resolved.alpha;

    if (const auto* linetype = db_.get<Linetype>(resolved.linetypeHandle)) {
        if (!linetype->isContinuous()) {
            style.dashPattern = linetype->toDashArray(resolved.linetypeScale);
        }
    }
    return style;
}

void SceneBuilder::appendLeader(const LeaderEntity& leader, const Mat4& transform,
                                const DrawStyle& style, Scene& scene) {
    const LeaderGeometry geometry = leader.geometry(db_.dimStyleByHandle(leader.styleHandle));

    DrawItem strokes;
    strokes.kind = DrawItem::Kind::Geometry;
    strokes.handle = leader.handle;
    strokes.style = style;
    // Never dashed, for the reason a dimension line is not: a dashed leader
    // reads as drawing geometry, and annotation must not be mistaken for the
    // thing it annotates.
    strokes.style.dashPattern.clear();
    for (const LineSegment& line : geometry.lines) {
        strokes.geometry.push_back(segmentRun(line, transform));
    }
    if (!strokes.geometry.empty()) {
        scene.items.push_back(std::move(strokes));
    }

    if (!geometry.arrowheads.empty()) {
        DrawItem arrows;
        arrows.kind = DrawItem::Kind::Filled;
        arrows.handle = leader.handle;
        arrows.style = style;
        arrows.style.dashPattern.clear();
        for (const std::array<Vec3, 3>& triangle : geometry.arrowheads) {
            arrows.geometry.push_back(
                closedRun({triangle[0], triangle[1], triangle[2]}, transform));
        }
        scene.items.push_back(std::move(arrows));
    }
}

void SceneBuilder::appendMLeader(const MLeaderEntity& leader, const Mat4& transform,
                                 const DrawStyle& style, Scene& scene) {
    DrawItem strokes;
    strokes.kind = DrawItem::Kind::Geometry;
    strokes.handle = leader.handle;
    strokes.style = style;
    strokes.style.dashPattern.clear();
    for (const auto& [from, to] : leader.segments()) {
        strokes.geometry.push_back(segmentRun(LineSegment{from, to}, transform));
    }
    if (!strokes.geometry.empty()) {
        scene.items.push_back(std::move(strokes));
    }

    const std::vector<std::array<Vec3, 3>> heads = leader.arrowheads();
    if (!heads.empty()) {
        DrawItem arrows;
        arrows.kind = DrawItem::Kind::Filled;
        arrows.handle = leader.handle;
        arrows.style = style;
        arrows.style.dashPattern.clear();
        for (const std::array<Vec3, 3>& triangle : heads) {
            arrows.geometry.push_back(
                closedRun({triangle[0], triangle[1], triangle[2]}, transform));
        }
        scene.items.push_back(std::move(arrows));
    }

    if (!leader.text.empty()) {
        DrawItem text;
        text.kind = DrawItem::Kind::Text;
        text.handle = leader.handle;
        text.style = style;
        text.position = transform.transformPoint(leader.textLocation);
        text.text = decodeTextSymbols(leader.text);
        text.textHeight = leader.textHeight * transform.scaleFactor2();
        text.textRotation = leader.textRotation + transform.rotation2();
        // The context data's attachment is the nine-way MTEXT one, split into
        // two axes the same way an MTEXT's is.
        const int attachment = leader.attachment < 1 || leader.attachment > 9
                                   ? 1
                                   : leader.attachment;
        text.horizontalAlignment = (attachment - 1) % 3;
        text.verticalAlignment = 3 - (attachment - 1) / 3;
        scene.items.push_back(std::move(text));
    }
}

void SceneBuilder::appendDimension(const DimensionEntity& dimension, const Mat4& transform,
                                   const DrawStyle& style, const Viewport& viewport,
                                   Scene& scene) {
    // The *effective* style: the named one plus whatever this dimension
    // overrides for itself. Nudging one dimension's arrow size is routine, and
    // drawing it at the style's size instead is visibly wrong.
    const DimensionGeometry geometry = dimension.generate(db_.effectiveDimStyle(dimension));

    TessellationOptions tessellation;
    tessellation.chordTolerance = chordToleranceFor(viewport);
    tessellation.minSegments = 4;

    DrawItem strokes;
    strokes.kind = DrawItem::Kind::Geometry;
    strokes.handle = dimension.handle;
    strokes.style = style;
    // Dimension lines are never dashed, whatever the entity's linetype says.
    // A dashed extension line reads as drawing geometry, which is exactly the
    // confusion annotation must not create.
    strokes.style.dashPattern.clear();

    for (const LineSegment& line : geometry.lines) {
        strokes.geometry.push_back(segmentRun(line, transform));
    }
    for (const Arc& arc : geometry.arcs) {
        Polyline2D run;
        const CurvePtr world = transform.isIdentity() ? nullptr : arc.transformed(transform);
        run.points = (world == nullptr ? static_cast<const Curve&>(arc) : *world)
                         .tessellate(tessellation);
        if (run.points.size() >= 2) {
            strokes.geometry.push_back(std::move(run));
        }
    }
    if (!strokes.geometry.empty()) {
        scene.items.push_back(std::move(strokes));
    }

    if (!geometry.arrowheads.empty()) {
        DrawItem arrows;
        arrows.kind = DrawItem::Kind::Filled;
        arrows.handle = dimension.handle;
        arrows.style = style;
        arrows.style.dashPattern.clear();
        for (const std::array<Vec3, 3>& triangle : geometry.arrowheads) {
            arrows.geometry.push_back(
                closedRun({triangle[0], triangle[1], triangle[2]}, transform));
        }
        scene.items.push_back(std::move(arrows));
    }

    if (!geometry.text.empty()) {
        DrawItem text;
        text.kind = DrawItem::Kind::Text;
        text.handle = dimension.handle;
        text.style = style;
        text.position = transform.transformPoint(geometry.textPosition);
        text.text = decodeTextSymbols(geometry.text);
        text.textHeight = geometry.textHeight * transform.scaleFactor2();
        text.textRotation = geometry.textRotation + transform.rotation2();
        // Dimension text hangs from its middle, not from a baseline corner.
        text.horizontalAlignment = static_cast<int>(TextHorizontalAlignment::Center);
        text.verticalAlignment = static_cast<int>(TextVerticalAlignment::Middle);
        scene.items.push_back(std::move(text));
    }
}

void SceneBuilder::appendWidePolyline(const PolylineEntity& polyline, const Mat4& transform,
                                      const DrawItem& prototype, const Viewport& viewport,
                                      Scene& scene) {
    TessellationOptions tessellation;
    tessellation.chordTolerance = chordToleranceFor(viewport);
    tessellation.minSegments = 2;
    tessellation.maxSegments = 4096;

    // One item holding every ring, filled by winding rather than even-odd.
    //
    // Consecutive segments overlap on the inside of a bend. Under even-odd each
    // overlap becomes a hole exactly where the corner should be solid, and
    // splitting them into separate items instead leaves hairline seams where
    // the antialiased edges abut. Winding unions them, which is what a widened
    // polyline is.
    DrawItem item = prototype;
    item.kind = DrawItem::Kind::Filled;
    item.unionFill = true;

    // Winding only unions rings that turn the same way, so every ring added
    // here is normalised to one direction. A wedge that happened to be wound
    // the other way would cancel the segment underneath it instead.
    const auto emit = [&](Polyline2D ring) {
        double twiceArea = 0.0;
        for (std::size_t k = 0; k < ring.points.size(); ++k) {
            const Vec3& a = ring.points[k];
            const Vec3& b = ring.points[(k + 1) % ring.points.size()];
            twiceArea += a.x * b.y - b.x * a.y;
        }
        if (twiceArea > 0.0) {
            std::reverse(ring.points.begin(), ring.points.end());
        }
        item.geometry.push_back(std::move(ring));
    };
    WidenedEnds previous;

    // The polyline's own segments, not `toCurves`, which hands back the whole
    // path as a single curve. Widths are per segment, so one curve meant one
    // width: a taper came out uniformly wide along its entire length.
    const Polyline geometry = polyline.geometry();
    const std::vector<CurvePtr>& curves = geometry.segments();
    const std::size_t segments = polyline.segmentCount();
    for (std::size_t i = 0; i < segments && i < curves.size(); ++i) {
        const CurvePtr& curve = curves[i];
        if (curve == nullptr) {
            continue;
        }
        const PolylineVertex& from = polyline.vertices[i];
        // A per-vertex width of zero means "use the constant one", which is
        // how DXF stores a uniformly wide polyline: the widths are simply
        // absent and code 43 carries it for the whole thing.
        const double startWidth = from.startWidth > 0.0 ? from.startWidth
                                                        : polyline.constantWidth;
        const double endWidth = from.endWidth > 0.0 ? from.endWidth : polyline.constantWidth;
        if (startWidth <= 0.0 && endWidth <= 0.0) {
            // A segment with no width inside a polyline that has some: drawn as
            // a line, because filling it would produce nothing and dropping it
            // would leave a gap.
            DrawItem thin = prototype;
            thin.kind = DrawItem::Kind::Geometry;
            previous.valid = false;
            Polyline2D run;
            run.points = curve->tessellate(tessellation);
            for (Vec3& point : run.points) {
                point = transform.transformPoint(point);
            }
            if (run.points.size() >= 2) {
                thin.geometry.push_back(std::move(run));
                scene.items.push_back(std::move(thin));
            }
            continue;
        }

        std::vector<Vec3> centre = curve->tessellate(tessellation);
        for (Vec3& point : centre) {
            point = transform.transformPoint(point);
        }
        // Scaled with the drawing, since the width is in drawing units and the
        // transform may be a block reference's.
        const double scale = transform.scaleFactor2();
        WidenedEnds ends;
        std::vector<Vec3> ring =
            widenedSegment(centre, startWidth * scale, endWidth * scale, &ends);
        if (ring.size() < 3) {
            previous.valid = false;
            continue;
        }

        // Fill the join. Each segment is offset from its own direction, so at a
        // bend the two outlines diverge and leave a notch on the outside of the
        // corner -- small, but plainly visible on a wide polyline and not
        // something other CAD software shows. A triangle down each side through the shared
        // vertex covers it. Two triangles rather than one quad across the join:
        // at a sharp bend the quad crosses itself, and an even-odd fill then
        // leaves a hole where the notch was.
        if (previous.valid) {
            const Vec3 corner = transform.transformPoint(polyline.vertices[i].position);
            for (const auto& [before, after] : {std::pair{previous.endLeft, ends.startLeft},
                                                std::pair{previous.endRight, ends.startRight}}) {
                if (before.equals(after)) {
                    continue;
                }
                Polyline2D wedge;
                wedge.points = {corner, before, after};
                wedge.closed = true;
                emit(std::move(wedge));
            }
        }
        previous = ends;

        Polyline2D run;
        run.points = std::move(ring);
        run.closed = true;
        emit(std::move(run));
    }

    if (!item.geometry.empty()) {
        scene.items.push_back(std::move(item));
    }
}

void SceneBuilder::appendHatch(const HatchEntity& hatch, const Mat4& transform,
                               const DrawStyle& style, const Viewport& viewport,
                               const SceneOptions& options, Scene& scene) {
    TessellationOptions tessellation;
    tessellation.chordTolerance = chordToleranceFor(viewport);
    tessellation.minSegments = 4;

    if (hatch.isSolidFill()) {
        DrawItem item;
        item.kind = DrawItem::Kind::Filled;
        item.handle = hatch.handle;
        item.style = style;
        if (hatch.gradient.enabled) {
            // Drawn flat in the first colour until gradients are implemented.
            // Visibly approximate beats invisibly absent.
            item.style.rgb = hatch.gradient.color1.toRgb();
        }
        for (const std::vector<Vec3>& ring : hatch.fillPolygons(tessellation)) {
            item.geometry.push_back(closedRun(ring, transform));
        }
        if (!item.geometry.empty()) {
            scene.items.push_back(std::move(item));
        }
        return;
    }

    // Only what the view can see is generated. A hatch covering a whole site
    // plan costs the same as one covering a doorway when both are zoomed to
    // the same window.
    Box3 clip = viewport.visibleBounds();
    if (!transform.isIdentity()) {
        // The hatch generates in its own space, so the clip travels the other
        // way rather than the generated lines being made and thrown away.
        if (const auto inverse = transform.inverted()) {
            clip = clip.transformed(*inverse);
        }
    }

    DrawItem item;
    item.kind = DrawItem::Kind::Geometry;
    item.handle = hatch.handle;
    item.style = style;
    for (const LineSegment& segment : hatch.generateLines(clip, options.maxHatchSegments)) {
        item.geometry.push_back(segmentRun(segment, transform));
    }
    if (!item.geometry.empty()) {
        scene.items.push_back(std::move(item));
    }
}

void SceneBuilder::appendTable(const TableEntity& table, const Mat4& transform,
                               const DrawStyle& style, const Viewport& viewport, Scene& scene) {
    // The grid first, from the row heights and column widths, then a text run
    // per cell. Neither is read from the anonymous block the file also carries:
    // see TableEntity for why a table is generated rather than replayed.
    TessellationOptions tessellation;
    tessellation.chordTolerance = chordToleranceFor(viewport);

    DrawItem grid;
    grid.kind = DrawItem::Kind::Geometry;
    grid.handle = table.handle;
    grid.style = style;
    // A table's rules are never dashed, whatever linetype the entity carries: a
    // dashed rule reads as drawing geometry, which is the confusion annotation
    // must not create.
    grid.style.dashPattern.clear();
    for (const CurvePtr& curve : table.toCurves()) {
        const CurvePtr world = transform.isIdentity() ? nullptr : curve->transformed(transform);
        Polyline2D run;
        run.points = (world == nullptr ? *curve : *world).tessellate(tessellation);
        run.closed = curve->isClosed();
        if (run.points.size() >= 2) {
            grid.geometry.push_back(std::move(run));
        }
    }
    if (!grid.geometry.empty()) {
        scene.items.push_back(std::move(grid));
    }

    for (std::size_t row = 0; row < table.rowCount(); ++row) {
        for (std::size_t column = 0; column < table.columnCount(); ++column) {
            const TableCell* cell = table.cellAt(row, column);
            if (cell == nullptr) {
                continue;
            }

            // A schedule is mostly derived, and the derived value is composed
            // rather than stored — `refreshTableCells` works it out after the
            // drawing is read. Reading the field's own cache instead gives an
            // empty grid: the cell's field is a template over a child, and only
            // the child caches anything.
            // A cell's value carries MText markup — a colour override on a
            // computed area, a paragraph break in a long room name — because a
            // cell *is* an MText. Drawn as written it reads `{\C256;423.08}`,
            // which is the markup showing through rather than doing its job.
            MTextEntity scratch;
            scratch.contents = cell->displayText();
            const std::string text = scratch.plainText();
            if (text.empty()) {
                continue;
            }

            const Box3 box = table.cellBounds(row, column);
            if (box.isEmpty()) {
                continue;
            }

            DrawItem item;
            item.kind = DrawItem::Kind::Text;
            item.handle = table.handle;
            item.style = style;
            item.text = text;
            item.textHeight = (cell->textHeight > 0.0 ? cell->textHeight : box.height() * 0.6) *
                              transform.scaleFactor2();
            item.position = transform.transformPoint(box.min.midpoint(box.max));
            item.lineSpacing = item.textHeight;
            item.horizontalAlignment = static_cast<int>(TextHorizontalAlignment::Center);
            item.verticalAlignment = static_cast<int>(TextVerticalAlignment::Middle);
            // Shaded like any other derived text, so a drafter can see at a
            // glance which numbers in a schedule will move on their own.
            item.fromField = !cell->fieldHandle.isNull();
            scene.items.push_back(std::move(item));
        }
    }
}

void SceneBuilder::appendImage(const RasterImageEntity& image, const Mat4& transform,
                               const DrawStyle& style, const Viewport& viewport,
                               const SceneOptions& options, Scene& scene) {
    const auto* definition = db_.get<ImageDefinition>(image.definitionHandle);
    const std::string storedPath = definition == nullptr ? std::string{} : definition->fileName;
    const std::string resolvedPath =
        definition == nullptr ? std::string{} : definition->resolvedPath;

    const std::vector<Vec3> frame = image.corners();
    const bool drawable = image.showImage && !resolvedPath.empty();

    if (drawable) {
        SceneImage payload;
        payload.path = resolvedPath;
        payload.storedPath = storedPath;
        // The entity's origin is the raster's *bottom* left and its v vector
        // points up, so the top-left corner is the origin plus a full height of
        // v. Every decoder hands back row 0 at the top; doing the flip here
        // means no backend has to know that.
        payload.topLeft = transform.transformPoint(frame[3]);
        payload.across = transform.transformVector(image.uVector * image.widthPixels);
        payload.down = transform.transformVector(image.vVector * -image.heightPixels);
        for (const Vec3& point : image.worldClipBoundary()) {
            payload.clip.push_back(transform.transformPoint(point));
        }
        payload.clipInverted = image.clipInverted;
        payload.brightness = image.brightness;
        payload.contrast = image.contrast;
        payload.fade = image.fade;

        DrawItem item;
        item.kind = DrawItem::Kind::Image;
        item.handle = image.handle;
        item.style = style;
        item.reference = scene.images.size();
        scene.images.push_back(std::move(payload));
        scene.items.push_back(std::move(item));
    }

    if (drawable) {
        // The frame is an editing aid once the picture is there, so it follows
        // IMAGEFRAME and is off by default.
        if (options.showImageFrames) {
            DrawItem outline;
            outline.kind = DrawItem::Kind::Geometry;
            outline.handle = image.handle;
            outline.style = style;
            outline.geometry.push_back(closedRun(image.displayOutline(), transform));
            scene.items.push_back(std::move(outline));
        }
        return;
    }

    // No picture: the same treatment an underlay gets, and for the same reason.
    // The caption names the file, because the file name is the only part of
    // this a user can do anything about.
    std::string caption = "Image";
    if (!storedPath.empty()) {
        const std::size_t slash = storedPath.find_last_of("/\\");
        caption += ": " + (slash == std::string::npos ? storedPath
                                                      : storedPath.substr(slash + 1));
    } else if (!image.showImage) {
        caption = "Image (hidden)";
    }
    appendPlaceholderFrame(image, image.displayOutline(), caption, transform, style, viewport,
                           options, scene);
}

void SceneBuilder::appendPlaceholderFrame(const Entity& entity, const std::vector<Vec3>& outline,
                                          const std::string& caption, const Mat4& transform,
                                          const DrawStyle& style, const Viewport& viewport,
                                          const SceneOptions& options, Scene& scene) {
    if (!options.showContentPlaceholders) {
        ++scene.placeholdersOmitted;
        return;
    }
    if (outline.size() < 3) {
        return;
    }

    DrawItem frame;
    frame.kind = DrawItem::Kind::Geometry;
    frame.handle = entity.handle;
    frame.style = style;
    const Box3 extents = entity.bounds();
    const double span = extents.diagonal();
    const double dash = span > 0.0 ? span / 40.0 : 1.0;
    frame.style.dashPattern = {dash, dash};
    frame.geometry.push_back(closedRun(outline, transform));
    scene.items.push_back(std::move(frame));

    if (caption.empty()) {
        return;
    }

    // Subordinate to the frame, deliberately.
    //
    // The caption exists to answer "what is this empty box?", and a caption
    // sized to fill the box answers it by shouting. A third of the width and a
    // tenth of the height leaves it plainly a label on something rather than
    // the something. Below seven pixels it is dropped entirely: a caption that
    // has to be zoomed into is worse than none, because it is clutter that
    // looks like drawing.
    const double width = extents.max.x - extents.min.x;
    const double height = extents.max.y - extents.min.y;
    constexpr double kGlyphAspect = 0.6;
    const double byWidth = width / (static_cast<double>(caption.size()) * kGlyphAspect * 3.0);
    const double textHeight = std::min(byWidth, height / 10.0);
    if (textHeight <= 0.0 || textHeight * viewport.scale() < 7.0) {
        return;
    }

    DrawItem label;
    label.kind = DrawItem::Kind::Text;
    label.handle = entity.handle;
    label.style = style;
    label.position = transform.transformPoint(extents.min.midpoint(extents.max));
    label.text = caption;
    label.textHeight = textHeight * transform.scaleFactor2();
    label.horizontalAlignment = static_cast<int>(TextHorizontalAlignment::Center);
    label.verticalAlignment = static_cast<int>(TextVerticalAlignment::Middle);
    scene.items.push_back(std::move(label));
}

void SceneBuilder::appendEntity(const Entity& entity, const Mat4& transform,
                                const ResolutionContext& context, const Viewport& viewport,
                                const SceneOptions& options, Scene& scene) {
    // Resolved once and reused. It used to be computed here for the visibility
    // test and again inside resolveStyle, which doubled the cost of the single
    // most-executed function in the renderer for no benefit.
    const ResolvedProperties resolved = entity.resolveProperties(context);
    if (!resolved.visible) {
        ++scene.culled;
        return;
    }

    DrawItem item;
    item.handle = entity.handle;
    item.style = resolveStyle(resolved);
    item.fromField = !entity.fieldHandle.isNull();

    if (const auto* dimension = dynamic_cast<const DimensionEntity*>(&entity)) {
        appendDimension(*dimension, transform, item.style, viewport, scene);
        return;
    }

    if (const auto* leader = dynamic_cast<const LeaderEntity*>(&entity)) {
        appendLeader(*leader, transform, item.style, scene);
        return;
    }

    if (const auto* multileader = dynamic_cast<const MLeaderEntity*>(&entity)) {
        appendMLeader(*multileader, transform, item.style, scene);
        return;
    }

    if (const auto* solid = dynamic_cast<const SolidEntity*>(&entity)) {
        // Filled, not stroked: a SOLID is a filled region, and outlining one
        // draws a hairline triangle where a black one belongs.
        item.kind = DrawItem::Kind::Filled;
        item.geometry.push_back(closedRun(solid->ring(), transform));
        scene.items.push_back(std::move(item));
        return;
    }

    if (const auto* hatch = dynamic_cast<const HatchEntity*>(&entity)) {
        appendHatch(*hatch, transform, item.style, viewport, options, scene);
        return;
    }

    if (const auto* table = dynamic_cast<const TableEntity*>(&entity)) {
        appendTable(*table, transform, item.style, viewport, scene);
        return;
    }

    if (const auto* image = dynamic_cast<const RasterImageEntity*>(&entity)) {
        appendImage(*image, transform, item.style, viewport, options, scene);
        return;
    }

    // An underlay and an OLE object are drawn the same way for the same reason:
    // the drawing knows exactly where they are and nothing about what they
    // contain. Before this they rendered as nothing at all, which left a user
    // able to select something they could not see.
    if (const auto* underlay = dynamic_cast<const UnderlayEntity*>(&entity)) {
        const auto* definition = db_.get<UnderlayDefinition>(underlay->definitionHandle);
        // A PDF page whose file was found and whose size is known is a
        // picture like any other: the backend renders the page. The engine
        // only says where it goes, as with an image.
        const bool drawable = definition != nullptr && !definition->resolvedPath.empty() &&
                              underlay->kind == UnderlayKind::Pdf && underlay->underlayOn &&
                              underlay->pageSize.x > 0.0 && underlay->pageSize.y > 0.0;
        if (drawable) {
            const Mat4 placement = underlay->placementTransform();
            SceneImage payload;
            payload.path = definition->resolvedPath;
            payload.storedPath = definition->fileName;
            // Pages are numbered from one in the file and from zero here.
            int page = 0;
            try {
                page = std::max(0, std::stoi(definition->itemName) - 1);
            } catch (const std::exception&) {
                page = 0;
            }
            payload.page = page;
            const Vec3 size = underlay->pageSize;
            payload.topLeft = transform.transformPoint(placement.transformPoint(Vec3{0.0, size.y, 0.0}));
            payload.across = transform.transformVector(placement.transformVector(Vec3{size.x, 0.0, 0.0}));
            payload.down = transform.transformVector(placement.transformVector(Vec3{0.0, -size.y, 0.0}));
            if (underlay->clippingEnabled && underlay->clipVertices.size() >= 2) {
                for (const Vec3& point : underlay->displayOutline()) {
                    payload.clip.push_back(transform.transformPoint(point));
                }
            }
            payload.fade = underlay->fade;

            DrawItem picture;
            picture.kind = DrawItem::Kind::Image;
            picture.handle = underlay->handle;
            picture.style = item.style;
            picture.reference = scene.images.size();
            scene.images.push_back(std::move(payload));
            scene.items.push_back(std::move(picture));
            if (options.showImageFrames) {
                DrawItem outline;
                outline.kind = DrawItem::Kind::Geometry;
                outline.handle = underlay->handle;
                outline.style = item.style;
                outline.geometry.push_back(closedRun(underlay->displayOutline(), transform));
                scene.items.push_back(std::move(outline));
            }
            return;
        }

        std::string caption{underlayKindName(underlay->kind)};
        if (definition != nullptr) {
            caption += ": " + definition->fileName;
            if (!definition->itemName.empty()) {
                caption += " (" + definition->itemName + ")";
            }
        }
        appendPlaceholderFrame(entity, underlay->displayOutline(), caption, transform, item.style,
                               viewport, options, scene);
        return;
    }

    if (const auto* ole = dynamic_cast<const OleFrameEntity*>(&entity)) {
        appendPlaceholderFrame(entity, ole->corners(), "OLE object", transform, item.style,
                               viewport, options, scene);
        return;
    }

    // Text carries its content rather than geometry: the backend owns font
    // measurement and layout, which the core deliberately cannot do.
    if (const auto* text = dynamic_cast<const TextEntity*>(&entity)) {
        // An invisible attribute is data, not drawing.
        if (const auto* attribute = dynamic_cast<const AttributeEntity*>(text);
            attribute != nullptr && attribute->flags.isInvisible) {
            return;
        }
        item.kind = DrawItem::Kind::Text;
        item.position = transform.transformPoint(text->effectivePosition());
        item.text = decodeTextSymbols(text->contents);
        // A definition in the open -- in the model, or a block being edited --
        // shows its tag, which is what a drafter placing it needs to see. Seen
        // through an insert it shows its value (only constant ones get here).
        if (const auto* definition = dynamic_cast<const AttributeDefinitionEntity*>(text);
            definition != nullptr && insertDepth_ == 0) {
            item.text = definition->tag;
        }
        item.textHeight = text->height * transform.scaleFactor2();
        item.textRotation = text->rotation + transform.rotation2();
        item.horizontalAlignment = static_cast<int>(text->horizontalAlignment);
        item.verticalAlignment = static_cast<int>(text->verticalAlignment);
        scene.items.push_back(std::move(item));
        return;
    }

    if (const auto* mtext = dynamic_cast<const MTextEntity*>(&entity)) {
        item.kind = DrawItem::Kind::Text;
        item.position = transform.transformPoint(mtext->position);
        item.text = mtext->plainText();
        item.textHeight = mtext->height * transform.scaleFactor2();
        // `\P` survives plainText() as a newline, and bounds() reserves a line
        // of height for each. Handing over the spacing lets the backend lay
        // them out on the same terms, instead of running a two-line room label
        // together on one line inside a box sized for two.
        item.lineSpacing = mtext->height * mtext->lineSpacingFactor * transform.scaleFactor2();
        item.textRotation = mtext->rotation + transform.rotation2();

        // MTEXT anchors by a nine-way attachment point, not by TEXT's separate
        // horizontal and vertical codes, so it has to be split into the two the
        // backend understands. Leaving them unset drew every MTEXT from its
        // insertion point regardless of attachment, while bounds() honoured the
        // attachment — so a centred room label was picked from a box sitting
        // half a label to its left, and clicking the text selected nothing.
        //
        // Derived by the same column/row split bounds() uses, because these two
        // agreeing *is* the requirement.
        const int index = std::clamp(static_cast<int>(mtext->attachment) - 1, 0, 8);
        static constexpr int kColumnToHorizontal[] = {
            static_cast<int>(TextHorizontalAlignment::Left),
            static_cast<int>(TextHorizontalAlignment::Center),
            static_cast<int>(TextHorizontalAlignment::Right),
        };
        static constexpr int kRowToVertical[] = {
            static_cast<int>(TextVerticalAlignment::Top),
            static_cast<int>(TextVerticalAlignment::Middle),
            static_cast<int>(TextVerticalAlignment::Bottom),
        };
        item.horizontalAlignment = kColumnToHorizontal[index % 3];
        item.verticalAlignment = kRowToVertical[index / 3];

        // Formatting travels separately, and only when there is any. A note
        // with a bold heading and a body is two runs; a plain label is one, and
        // pays nothing.
        const std::vector<MTextRun> runs = mtext->formattedRuns();
        const bool formatted =
            runs.size() > 1 ||
            (runs.size() == 1 && !runs.front().sameFormatting(MTextRun{}));
        if (formatted) {
            std::vector<SceneTextRun> converted;
            converted.reserve(runs.size());
            for (const MTextRun& run : runs) {
                SceneTextRun out;
                out.text = run.text;
                out.heightFactor = run.heightFactor;
                out.widthFactor = run.widthFactor;
                out.bold = run.bold;
                out.italic = run.italic;
                out.underline = run.underline;
                out.overline = run.overline;
                out.strikethrough = run.strikethrough;
                out.font = run.font;
                out.endsLine = run.endsParagraph;
                if (run.color) {
                    // Already an explicit colour: the parser leaves ByLayer and
                    // ByBlock unset, because neither is a colour the run is
                    // choosing.
                    out.hasColor = true;
                    out.rgb = run.color->toRgb();
                }
                converted.push_back(std::move(out));
            }
            item.reference = scene.textRuns.size();
            scene.textRuns.push_back(std::move(converted));
        }

        scene.items.push_back(std::move(item));
        return;
    }

    // A point has no extent, so its marker is drawn at a fixed pixel size —
    // one of the few places where screen space is the correct space.
    if (const auto* point = dynamic_cast<const PointEntity*>(&entity)) {
        item.kind = DrawItem::Kind::Marker;
        item.position = transform.transformPoint(point->position);
        scene.items.push_back(std::move(item));
        return;
    }

    // A polyline carrying a width is a filled shape rather than a thick line.
    // Its width is in drawing units and scales with the drawing, unlike
    // lineweight, which is a plotting property in millimetres on the paper --
    // so it cannot be handed to the backend as a pen thickness.
    if (const auto* polyline = dynamic_cast<const PolylineEntity*>(&entity);
        polyline != nullptr && hasWidth(*polyline)) {
        appendWidePolyline(*polyline, transform, item, viewport, scene);
        return;
    }

    const std::vector<CurvePtr> curves = entity.toCurves();
    if (curves.empty()) {
        return;
    }

    // Tessellation quality follows the zoom, so curves are smooth when zoomed
    // in without generating thousands of segments for a tiny circle.
    TessellationOptions tessellation;
    tessellation.chordTolerance = chordToleranceFor(viewport);
    tessellation.minSegments = 2;
    tessellation.maxSegments = 4096;

    item.kind = DrawItem::Kind::Geometry;
    for (const CurvePtr& curve : curves) {
        const CurvePtr worldCurve =
            transform.isIdentity() ? nullptr : curve->transformed(transform);
        const Curve& source = worldCurve == nullptr ? *curve : *worldCurve;

        Polyline2D run;
        run.points = source.tessellate(tessellation);
        run.closed = source.isClosed();
        if (run.points.size() >= 2) {
            item.geometry.push_back(std::move(run));
        }
    }

    if (!item.geometry.empty()) {
        scene.items.push_back(std::move(item));
    }
}

void SceneBuilder::expandInsert(const Entity& insertEntity, const Mat4& parentTransform,
                                const Viewport& viewport, const SceneOptions& options,
                                int depth, Scene& scene) {
    const auto* insert = dynamic_cast<const InsertEntity*>(&insertEntity);
    if (insert == nullptr || depth > options.maxDepth) {
        return;
    }

    const auto* definition = db_.get<BlockTableRecord>(insert->blockHandle);
    if (definition == nullptr) {
        return;
    }

    // A reference whose content could not be loaded still draws a marker, so a
    // missing xref is visible rather than silently absent.
    if (definition->isMissingContent()) {
        if (options.showMissingReferences) {
            DrawItem item;
            item.kind = DrawItem::Kind::MissingReference;
            item.handle = insert->handle;
            item.position = parentTransform.transformPoint(insert->position);
            item.style = resolveStyle(insert->resolveProperties(contextFor(*insert)));
            scene.items.push_back(std::move(item));
        }
        return;
    }

    const Box3 visible = viewport.visibleBounds();
    const double minWorldSize = viewport.pixelsToWorld(options.minPixelSize);

    for (const Mat4& cell : insert->cellTransforms()) {
        const Mat4 transform = parentTransform.multiply(cell);

        // Cull the whole cell before touching its contents.
        const Box3 cellBounds = insert->blockExtents().transformed(transform);
        if (!cellBounds.isEmpty()) {
            if (!cellBounds.intersects2(visible)) {
                continue;
            }
            // An entire block smaller than a pixel is dropped whole — the
            // single biggest saving on drawings full of small repeated symbols.
            if (cellBounds.diagonal() < minWorldSize) {
                ++scene.culled;
                continue;
            }
        }

        for (const Entity* child : db_.entitiesIn(*definition)) {
            if (scene.items.size() >= options.maxItems) {
                return;
            }
            if (dynamic_cast<const InsertEntity*>(child) != nullptr) {
                expandInsert(*child, transform, viewport, options, depth + 1, scene);
                continue;
            }

            // An attribute definition is a question the insert answers with
            // its own ATTRIB; drawing the definition too would print the
            // default under every value. A constant one has no ATTRIB and is
            // drawn from the definition, as other CAD software draws it.
            if (const auto* attribute = dynamic_cast<const AttributeDefinitionEntity*>(child);
                attribute != nullptr &&
                (!attribute->flags.isConstant || attribute->flags.isInvisible)) {
                continue;
            }

            const Box3 childBounds = child->bounds().transformed(transform);
            if (!childBounds.isEmpty() && !childBounds.intersects2(visible)) {
                continue;
            }

            // Properties resolve against the *reference*, so a ByBlock entity
            // takes its colour from the insert rather than from the definition.
            ++insertDepth_;
            appendEntity(*child, transform, db_.resolutionContextFor(*child, *insert), viewport,
                         options, scene);
            --insertDepth_;
        }
    }
}

// `space` is accepted for interface symmetry with `reindex` and to make the
// call site read correctly, but the build walks the index rather than the
// record: the index already holds exactly that space's contents, and querying
// it is the whole point of having one.
/// Put handles into the order their space draws them: the order the record
/// keeps them in, which DRAWORDER rearranges. The index hands them back in
/// its own order, and a hatch drawn after the wall it fills would hide it.
void sortByDrawOrder(std::vector<Handle>& handles, const BlockTableRecord& space) {
    if (handles.size() < 2) {
        return;
    }
    std::unordered_map<Handle, std::size_t> rank;
    rank.reserve(space.entityHandles.size());
    for (std::size_t i = 0; i < space.entityHandles.size(); ++i) {
        rank.emplace(space.entityHandles[i], i);
    }
    const auto rankOf = [&](Handle handle) {
        const auto found = rank.find(handle);
        return found == rank.end() ? std::numeric_limits<std::size_t>::max() : found->second;
    };
    std::stable_sort(handles.begin(), handles.end(),
                     [&](Handle a, Handle b) { return rankOf(a) < rankOf(b); });
}

Scene SceneBuilder::build(const BlockTableRecord& space, const Viewport& viewport,
                          const SceneOptions& options) {
    const double started = nowMilliseconds();
    contextCache_.clear();

    Scene scene;
    const Box3 visible = viewport.visibleBounds();
    const double minWorldSize = viewport.pixelsToWorld(options.minPixelSize);

    std::vector<Handle> candidates = index_.search(visible);
    sortByDrawOrder(candidates, space);
    scene.totalCandidates = index_.size();

    for (const Handle handle : candidates) {
        if (scene.items.size() >= options.maxItems) {
            break;
        }
        Entity* entity = db_.getEntity(handle);
        if (entity == nullptr || entity->erased) {
            continue;
        }

        if (dynamic_cast<InsertEntity*>(entity) != nullptr) {
            expandInsert(*entity, Mat4::identity(), viewport, options, 0, scene);
            continue;
        }

        // Level of detail: an entity smaller than a pixel contributes nothing.
        //
        // A *zero*-extent entity is exempt, not culled. A POINT has no extent
        // by definition and is drawn as a fixed-pixel marker, so a naive
        // "diagonal < one pixel" test makes every point in the drawing vanish.
        // Genuinely degenerate geometry is dropped later, when tessellation
        // yields fewer than two vertices.
        const Box3 bounds = entity->bounds();
        const double extent = bounds.diagonal();
        if (!bounds.isEmpty() && extent > 0.0 && extent < minWorldSize) {
            ++scene.culled;
            continue;
        }

        appendEntity(*entity, Mat4::identity(), contextFor(*entity), viewport,
                     options, scene);
    }

    scene.buildMilliseconds = nowMilliseconds() - started;
    return scene;
}

// ------------------------------------------------------------- paper space

void SceneBuilder::appendViewportContents(const ViewportEntity& window, const Viewport& viewport,
                                          const SceneOptions& options, Scene& scene) {
    const Box3 region = window.modelRegion();
    const Box3 clip = window.paperRect();
    if (region.isEmpty() || clip.isEmpty()) {
        return;
    }
    const Mat4 toPaper = window.modelToPaper();

    // Cull in *model* space against what this window shows, intersected with
    // what is on screen. Two culls rather than one: a sheet zoomed to a corner
    // must not pay for the whole model, and a viewport showing one room must
    // not pay for the rest of the building.
    Box3 wanted = region;
    if (const auto inverse = toPaper.inverted()) {
        wanted = region.intersected(viewport.visibleBounds().transformed(*inverse));
    }
    if (wanted.isEmpty()) {
        return;
    }

    // Level of detail follows the *paper* scale: an entity a metre long in a
    // 1:100 viewport is a centimetre on the sheet, and should be dropped on the
    // same terms as a centimetre-long sheet annotation.
    const double modelPerPaper = window.modelPerPaper();
    const std::size_t itemsBefore = scene.items.size();

    // The model's index, not the sheet's: see setModelIndex.
    const SpatialIndex& model = modelIndex_ != nullptr ? *modelIndex_ : index_;
    std::vector<Handle> shown = model.search(wanted);
    sortByDrawOrder(shown, db_.modelSpace());
    for (const Handle handle : shown) {
        if (scene.items.size() >= options.maxItems) {
            break;
        }
        Entity* entity = db_.getEntity(handle);
        if (entity == nullptr || entity->erased) {
            continue;
        }
        // Layers frozen in this viewport alone: the mechanism that lets one
        // model serve a demolition sheet and a new-work sheet.
        if (window.isLayerFrozen(entity->layerHandle)) {
            ++scene.culled;
            continue;
        }

        if (dynamic_cast<InsertEntity*>(entity) != nullptr) {
            expandInsert(*entity, toPaper, viewport, options, 0, scene);
            continue;
        }

        const Box3 bounds = entity->bounds();
        const double paperExtent = bounds.diagonal() / std::max(modelPerPaper, 1e-12);
        if (!bounds.isEmpty() && paperExtent > 0.0 &&
            paperExtent < viewport.pixelsToWorld(options.minPixelSize)) {
            ++scene.culled;
            continue;
        }

        appendEntity(*entity, toPaper, contextFor(*entity), viewport, options,
                     scene);
    }

    // Everything this window produced is clipped to its frame. Applied
    // afterwards so each of the several items an entity can produce — strokes,
    // fills and text for one dimension — is covered without threading the clip
    // through every append path.
    for (std::size_t i = itemsBefore; i < scene.items.size(); ++i) {
        scene.items[i].clip = clip;
    }
}

Scene SceneBuilder::buildLayout(const Layout& layout, const Viewport& viewport,
                                const SceneOptions& options) {
    const double started = nowMilliseconds();
    contextCache_.clear();
    Scene scene;

    const auto* sheet = db_.get<BlockTableRecord>(layout.blockRecordHandle);
    if (sheet == nullptr) {
        scene.buildMilliseconds = nowMilliseconds() - started;
        return scene;
    }

    const std::vector<Entity*> contents = db_.entitiesIn(*sheet);
    scene.totalCandidates = contents.size();
    const Box3 visible = viewport.visibleBounds();

    // Viewport contents first, so the sheet's own geometry — the title block
    // and the viewport frames — draws over them rather than under.
    for (const Entity* entity : contents) {
        const auto* window = dynamic_cast<const ViewportEntity*>(entity);
        if (window == nullptr || window->isPaperSpaceViewport() || !window->isOn()) {
            continue;
        }
        if (!window->paperRect().intersects2(visible)) {
            continue;
        }
        appendViewportContents(*window, viewport, options, scene);
    }

    for (Entity* entity : contents) {
        if (scene.items.size() >= options.maxItems) {
            break;
        }
        if (entity->erased) {
            continue;
        }
        // The sheet's pseudo-viewport stands for the paper itself; drawing it
        // would put a frame round the whole sheet and a second copy of the
        // model inside it.
        if (const auto* window = dynamic_cast<const ViewportEntity*>(entity);
            window != nullptr && window->isPaperSpaceViewport()) {
            continue;
        }
        if (auto* insert = dynamic_cast<InsertEntity*>(entity)) {
            if (auto* definition = db_.get<BlockTableRecord>(insert->blockHandle)) {
                insert->setBlockExtents(db_.blockExtents(*definition));
            }
            expandInsert(*entity, Mat4::identity(), viewport, options, 0, scene);
            continue;
        }
        const Box3 bounds = entity->bounds();
        if (!bounds.isEmpty() && !bounds.intersects2(visible)) {
            continue;
        }
        appendEntity(*entity, Mat4::identity(), contextFor(*entity), viewport,
                     options, scene);
    }

    scene.buildMilliseconds = nowMilliseconds() - started;
    return scene;
}

Box3 spaceExtents(Database& db, const BlockTableRecord& space, const SpatialIndex* index) {
    if (index != nullptr && index->size() > 0) {
        return index->bounds();
    }
    return db.blockExtents(space);
}

}  // namespace cad::render
