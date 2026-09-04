#include "cad/db/MlineEntity.hpp"

#include <stdexcept>

#include "cad/geom/LineSegment.hpp"

namespace cad {

std::vector<std::vector<Vec3>> MlineEntity::elementLines() const {
    std::vector<std::vector<Vec3>> lines;
    if (elementCount <= 0) {
        return lines;
    }
    lines.resize(static_cast<std::size_t>(elementCount));

    for (const MlineVertex& vertex : vertices) {
        for (std::size_t element = 0; element < lines.size(); ++element) {
            if (element >= vertex.elementOffsets.size() ||
                vertex.elementOffsets[element].empty()) {
                continue;
            }
            // The first parameter is where the element's line crosses the
            // miter; the rest describe breaks in it, which are not drawn yet.
            const double offset = vertex.elementOffsets[element].front();
            lines[element].push_back(vertex.position + vertex.miterDirection * offset);
        }
    }

    if (closed) {
        for (std::vector<Vec3>& line : lines) {
            if (line.size() >= 3) {
                line.push_back(line.front());
            }
        }
    }
    return lines;
}

Box3 MlineEntity::bounds() const {
    Box3 box = Box3::empty();
    for (const std::vector<Vec3>& line : elementLines()) {
        for (const Vec3& point : line) {
            box.expandToPoint(point);
        }
    }
    if (box.isEmpty()) {
        // A multiline whose elements produced nothing still has a location.
        // Reporting nothing would drop it out of ZOOM EXTENTS and leave the
        // user unable to find the thing they cannot see.
        for (const MlineVertex& vertex : vertices) {
            box.expandToPoint(vertex.position);
        }
    }
    return box;
}

std::vector<CurvePtr> MlineEntity::toCurves() const {
    std::vector<CurvePtr> curves;
    for (const std::vector<Vec3>& line : elementLines()) {
        for (std::size_t i = 0; i + 1 < line.size(); ++i) {
            curves.push_back(std::make_unique<LineSegment>(line[i], line[i + 1]));
        }
    }
    return curves;
}

void MlineEntity::transformBy(const Mat4& m) {
    startPoint = m.transformPoint(startPoint);

    // The offsets are lengths along the miter direction, so a scale has to
    // reach them too. Left alone, a multiline scaled up would keep its original
    // wall thickness while its length changed -- which is not a wall.
    const double factor = m.scaleFactor2();
    const bool rescale = factor > 0.0 && factor != 1.0;

    for (MlineVertex& vertex : vertices) {
        vertex.position = m.transformPoint(vertex.position);
        vertex.direction = m.transformVector(vertex.direction);
        // Normalized after transforming: the offsets are measured along this,
        // so a miter vector that grew with the transform would scale them a
        // second time.
        vertex.miterDirection = m.transformVector(vertex.miterDirection);
        if (vertex.miterDirection.lengthSquared() > 0.0) {
            vertex.miterDirection = vertex.miterDirection.normalized();
        }
        if (!rescale) {
            continue;
        }
        for (std::vector<double>& offsets : vertex.elementOffsets) {
            for (double& offset : offsets) {
                offset *= factor;
            }
        }
        for (std::vector<double>& offsets : vertex.fillOffsets) {
            for (double& offset : offsets) {
                offset *= factor;
            }
        }
    }

    if (rescale) {
        scale *= factor;
    }
}

std::vector<GripPoint> MlineEntity::gripPoints() const {
    // The vertices, not the element lines. A multiline is edited by its spine;
    // dragging one element away from the others would not be a multiline.
    std::vector<GripPoint> grips;
    grips.reserve(vertices.size());
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        grips.push_back({vertices[i].position, GripKind::Endpoint, i});
    }
    return grips;
}

void MlineEntity::moveGrip(const GripPoint& grip, const Vec3& to) {
    if (grip.index >= vertices.size()) {
        return;
    }
    vertices[grip.index].position = to;
    if (grip.index == 0) {
        startPoint = to;
    }
    // The miter directions of this vertex and its neighbours are now wrong --
    // they bisect angles that have changed. Recomputing them needs the style's
    // element offsets to stay parallel, which is not modelled yet, so the joint
    // keeps its old angle rather than being silently recomputed wrongly.
}

DbObjectPtr MlineEntity::clone() const {
    auto copy = std::make_unique<MlineEntity>();
    copyEntityInto(*copy);
    copy->styleName = styleName;
    copy->styleHandle = styleHandle;
    copy->scale = scale;
    copy->justification = justification;
    copy->closed = closed;
    copy->suppressStartCaps = suppressStartCaps;
    copy->suppressEndCaps = suppressEndCaps;
    copy->elementCount = elementCount;
    copy->startPoint = startPoint;
    copy->vertices = vertices;
    return copy;
}

void MlineEntity::restoreFrom(const DbObject& snapshot) {
    const auto* source = dynamic_cast<const MlineEntity*>(&snapshot);
    if (source == nullptr) {
        throw std::invalid_argument("Snapshot type mismatch restoring a multiline");
    }
    restoreEntityFrom(*source);
    styleName = source->styleName;
    styleHandle = source->styleHandle;
    scale = source->scale;
    justification = source->justification;
    closed = source->closed;
    suppressStartCaps = source->suppressStartCaps;
    suppressEndCaps = source->suppressEndCaps;
    elementCount = source->elementCount;
    startPoint = source->startPoint;
    vertices = source->vertices;
}

}  // namespace cad
