#pragma once

#include <vector>

#include "cad/db/Entity.hpp"

namespace cad {

/// A block reference — the INSERT entity.
///
/// An insert places a block record's contents into a drawing under a transform.
/// It does *not* copy them: one definition, many references, which is what
/// makes blocks memory-efficient and globally editable.
///
/// Inserts may nest arbitrarily, and the same mechanism carries external
/// references. Bounds and rendering therefore need the block definition, which
/// the entity cannot reach on its own — the database supplies it. That
/// indirection is deliberate: it keeps the entity free of a back-pointer that
/// undo would have to maintain.
class InsertEntity final : public Entity {
public:
    /// Handle of the block definition being referenced.
    Handle blockHandle;

    /// Where the block's origin lands, in the containing space.
    Vec3 position;

    /// Per-axis scale. Negative values mirror.
    Vec3 scale{1.0, 1.0, 1.0};

    /// Rotation about the normal, in radians.
    double rotation = 0.0;

    /// Handles of ATTRIB entities attached to this reference.
    std::vector<Handle> attributeHandles;

    /// Rectangular array parameters (the MINSERT entity).
    int columnCount = 1;
    int rowCount = 1;
    double columnSpacing = 0.0;
    double rowSpacing = 0.0;

    InsertEntity() = default;
    InsertEntity(Handle block, const Vec3& where) : blockHandle(block), position(where) {}

    [[nodiscard]] std::string objectType() const override { return "Insert"; }

    /// True when this is a MINSERT array rather than a single placement.
    [[nodiscard]] bool isArray() const noexcept { return columnCount > 1 || rowCount > 1; }

    /// Transform from the block definition's coordinate system into the
    /// containing space. Scale, then rotate, then translate — the order DWG
    /// defines and the one every nested traversal must use.
    [[nodiscard]] Mat4 blockTransform() const {
        return Mat4::blockTransform(position, scale, rotation);
    }

    /// Transform for one cell of a MINSERT array.
    [[nodiscard]] Mat4 transformForCell(int column, int row) const;

    /// All cell transforms, one per placement.
    [[nodiscard]] std::vector<Mat4> cellTransforms() const;

    /// Supply the extents of the referenced block's contents so `bounds` can be
    /// computed. Called by the database, the only component able to traverse
    /// the definition.
    ///
    /// Recomputing a deeply nested block's extents on every bounds query is a
    /// dominant cost on large drawings, so the result is cached here. It is
    /// never persisted.
    void setBlockExtents(const Box3& extents) { blockExtents_ = extents; }
    void clearBlockExtents() { blockExtents_ = Box3::empty(); }

    [[nodiscard]] const Box3& blockExtents() const noexcept { return blockExtents_; }

    /// World extents of this reference.
    ///
    /// Falls back to a degenerate box at the insertion point when the block's
    /// extents are unknown — a reference to a missing xref still has a
    /// location, and reporting an empty box there would exclude it from ZOOM
    /// EXTENTS and make the problem invisible.
    [[nodiscard]] Box3 bounds() const override;

    /// Block references expose no curves of their own. Their geometry belongs
    /// to the definition and is reached by traversal, not by flattening —
    /// flattening here would defeat the point of blocks and break global edits.
    [[nodiscard]] std::vector<CurvePtr> toCurves() const override { return {}; }

    [[nodiscard]] double distanceToPoint(const Vec3& p) const override;

    void transformBy(const Mat4& m) override;

    [[nodiscard]] std::vector<GripPoint> gripPoints() const override;
    void moveGrip(const GripPoint& grip, const Vec3& to) override;

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
    [[nodiscard]] std::vector<Handle> referencedHandles() const override;

private:
    Box3 blockExtents_;
};

}  // namespace cad
