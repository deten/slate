#pragma once

#include <string>
#include <vector>

#include "cad/db/Entity.hpp"

namespace cad {

/// A raster file a drawing refers to — the IMAGEDEF object.
///
/// One definition, many placements: the same scan can be inserted on six sheets
/// and the file is named once. That is the same relationship a block record has
/// with its inserts, and it exists for the same reason — a path stored six
/// times is a path that gets fixed five times.
class ImageDefinition final : public DbObject {
public:
    /// The path exactly as the file stores it.
    ///
    /// Never rewritten as a side effect of anything. A drawing authored with a
    /// relative path must stay relative when saved, or the project folder stops
    /// being portable for everybody else who opens it.
    std::string fileName;

    /// Size of the raster in pixels.
    Vec3 sizeInPixels{0.0, 0.0, 0.0};

    /// Size of one pixel in the units the image file declares. Not the size it
    /// is drawn at — that comes from the placement, not from here.
    Vec3 pixelSize{1.0, 1.0, 0.0};

    /// Whether the authoring application had the image loaded.
    bool loaded = true;

    /// 0 = none, 2 = centimetres, 5 = inches.
    int resolutionUnits = 0;

    /// Where the file was actually found, filled in after resolution. Runtime
    /// state: never written to a file, because the stored path is what the file
    /// is entitled to keep.
    std::string resolvedPath;

    /// Why it was not found, when it was not. Shown to the user rather than
    /// logged, because a broken image path is a thing only they can fix.
    std::string resolutionProblem;

    [[nodiscard]] std::string objectType() const override { return "ImageDefinition"; }

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
};

/// A placed raster image — the IMAGE entity.
///
/// The geometry is a parallelogram, not a rectangle, and that is worth stating
/// plainly because it is the source of every mistake made with this entity. DXF
/// stores an origin and two vectors, each spanning *one pixel*, so an image
/// rotated on a UCS or mirrored by a negative scale is represented exactly and
/// without a separate rotation field. Multiply by the pixel counts to get the
/// corners; do not assume the corners are axis-aligned.
///
/// The image data itself is deliberately absent. The core decodes no PNGs: that
/// would drag an image library into a layer that is meant to be the standard
/// library and nothing else. The entity says *where* and *how bright*, the
/// backend that owns a decoder says *what*.
class RasterImageEntity final : public Entity {
public:
    /// The IMAGEDEF this places (group 340).
    Handle definitionHandle;

    /// The IMAGEDEF_REACTOR that lets the definition find its placements
    /// (group 360). Kept so a save does not orphan the definition's reactor
    /// list, which is how other CAD software knows an image is still in use.
    Handle reactorHandle;

    /// World position of the image's lower-left corner (group 10).
    Vec3 origin;

    /// World vector spanning one pixel across the image (group 11).
    Vec3 uVector{1.0, 0.0, 0.0};

    /// World vector spanning one pixel up the image (group 12).
    ///
    /// Up, in image terms, is the *bottom* row of the raster: a decoder hands
    /// back row 0 at the top, and every renderer of this entity has to flip.
    Vec3 vVector{0.0, 1.0, 0.0};

    /// Image size in pixels (groups 13/23), as the placement understood it.
    double widthPixels = 0.0;
    double heightPixels = 0.0;

    // Display flags, group 70.
    bool showImage = true;
    bool showWhenNotAligned = true;
    bool useClippingBoundary = true;
    bool transparent = false;

    /// Whether the clip boundary is in force (group 280).
    bool clippingEnabled = false;

    /// 0-100, 50 is neutral (groups 281/282).
    int brightness = 50;
    int contrast = 50;
    /// 0-100, 0 is fully opaque (group 283).
    int fade = 0;

    /// 1 = rectangular (two opposite corners), 2 = polygonal (group 71).
    int clipBoundaryType = 1;

    /// Clip boundary, in image pixel coordinates measured from the lower-left
    /// corner of the lower-left pixel — so the whole image is the rectangle
    /// from (-0.5, -0.5) to (width - 0.5, height - 0.5), which is exactly what
    /// other CAD software writes for an unclipped image.
    std::vector<Vec3> clipVertices;

    /// Clip keeps the outside rather than the inside (group 290).
    bool clipInverted = false;

    [[nodiscard]] std::string objectType() const override { return "Image"; }

    /// The four world corners, counter-clockwise from the origin.
    [[nodiscard]] std::vector<Vec3> corners() const;

    /// The clip boundary in world coordinates, empty when there is none in
    /// force. A rectangular boundary is expanded to its four corners here, so
    /// no consumer has to know which of the two forms it was stored in.
    [[nodiscard]] std::vector<Vec3> worldClipBoundary() const;

    /// The outline actually drawn: the clip boundary when one is in force,
    /// otherwise the frame.
    [[nodiscard]] std::vector<Vec3> displayOutline() const;

    [[nodiscard]] Box3 bounds() const override;
    void transformBy(const Mat4& m) override;
    [[nodiscard]] std::vector<CurvePtr> toCurves() const override;

    /// Zero inside the outline, and the distance to its edge outside it.
    ///
    /// Picked by its area, like a solid hatch, because that is what a user
    /// expects of something that looks like a filled region — clicking the
    /// middle of a photograph must select the photograph.
    [[nodiscard]] double distanceToPoint(const Vec3& p) const override;

    [[nodiscard]] std::vector<GripPoint> gripPoints() const override;
    void moveGrip(const GripPoint& grip, const Vec3& to) override;

    [[nodiscard]] std::vector<Handle> referencedHandles() const override;

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
};

}  // namespace cad
