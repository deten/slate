#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cad/db/Database.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/HatchEntity.hpp"
#include "cad/db/LeaderEntity.hpp"
#include "cad/db/MLeaderEntity.hpp"
#include "cad/db/RasterImageEntity.hpp"
#include "cad/db/TableEntity.hpp"
#include "cad/db/ViewportEntity.hpp"
#include "cad/render/SpatialIndex.hpp"
#include "cad/render/Viewport.hpp"

namespace cad::render {

/// A tessellated run of connected points, in world coordinates.
///
/// Every curve-backed entity reduces to these. The renderer never sees curve
/// types, entity types or the database — which is what lets a GPU backend and a
/// CPU backend consume exactly the same output, and what keeps the scene
/// testable with no graphics API present.
struct Polyline2D {
    std::vector<Vec3> points;
    bool closed = false;
};

/// How a run of geometry should look, already resolved through the
/// ByLayer/ByBlock chain. No further lookup is required to draw it.
struct DrawStyle {
    std::uint32_t rgb = 0xFFFFFF;
    /// Hundredths of a millimetre; never a negative sentinel.
    std::int16_t lineweight = 25;
    /// 0-255, 255 fully opaque.
    std::uint8_t alpha = 255;
    /// Dash pattern in world units, empty for a solid line.
    std::vector<double> dashPattern;
};

/// One thing to draw.
///
/// A tagged union rather than a class hierarchy: the set of drawable *forms* is
/// small and closed (geometry, text, a marker, a placeholder), even though the
/// set of entity types is open. New entity types produce these same forms.
struct DrawItem {
    enum class Kind : std::uint8_t {
        /// Stroked geometry.
        Geometry,
        /// Filled regions, drawn with the even-odd rule so nested rings cut
        /// holes. Solid hatches, gradient fills and dimension arrowheads all
        /// reduce to this — the alternative, a stroked triangle, looks like a
        /// smudge at arrowhead sizes.
        Filled,
        /// A text run; the backend measures and lays it out.
        Text,
        /// A fixed-pixel-size marker, for POINT entities.
        Marker,
        /// A placeholder for a reference whose content could not be loaded.
        MissingReference,
        /// A raster image, described by `Scene::images[reference]`.
        Image,
    };

    Kind kind = Kind::Geometry;
    Handle handle;
    DrawStyle style;

    /// Fill overlapping rings rather than cancelling them, for Kind::Filled.
    ///
    /// A hatch wants the even-odd rule, so that an island ring inside an outer
    /// one cuts a hole. A widened polyline wants the opposite: its segments
    /// overlap on the inside of every bend, and even-odd turns each overlap
    /// into a hole exactly where the corner should be solid. Filling them as
    /// one path also removes the hairline seams that abutting antialiased
    /// polygons leave between them.
    bool unionFill = false;

    /// Tessellated geometry, for Kind::Geometry and Kind::Filled.
    std::vector<Polyline2D> geometry;

    /// Anchor point, for Text, Marker and MissingReference.
    Vec3 position;

    /// Text payload, for Kind::Text. May contain newlines: MTEXT is
    /// multi-line, and the backend lays the lines out because only it can
    /// measure a font.
    std::string text;
    /// Text height in world units.
    double textHeight = 0.0;
    /// Baseline-to-baseline distance in world units, for multi-line text.
    /// Zero means single-line, and the backend may ignore any newlines.
    double lineSpacing = 0.0;
    /// Text rotation in radians.
    double textRotation = 0.0;
    /// Horizontal and vertical justification, matching the DXF enumerations.
    /// For MTEXT these are its nine-way attachment split into two axes.
    int horizontalAlignment = 0;
    int verticalAlignment = 0;

    /// True when this text is produced by a field rather than typed.
    ///
    /// Carried through so the backend can shade it, as other CAD software does. That
    /// shading is the only thing that tells a drafter a number is derived and
    /// will move on its own — which is exactly what they need to know before
    /// deciding whether to trust it or edit it.
    bool fromField = false;

    /// No side-table payload.
    static constexpr std::size_t kNoReference = static_cast<std::size_t>(-1);

