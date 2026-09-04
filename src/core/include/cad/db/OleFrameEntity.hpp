#pragma once

#include <string>
#include <vector>

#include "cad/db/Entity.hpp"

namespace cad {

/// An embedded OLE object — the OLE2FRAME entity.
///
/// A spreadsheet fragment, a Word table, a pasted PDF page: Windows applications
/// have dropped these into drawings for thirty years and they turn up in real
/// files constantly. The payload is a whole OLE compound document, megabytes of
/// it, and rendering it means asking Windows to render it — a dependency this
/// application does not have and would not want in its engine.
///
/// So what is modelled is the *frame*: two corners, a layer, a place in the
/// draw order. That is enough for the entity to be seen, selected, moved,
/// plotted as an outline, and — the part that actually matters — saved back
/// with its payload intact. An OLE object that silently vanished on save would
/// take somebody's schedule table with it.
///
/// Before this existed, the frame was invisible and unselectable: a rectangle
/// of nothing that a user could neither see nor click, in the middle of a sheet
/// that plainly had something there in other CAD software.
class OleFrameEntity final : public Entity {
public:
    /// The frame's upper-left corner (group 10).
    Vec3 upperLeft;

    /// The frame's lower-right corner (group 11).
    Vec3 lowerRight;

    // Nothing else about the entity is interpreted, deliberately.
    //
    // The version, object type and tile-mode codes are written differently by
    // different applications — this drawing's OLE2FRAME carries 72 and 73 where
    // the published group codes say 71 and 72 — and the only use for them here
    // would be to label the frame. A label that says "embedded" because a code
    // happened to be 2 is worse than no label: it is a guess wearing the
    // clothes of a fact. They round-trip verbatim with the rest of the record.

    [[nodiscard]] std::string objectType() const override { return "OleFrame"; }

    /// The four world corners, counter-clockwise from the lower-left.
    [[nodiscard]] std::vector<Vec3> corners() const;

    [[nodiscard]] Box3 bounds() const override;
    void transformBy(const Mat4& m) override;
    [[nodiscard]] std::vector<CurvePtr> toCurves() const override;
    [[nodiscard]] double distanceToPoint(const Vec3& p) const override;
    [[nodiscard]] std::vector<GripPoint> gripPoints() const override;
    void moveGrip(const GripPoint& grip, const Vec3& to) override;

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
};

}  // namespace cad
