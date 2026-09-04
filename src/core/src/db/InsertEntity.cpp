#include "cad/db/InsertEntity.hpp"

#include <algorithm>
#include <stdexcept>

namespace cad {

Mat4 InsertEntity::transformForCell(int column, int row) const {
    if (!isArray()) {
        return blockTransform();
    }
    // Array offsets are applied in the *rotated* frame, matching other CAD software.
    const Vec3 offset = Vec3{static_cast<double>(column) * columnSpacing,
                             static_cast<double>(row) * rowSpacing, 0.0}
                            .rotatedZ(rotation);
    return Mat4::translation(position + offset)
        .multiply(Mat4::rotationZ(rotation))
        .multiply(Mat4::scaling(scale));
}

std::vector<Mat4> InsertEntity::cellTransforms() const {
    std::vector<Mat4> transforms;
    const int rows = std::max(1, rowCount);
    const int columns = std::max(1, columnCount);
    transforms.reserve(static_cast<std::size_t>(rows) * static_cast<std::size_t>(columns));

    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            transforms.push_back(transformForCell(column, row));
        }
    }
    return transforms;
}

Box3 InsertEntity::bounds() const {
    if (blockExtents_.isEmpty()) {
        return Box3::fromPoint(position);
    }
    Box3 result;
    for (const Mat4& transform : cellTransforms()) {
        result = result.united(blockExtents_.transformed(transform));
    }
    return result;
}

double InsertEntity::distanceToPoint(const Vec3& p) const {
    const Box3 box = bounds();
    return box.containsPoint(p) ? 0.0 : box.distanceToPoint(p);
}

void InsertEntity::transformBy(const Mat4& m) {
    // Compose the new placement rather than transforming the stored scalars
    // independently, so rotation and mirroring interact correctly.
    const Mat4 composed = m.multiply(blockTransform());
    position = composed.translationPart();
    rotation = composed.rotation2();

    // Extract per-axis scale from the composed basis: the column lengths give
    // the magnitudes, and the determinant's sign gives the mirror.
    const auto& e = composed.m;
    const double sx = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
    const double sy = std::sqrt(e[4] * e[4] + e[5] * e[5] + e[6] * e[6]);
    const double sz = std::sqrt(e[8] * e[8] + e[9] * e[9] + e[10] * e[10]);

    scale = Vec3{composed.isMirrored() ? -sx : sx, sy, sz == 0.0 ? scale.z : sz};

    const double spacingScale = m.scaleFactor2();
    columnSpacing *= spacingScale;
    rowSpacing *= spacingScale;
}

std::vector<GripPoint> InsertEntity::gripPoints() const {
    return {{position, GripKind::Insertion, 0}};
}

void InsertEntity::moveGrip(const GripPoint& /*grip*/, const Vec3& to) {
    position = to;
}

DbObjectPtr InsertEntity::clone() const {
    auto copy = std::make_unique<InsertEntity>();
    copyEntityInto(*copy);
    copy->blockHandle = blockHandle;
    copy->position = position;
    copy->scale = scale;
    copy->rotation = rotation;
    copy->attributeHandles = attributeHandles;
    copy->columnCount = columnCount;
    copy->rowCount = rowCount;
    copy->columnSpacing = columnSpacing;
    copy->rowSpacing = rowSpacing;
    copy->blockExtents_ = blockExtents_;
    return copy;
}

void InsertEntity::restoreFrom(const DbObject& snapshot) {
    const auto* source = dynamic_cast<const InsertEntity*>(&snapshot);
    if (source == nullptr) {
        throw std::invalid_argument("Snapshot type mismatch restoring an insert");
    }
    restoreEntityFrom(*source);
    blockHandle = source->blockHandle;
    position = source->position;
    scale = source->scale;
    rotation = source->rotation;
    attributeHandles = source->attributeHandles;
    columnCount = source->columnCount;
    rowCount = source->rowCount;
    columnSpacing = source->columnSpacing;
    rowSpacing = source->rowSpacing;
    blockExtents_ = source->blockExtents_;
}

std::vector<Handle> InsertEntity::referencedHandles() const {
    std::vector<Handle> refs = Entity::referencedHandles();
    if (!blockHandle.isNull()) {
        refs.push_back(blockHandle);
    }
    refs.insert(refs.end(), attributeHandles.begin(), attributeHandles.end());
    return refs;
}

}  // namespace cad