    /// Index into `Scene::images` for Kind::Image, or into `Scene::textRuns`
    /// for a Kind::Text item that carries formatting.
    ///
    /// A side table rather than more fields here. A scene routinely holds half
    /// a million items, of which none or one is an image and almost none are
    /// formatted — so putting either payload in every item would cost the
    /// common case a great deal to spare the rare one an indirection.
    std::size_t reference = kNoReference;

    /// Rectangle this item must be clipped to, in the same coordinates as its
    /// geometry. Set only for items drawn through a paper-space viewport.
    ///
    /// Clipping is expressed here rather than performed here because a backend
    /// can do it far more cheaply than the scene can: a scissor rectangle on a
    /// GPU, a clip region in QPainter. Cutting the geometry ourselves would
    /// throw away that advantage and produce a different result per backend.
    std::optional<Box3> clip;
};

/// A raster image to draw, in world coordinates.
///
/// Expressed as a corner and two edge vectors rather than a transform, because
/// that is what every backend needs and none of them need the same matrix
/// convention. It is stated from the *top* left because that is where a decoded
/// raster's first row is: the entity stores its origin at the bottom left, and
/// resolving that flip once here is better than each backend rediscovering it.
struct SceneImage {
    /// Absolute path to the image file. Empty when it could not be found, which
    /// is the case the backend must draw a placeholder for.
    std::string path;

    /// The path as the drawing stores it, for a placeholder a user can act on.
    /// A message naming a path they recognise is a message they can fix.
    std::string storedPath;

    /// World position of the raster's top-left corner.
    Vec3 topLeft;
    /// World vector spanning the full raster width, left to right.
    Vec3 across;
    /// World vector spanning the full raster height, top to bottom.
    Vec3 down;

    /// Clip boundary in world coordinates; empty when the image is unclipped.
    std::vector<Vec3> clip;
    /// The clip keeps the outside rather than the inside.
    bool clipInverted = false;

    /// 0-100, 50 neutral.
    int brightness = 50;
    int contrast = 50;
    /// 0-100, 0 fully opaque.
    int fade = 0;
};

/// A stretch of a text item drawn with its own font.
///
/// MText is not a string with a font — it is a string with formatting *inside*
/// it, and reducing it to plain text draws a whole note in one size and one
/// colour. Everything here is relative to the item: the height is a factor, and
/// an unset colour means the item's own, so a run survives the entity being
/// restyled.
struct SceneTextRun {
    std::string text;
    double heightFactor = 1.0;
    double widthFactor = 1.0;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool overline = false;
    bool strikethrough = false;
    /// Empty means the item's text style.
    std::string font;
    bool hasColor = false;
    std::uint32_t rgb = 0;
    /// This run is the last on its line.
    bool endsLine = false;
};

/// Everything to draw for one view, plus what it cost to work out.
struct Scene {
    std::vector<DrawItem> items;

    /// Payloads for Kind::Image items, indexed by `DrawItem::reference`.
    std::vector<SceneImage> images;

    /// Formatting for Kind::Text items that have any, indexed the same way.
    ///
    /// Only items that are actually formatted appear here: a plain label is the
    /// overwhelming majority and pays nothing for this.
    std::vector<std::vector<SceneTextRun>> textRuns;

    /// Entities considered before culling.
    std::size_t totalCandidates = 0;
    /// Entities dropped by layer visibility or level of detail.
    std::size_t culled = 0;
    /// Attached content left blank because its outline was suppressed.
    ///
    /// A plot that quietly omits somebody's site photograph is a plot that gets
    /// sent to a client with a hole in it, so the count travels back out with
    /// the scene rather than being dropped on the floor.
    std::size_t placeholdersOmitted = 0;
    /// Time spent building, in milliseconds.
    double buildMilliseconds = 0.0;
};

struct SceneOptions {
    /// Skip entities whose whole extent is smaller than this many pixels.
    double minPixelSize = 1.0;
    /// Maximum block nesting depth to expand.
    int maxDepth = 16;
    /// Cap on items produced, so a pathological drawing cannot stall a frame.
    std::size_t maxItems = 500'000;
    /// Draw a placeholder for references that failed to load.
    bool showMissingReferences = true;

