#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "cad/db/Entity.hpp"

namespace cad {

/// Which kind of document an underlay shows.
enum class UnderlayKind : std::uint8_t {
    Pdf,
    Dwf,
    Dgn,
};

[[nodiscard]] std::string_view underlayKindName(UnderlayKind kind) noexcept;

/// A document a drawing traces over — the PDFDEFINITION / DWFDEFINITION /
/// DGNDEFINITION object.
///
/// The same relationship as an image definition to its placements, and named
/// separately because the thing being placed is a *page*, not a raster: the
/// item name is a page number in a PDF and a model name in a DGN.
class UnderlayDefinition final : public DbObject {
public:
    UnderlayKind kind = UnderlayKind::Pdf;

    /// The path exactly as the file stores it; never rewritten on resolution.
    std::string fileName;

    /// Which page or model within that file, from group 2.
    std::string itemName;

    /// Where the file was found, filled in after resolution. Runtime state.
    std::string resolvedPath;
    std::string resolutionProblem;

    [[nodiscard]] std::string objectType() const override { return "UnderlayDefinition"; }

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
};

/// A placed underlay — the PDFUNDERLAY / DWFUNDERLAY / DGNUNDERLAY entity.
///
/// An architect traces over a consultant's PDF constantly, and the entity that
/// results is a first-class part of the drawing: it is on a layer, it is
/// selected, moved and clipped, and it plots. All of that is here.
///
/// What is *not* here is the page's contents. Rasterising a PDF needs a PDF
/// engine, which the core does not have and should not grow; the entity carries
/// the placement and the reference, and a backend that has a rasteriser can
/// draw through it. Until one does, an underlay shows as its outline, which is
/// the honest picture: something is here, this is exactly where it is, and its
/// contents could not be drawn.
class UnderlayEntity final : public Entity {
public:
    UnderlayKind kind = UnderlayKind::Pdf;

    /// The definition this places (group 340).
    Handle definitionHandle;

    /// Insertion point in OCS (group 10).
    Vec3 position;

    /// Per-axis scale (groups 41/42/43).
    Vec3 scale{1.0, 1.0, 1.0};

    /// Rotation about the normal, in radians (group 50).
    double rotation = 0.0;

    // Flags, group 280.
    bool clippingEnabled = false;
    bool underlayOn = true;
    bool monochrome = false;
    bool adjustForBackground = false;

    /// 0-100, 100 is full contrast (group 281).
    int contrast = 100;
    /// 0-100, 0 is unfaded (group 282).
    int fade = 0;

    /// Clip boundary vertices in OCS (groups 11/21). Two points mean a
    /// rectangle by its opposite corners, more mean a polygon.
    std::vector<Vec3> clipVertices;

    /// The page's size in drawing units, when something has worked it out.
    ///
    /// Not stored in DXF — it comes from the underlay file, which only a
    /// backend with a decoder can open. Zero means unknown, and everything here
    /// falls back to the clip boundary in that case rather than guessing at a
    /// page size and drawing a box in the wrong place.
    Vec3 pageSize;

    /// "PdfUnderlay", "DwfUnderlay" or "DgnUnderlay".
    ///
    /// One type per kind rather than a single "Underlay", because the type is
    /// what the DXF writer's registry is keyed on and what the properties
    /// palette shows. A user looking at a selection wants to be told they have
    /// a PDF, and a lookup table that has to be told twice — once for the name,
    /// once for the kind — is a lookup table that will one day disagree.
    [[nodiscard]] std::string objectType() const override;

    /// Transform from the underlay's own page space into the drawing.
    [[nodiscard]] Mat4 placementTransform() const;

    /// The outline actually drawn: the clip boundary when one is in force, the
    /// page rectangle when the page size is known, otherwise empty.
    [[nodiscard]] std::vector<Vec3> displayOutline() const;

    [[nodiscard]] Box3 bounds() const override;
    void transformBy(const Mat4& m) override;
    [[nodiscard]] std::vector<CurvePtr> toCurves() const override;
    [[nodiscard]] double distanceToPoint(const Vec3& p) const override;
    [[nodiscard]] std::vector<GripPoint> gripPoints() const override;
    void moveGrip(const GripPoint& grip, const Vec3& to) override;

    [[nodiscard]] std::vector<Handle> referencedHandles() const override;

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
};

}  // namespace cad
