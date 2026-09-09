#pragma once

#include <span>
#include <vector>

#include "cad/db/Color.hpp"
#include "cad/db/DbObject.hpp"
#include "cad/geom/Curve.hpp"
#include "cad/math/Box3.hpp"
#include "cad/math/Mat4.hpp"
#include "cad/math/Vec3.hpp"

namespace cad {

/// Fully resolved display properties for an entity.
///
/// Produced by walking the ByLayer/ByBlock inheritance chain. The renderer
/// consumes this and never inspects raw entity properties, which keeps
/// inheritance logic in exactly one place.
struct ResolvedProperties {
    std::uint32_t rgb = 0xFFFFFF;
    Handle linetypeHandle;
    double linetypeScale = 1.0;
    /// Hundredths of a millimetre; never a negative sentinel.
    std::int16_t lineweight = 25;
    /// 0-255, 255 fully opaque.
    std::uint8_t alpha = 255;
    bool visible = true;
};

/// The context an entity's properties resolve against.
///
/// When an entity is drawn inside a block reference, ByBlock properties come
/// from the *reference*, not the block definition — so the same block can look
/// different at two insertion points. Passing this explicitly, rather than
/// storing resolved values on the entity, is what makes that work.
struct ResolutionContext {
    Color layerColor = Color::fromIndex(7);
    Handle layerLinetypeHandle;
    std::int16_t layerLineweight = toRaw(Lineweight::Default);
    Transparency layerTransparency;
    bool layerVisible = true;

    /// Properties of the enclosing block reference, when nested.
    bool insideBlock = false;
    Color blockColor = Color::byLayer();
    Handle blockLinetypeHandle;
    std::int16_t blockLineweight = toRaw(Lineweight::ByLayer);

    /// Global LTSCALE system variable.
    double globalLinetypeScale = 1.0;
};

/// Grip points for an entity — the small squares a user drags to edit geometry
/// directly, without invoking a command.
enum class GripKind : std::uint8_t {
    Endpoint,
    Midpoint,
    Center,
    Vertex,
    Quadrant,
    Insertion,
};

struct GripPoint {
    Vec3 position;
    GripKind kind = GripKind::Endpoint;
    /// Index within a repeated set, e.g. the vertex number on a polyline.
    std::size_t index = 0;
};

struct GripMove {
    GripPoint grip;
    Vec3 to;
};

/// Base class for every drawable object.
///
/// Entities hold *presentation* state (layer, colour, linetype) and *geometric*
/// state defined by the subclass. The split matters: presentation is uniform
/// across all entity types and handled here once, while geometry varies and is
/// delegated.
///
/// Every entity can expose its geometry as curves via `toCurves`. That single
/// method is what lets snapping, trimming, filleting, offsetting and selection
/// work on *any* entity — including ones added later by a plugin — without
/// those systems knowing the concrete type.
class Entity : public DbObject {
public:
    /// Handle of the layer this entity belongs to.
    Handle layerHandle;

    /// Entity colour, usually ByLayer.
    Color color = Color::byLayer();

    /// Handle of the linetype record; the null handle means ByLayer.
    Handle linetypeHandle;

    /// Per-entity linetype scale, multiplied with the global LTSCALE.
    double linetypeScale = 1.0;

    /// Lineweight, or one of the negative Lineweight sentinels.
    std::int16_t lineweight = toRaw(Lineweight::ByLayer);

    /// Per-entity transparency; empty inherits from the layer.
    Transparency transparency;

    /// Explicitly hidden, independent of layer visibility.
    bool visible = true;

    /// Extrusion direction defining this entity's OCS. Almost always +Z; a
    /// different value means the entity was drawn on a rotated or mirrored UCS
    /// and its stored coordinates are not world coordinates.
    Vec3 normal = Vec3::unitZ();

    /// Elevation along the normal. DWG stores this separately from the
    /// entity's points for planar entities.
    double elevation = 0.0;

    /// Handle of the material, preserved for fidelity.
    Handle materialHandle;

    /// Axis-aligned world-space extents.
    ///
    /// Must be tight enough for spatial indexing to be effective, but may be
    /// conservative where an exact bound is expensive (text before fonts are
    /// measured, for instance).
    [[nodiscard]] virtual Box3 bounds() const = 0;