    /// Draw the frame around raster images, as the `IMAGEFRAME` setting in other CAD software does.
    ///
    /// Off by default, matching other CAD software: a frame is an editing aid and it would
    /// otherwise print. An image whose file could not be found is outlined
    /// regardless, under `showContentPlaceholders` below — there is nothing
    /// else to see, and an image that renders as blank space is
    /// indistinguishable from no image at all.
    bool showImageFrames = false;

    /// Outline content this application cannot draw: a missing image, an
    /// underlay's page, an OLE object.
    ///
    /// On for the screen and off for paper. On screen the outline is the only
    /// thing that says something is there; on paper it is a mark that is not in
    /// the drawing, printed onto a deliverable, and other CAD software would not print it.
    /// A plot leaves the space blank and says so instead — see
    /// `Scene::placeholdersOmitted`.
    bool showContentPlaceholders = true;
    /// Cap on hatch pattern segments per entity, per frame.
    ///
    /// Hatch generation is the one part of scene building whose cost is not
    /// bounded by the number of entities: one hatch at a fine pattern scale can
    /// produce more segments than the rest of the drawing put together. The
    /// clip region keeps it proportional to the view, and this keeps it
    /// proportional to something even when the pattern scale is nonsense.
    std::size_t maxHatchSegments = 40'000;
};

/// Builds the list of things to draw for a given view.
///
/// This is where the performance of large drawings is won or lost. Three
/// mechanisms, in the order they matter:
///
///  1. **Spatial culling.** The index is queried for the visible region rather
///     than iterating the drawing. On a 500k-entity site plan zoomed into one
///     room, this alone is the difference between a few frames per second and
///     a smooth one.
///  2. **Layer filtering.** Frozen and off layers are skipped before any
///     geometry work happens.
///  3. **Level of detail.** Entities smaller than a pixel are dropped. Nothing
///     is lost visually, and on a drawing full of small blocks — bolts, tiles,
///     hatching — it removes most of the work at low zoom.
///
/// The scene is rebuilt per frame rather than cached and patched. A deliberate
/// trade: rebuilding is O(visible), and the visible set is bounded by the
/// screen, whereas an incrementally maintained cache must handle every kind of
/// database change correctly and is a reliable source of stale-render bugs.
class SceneBuilder {
public:
    SceneBuilder(Database& db, SpatialIndex& index) : db_(db), index_(index) {}

    /// The index a sheet's viewports look through. The builder's own index
    /// covers whatever space is on screen; when that is a sheet, its
    /// viewports need the *model*, and searching the sheet's index instead
    /// drew the sheet's own frames and notes inside every viewport, at the
    /// viewport's scale. Null means the builder's index is the model's.
    void setModelIndex(const SpatialIndex* index) noexcept { modelIndex_ = index; }

    /// Rebuild the spatial index from a block record's contents.
    ///
    /// Only the top level is indexed; a block reference is indexed by its
    /// overall extents rather than its contents. That keeps the index small — a
    /// drawing with 10,000 references to a 500-entity block indexes 10,000
    /// items, not 5,000,000 — and the contents are expanded only for references
    /// that survive culling.
    void reindex(const BlockTableRecord& space);

    [[nodiscard]] Scene build(const BlockTableRecord& space, const Viewport& viewport,
                              const SceneOptions& options = {});

    /// Build a paper-space sheet: its own geometry, plus the model seen through
    /// each of its viewports.
    ///
    /// A separate entry point rather than a flag on `build`, because the two
    /// walk different things. Model space is walked through the spatial index,
    /// which is what makes a large drawing affordable. A sheet holds a title
    /// block and a handful of viewport frames, so it is walked directly — and
    /// its *viewports* then query the model index for the region each one
    /// shows, which is where the index earns its keep a second time.
    [[nodiscard]] Scene buildLayout(const Layout& layout, const Viewport& viewport,
                                    const SceneOptions& options = {});

private:
    void appendEntity(const Entity& entity, const Mat4& transform,
                      const ResolutionContext& context, const Viewport& viewport,
                      const SceneOptions& options, Scene& scene);

