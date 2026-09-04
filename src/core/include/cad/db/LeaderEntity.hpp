#pragma once

#include <array>
#include <string>
#include <vector>

#include "cad/db/DimStyle.hpp"
#include "cad/db/Entity.hpp"
#include "cad/geom/LineSegment.hpp"

namespace cad {

/// What a leader draws, in world coordinates.
///
/// Built on demand from the vertices and the style, the same way a dimension's
/// geometry is (ADR 0005): the arrow size and the landing length come from the
/// dimension style, so a leader drawn from a stored picture would stop matching
/// the drawing's standard the moment that standard changed.
struct LeaderGeometry {
    std::vector<LineSegment> lines;
    /// The filled arrowhead at the first vertex, when the leader has one.
    std::vector<std::array<Vec3, 3>> arrowheads;
    /// Where the annotation hangs, at the far end of the landing.
    Vec3 landingEnd;
    /// True when the landing runs to the right, which decides which side of
    /// the hook the text sits on.
    bool landingToTheRight = true;
};

/// A leader: an arrow, a path back from it, and a note at the far end.
///
/// The entity that points at something and says what it is. A drawing set is
/// mostly leaders — every callout, every material tag, every "SEE DETAIL 4/A5.1"
/// — and without one there is no way to label a thing in place.
///
/// **The note is a separate entity.** `annotationHandle` names an MTEXT, and
/// that is how DXF stores it (group 340) rather than an accident of this
/// implementation. It costs a reference to keep in step, and in exchange the
/// note is edited, formatted, and rendered by everything that already knows how
/// to do those things to MTEXT, instead of by a second text implementation
/// living inside the leader.
///
/// **LEADER rather than MULTILEADER.** other CAD software's current command makes an
/// MLEADER, whose DXF form is a nested CONTEXT_DATA structure carrying its own
/// copy of the text, the leader lines, and a style override block. LEADER is
/// the older entity and is read, drawn and edited by every version of other CAD software
/// in use. Writing the simpler one that every consumer understands is worth
/// more than matching the newer one's storage, and it is the same trade ADR
/// 0001 makes about not inventing what cannot be done properly.
class LeaderEntity final : public Entity {
public:
    /// The path, arrow point first. At least two points to draw anything.
    std::vector<Vec3> vertices;

    /// Group 3, resolved. Null falls back to the drawing's current style.
    ///
    /// A leader takes its arrow size, its text height and its colour rules from
    /// a dimension style, exactly as a dimension does, so a sheet's leaders and
    /// its dimensions cannot drift apart.
    Handle styleHandle;

    /// Group 340. The MTEXT this leader points at, if any.
    Handle annotationHandle;

    /// Group 71. False for a leader drawn with no arrowhead, which is how a
    /// pointer to a surface rather than to an object is drawn.
    bool hasArrowhead = true;

    /// Group 75. The short horizontal run the note sits on.
    ///
    /// other CAD software calls it a hookline and adds it automatically; a leader without
    /// one has its text jammed against the angled segment.
    bool hasLanding = true;

    /// Group 73. 0 MTEXT, 1 tolerance, 2 block reference, 3 none. Kept for
    /// round-trip; only 0 and 3 are produced here.
    int annotationType = 3;

    /// Group 40/41. The height and width the authoring application recorded
    /// for the annotation. Preserved rather than used: the MTEXT carries its
    /// own, and those are the ones that draw.
    double textHeight = 0.0;
    double textWidth = 0.0;

    /// Group 211. Direction the landing runs, as the file recorded it.
    /// Recomputed from the last two vertices when it is not set.
    Vec3 horizontalDirection;
    bool hasHorizontalDirection = false;

    [[nodiscard]] std::string objectType() const override { return "Leader"; }

    /// What to draw, given the style this leader resolves to.
    [[nodiscard]] LeaderGeometry geometry(const DimStyle& style) const;

    [[nodiscard]] Box3 bounds() const override;

    [[nodiscard]] std::vector<CurvePtr> toCurves() const override;

    void transformBy(const Mat4& m) override;

    [[nodiscard]] std::vector<GripPoint> gripPoints() const override;
    void moveGrip(const GripPoint& grip, const Vec3& to) override;

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
    [[nodiscard]] std::vector<Handle> referencedHandles() const override;

    /// The XData application under which a leader Slate made carries its note.
    static constexpr const char* kSlateApplication = "SLATE";

    /// Name the note, and mark the link as this application's own.
    ///
    /// The handle is kept in `annotationHandle` for everything in the engine,
    /// and *also* in this entity's XData under `SLATE`. That second copy is
    /// what goes into the file, and the marker decides how the writer treats
    /// the leader: other CAD software lays out an *associated* note by rules of its own --
    /// it underlines the note for the width of its text, on the side it
    /// believes the text is -- and Slate does not model those, so a leader it
    /// made is written unassociated and the link rides in XData instead. A
    /// leader read from somebody else's file keeps whatever association it came
    /// with; this application must not silently change it.
    void setAnnotation(Handle note);

    /// True when the note link is this application's own (see setAnnotation).
    [[nodiscard]] bool isSlateManaged() const {
        return getXData(kSlateApplication) != nullptr;
    }

    /// Which way the landing runs, and so which side the note sits on.
    ///
    /// The file's own when it supplied one (group 211), derived from the
    /// direction the leader arrives from otherwise -- but *always* known, and
    /// always written. Left out of the file, other CAD software assumes +X and puts every
    /// note on the right whatever way the leader points, so a leader aimed
    /// leftwards had its own text struck through by its landing.
    [[nodiscard]] Vec3 landingDirection() const;

    /// The arrow point, which is the end that means something.
    [[nodiscard]] Vec3 arrowPoint() const {
        return vertices.empty() ? Vec3{} : vertices.front();
    }

    /// Where the note hangs: the far end of the landing.
    [[nodiscard]] Vec3 annotationPoint(const DimStyle& style) const {
        return geometry(style).landingEnd;
    }
};

}  // namespace cad