    /// Apply a transform to this entity's geometry, in place.
    ///
    /// In place because MOVE on a 50,000-entity selection must not allocate
    /// 50,000 replacement entities; the transaction layer has already
    /// snapshotted anything that needs undoing.
    virtual void transformBy(const Mat4& m) = 0;

    /// This entity's geometry as curves, in world coordinates.
    ///
    /// Entities with no curve geometry (text, points, block references before
    /// explosion) return an empty vector. Entities made of several curves — a
    /// polyline is one, a dimension is many — return all of them.
    [[nodiscard]] virtual std::vector<CurvePtr> toCurves() const { return {}; }

    /// True for an entity no box holds -- a construction line -- so the
    /// extents of a drawing leave it out. Its `bounds()` is then the part
    /// of it that is drawn, for picking, not its size.
    [[nodiscard]] virtual bool isUnbounded() const { return false; }

    /// Shortest distance from a world point to this entity's geometry, or
    /// infinity when the entity cannot be picked geometrically.
    ///
    /// The default walks `toCurves`, which is correct for every curve-backed
    /// entity; types with special pick behaviour (text and hatches, picked by
    /// their area) override it.
    [[nodiscard]] virtual double distanceToPoint(const Vec3& p) const;

    /// False when this entity cannot be moved, rotated or scaled.
    ///
    /// True for everything with geometry of its own. False for the two kinds
    /// that are written back from preserved bytes rather than from what this
    /// application understands: an entity type with no reader at all, and a
    /// multileader. Transforming either changes what is on the screen and not
    /// what a save emits, so the drawing and the file disagree -- and the file
    /// wins. Commands ask before transforming and say what they skipped, which
    /// is the difference between a refusal and a silent no-op.
    [[nodiscard]] virtual bool canTransform() const { return true; }

    /// Grip points, for direct editing without a command.
    [[nodiscard]] virtual std::vector<GripPoint> gripPoints() const { return {}; }

    /// Move one grip to a new position, mutating the entity.
    /// The default does nothing, for entity types without grip support.
    virtual void moveGrip(const GripPoint& /*grip*/, const Vec3& /*to*/) {}

    /// Move several grips as one edit.
    ///
    /// The default applies them in the order given, re-finding each grip by
    /// position first: an edit can renumber the grips after it, and a polyline
    /// midpoint grip inserts a vertex, which does exactly that.
    ///
    /// Shapes whose grips are not independent override this. An arc's start,
    /// middle and end all share one centre and one radius, so moving them one
    /// after another leaves each undoing part of the last -- two ends dragged
    /// together by the same amount came out a unit apart, and the arc's centre
    /// wandered. Given the whole set at once, such a shape can solve for the
    /// result the grips describe rather than converging on it.
    virtual void moveGrips(std::span<const GripMove> moves);

    /// Resolve display properties through the ByLayer/ByBlock chain.
    ///
    /// Resolution order for each property is: the explicit entity value, then
    /// the block reference's value when the entity value is ByBlock, then the
    /// layer's value when it is ByLayer. This mirrors other CAD software exactly,
    /// including the subtlety that ByBlock outside a block reference falls back
    /// to the layer.
    [[nodiscard]] ResolvedProperties resolveProperties(const ResolutionContext& context) const;

    [[nodiscard]] std::vector<Handle> referencedHandles() const override;

protected:
    Entity() = default;
    Entity(const Entity&) = default;
    Entity& operator=(const Entity&) = default;
    Entity(Entity&&) = default;
    Entity& operator=(Entity&&) = default;

    void copyEntityInto(Entity& target) const;
    void restoreEntityFrom(const Entity& snapshot);

private:
    [[nodiscard]] Color resolveColor(const ResolutionContext& context) const;
    [[nodiscard]] Handle resolveLinetype(const ResolutionContext& context) const;
    [[nodiscard]] std::int16_t resolveLineweight(const ResolutionContext& context) const;
    [[nodiscard]] std::uint8_t resolveAlpha(const ResolutionContext& context) const;
};

}  // namespace cad