    /// A dimension becomes several items — stroked lines, filled arrowheads and
    /// a text run — because its parts are drawn three different ways. They
    /// share the entity's handle, so selection still treats it as one object.
    void appendDimension(const DimensionEntity& dimension, const Mat4& transform,
                         const DrawStyle& style, const Viewport& viewport, Scene& scene);

    /// A leader: stroked path, filled arrowhead. The note it points at is a
    /// separate entity and draws itself.
    void appendLeader(const LeaderEntity& leader, const Mat4& transform,
                      const DrawStyle& style, Scene& scene);

    /// A multileader: stroked leader lines, filled arrowheads, and its note.
    /// Unlike a leader, the note is carried inside the entity rather than being
    /// a separate MTEXT, so this draws it.
    void appendMLeader(const MLeaderEntity& leader, const Mat4& transform,
                       const DrawStyle& style, Scene& scene);

    void appendHatch(const HatchEntity& hatch, const Mat4& transform, const DrawStyle& style,
                     const Viewport& viewport, const SceneOptions& options, Scene& scene);

    /// A polyline drawn with a width: filled outlines, one per segment.
    void appendWidePolyline(const PolylineEntity& polyline, const Mat4& transform,
                            const DrawItem& prototype, const Viewport& viewport, Scene& scene);

    /// A table: its grid and its cell values, generated rather than replayed.
    void appendTable(const TableEntity& table, const Mat4& transform, const DrawStyle& style,
                     const Viewport& viewport, Scene& scene);

    /// A raster image: the picture, and a frame when there is no picture.
    void appendImage(const RasterImageEntity& image, const Mat4& transform,
                     const DrawStyle& style, const Viewport& viewport,
                     const SceneOptions& options, Scene& scene);

    /// The outline of something whose contents this application cannot draw —
    /// an underlay's page, an OLE object's frame.
    ///
    /// Dashed, and captioned when there is room for the caption. A solid
    /// rectangle would be indistinguishable from a rectangle somebody drew, and
    /// the one thing the user needs to know is that there is content here that
    /// they are not being shown.
    void appendPlaceholderFrame(const Entity& entity, const std::vector<Vec3>& outline,
                                const std::string& caption, const Mat4& transform,
                                const DrawStyle& style, const Viewport& viewport,
                                const SceneOptions& options, Scene& scene);

    /// Draw the model through one viewport, onto the sheet.
    void appendViewportContents(const ViewportEntity& window, const Viewport& viewport,
                                const SceneOptions& options, Scene& scene);

    /// Expand a block reference into draw items for its contents.
    ///
    /// The reference's own extents are checked against the view before any
    /// contents are touched, so an off-screen block costs one box test rather
    /// than a full traversal.
    void expandInsert(const Entity& insert, const Mat4& parentTransform,
                      const Viewport& viewport, const SceneOptions& options, int depth,
                      Scene& scene);

    [[nodiscard]] DrawStyle resolveStyle(const ResolvedProperties& resolved) const;

    /// The resolution context for an entity's layer, cached per build.
    ///
    /// The answer depends only on the layer, and a drawing has tens of layers
    /// and hundreds of thousands of entities. Resolving it per entity is a
    /// layer lookup and a linetype lookup repeated for no reason — measurable
    /// at scale and invisible at small sizes, which is the worst combination.
    [[nodiscard]] const ResolutionContext& contextFor(const Entity& entity);

    Database& db_;
    SpatialIndex& index_;
    const SpatialIndex* modelIndex_ = nullptr;

    /// Cleared at the start of every build, because a layer's colour may have
    /// changed since the last frame — and a stale cache here would mean the
    /// layer manager appeared not to work.
    std::unordered_map<Handle, ResolutionContext> contextCache_;

    /// How many block references deep the entity being appended sits. Zero
    /// in the open; an attribute definition shows its tag there and its
    /// value inside a reference.
    int insertDepth_ = 0;
};

/// Extents of a space, for ZOOM EXTENTS.
///
/// Uses the index when populated, since that is already the union of every
/// indexed entity's bounds and avoids a second traversal.
[[nodiscard]] Box3 spaceExtents(Database& db, const BlockTableRecord& space,
                                const SpatialIndex* index = nullptr);

}  // namespace cad::render
