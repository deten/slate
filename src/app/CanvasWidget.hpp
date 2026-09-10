#pragma once

#include <array>
#include <unordered_map>

#include <QStringList>
#include <QBasicTimer>
#include <QTimer>
#include <QWidget>
#include <deque>
#include <memory>
#include <optional>

#include "Document.hpp"
#include "cad/db/ViewportEntity.hpp"
#include "cad/math/Mat4.hpp"
#include "DynamicInput.hpp"
#include "ImageCache.hpp"
#include "InPlaceTextEditor.hpp"
#include "cad/render/Scene.hpp"
#include "cad/render/Viewport.hpp"
#include "cad/snap/ObjectSnap.hpp"
#include "cad/snap/Tracking.hpp"

namespace cad {
class InsertEntity;
}  // namespace cad

namespace cad::app {

/// Snaps running out of the box.
///
/// Chosen for what a drafter wants on permanently rather than for completeness.
/// Tangent and Parallel need a base point and are situational, so they are
/// reached for deliberately. Nearest is left off because it matches *everywhere*
/// on an object: with it running, a point near a line can never be placed
/// freely, which is a common complaint about over-eager snapping. All of them
/// are one click away in the status bar.
inline constexpr std::uint32_t kDefaultSnapModes =
    toMask(SnapMode::Endpoint) | toMask(SnapMode::Midpoint) | toMask(SnapMode::Center) |
    toMask(SnapMode::Node) | toMask(SnapMode::Quadrant) | toMask(SnapMode::Intersection) |
    toMask(SnapMode::Perpendicular) | toMask(SnapMode::Extension);

/// The drawing canvas: rendering, navigation, selection and command input.
///
/// Renders with QPainter — an *interim* backend isolated here in the app layer
/// so it can be replaced cleanly (ADR 0003). Nothing below this widget knows Qt
/// exists, and the scene it draws is the same backend-agnostic
/// `cad::render::Scene` a GPU backend will consume.
///
/// The widget owns the *view*, never the document. It reads the database and
/// writes to it only through the command executor, so several canvases can show
/// one drawing correctly.
class CanvasWidget final : public QWidget {
    Q_OBJECT

public:
    explicit CanvasWidget(QWidget* parent = nullptr);
    ~CanvasWidget() override;

    /// Point the canvas at a document. Null clears it.
    void setDocument(Document* document);

    /// Rebuild the spatial index and fit the drawing to the window.
    void reload();

    void zoomExtents();
    /// Zoom to the selected objects, or to the extents when nothing is
    /// selected. What a middle double-click does.
    void zoomToSelection();
    /// Acquire the snap point the pointer is resting on now, without the
    /// wait. A scripted hover is a deliberate rest.
    void settleTracking();

    /// Frame a world-space box, as ZOOM Window does.
    void zoomToBox(const Box3& box);

    /// Shift the view by a world-space displacement, as PAN does.
    void panBy(const Vec3& displacement);

    // ---------------------------------------------------- active viewport
    // On a sheet, one viewport can be stepped into: picks, points, snaps and
    // the coordinates read out are then in model space through that window,
    // and zoom and pan move the model behind its frame.

    /// Screen to the coordinates commands work in: paper on a sheet, model
    /// through the active viewport.
    [[nodiscard]] Vec3 toWorld(const render::ScreenPoint& screen) const;
    [[nodiscard]] render::ScreenPoint toScreen(const Vec3& world) const;

    [[nodiscard]] bool viewportActive() const;
    bool activateViewport(Handle handle);
    /// The one model viewport on the sheet, when there is exactly one.
    bool activateSoleViewport();
    bool activateViewportAt(const render::ScreenPoint& screen);
    void exitViewport();
    /// Show a model region through the active viewport.
    void viewWindow(const Box3& modelBox);

    /// Restore the view from before the last command-driven change.
    ///
    /// Only command-driven changes are remembered. Recording every wheel notch
    /// would make "previous" mean one click back, which is not what a drafter
    /// reaching for it wants — they want the view they were working in before
    /// they went to look at something.
    /// @returns false when there is nothing to go back to.
    bool zoomPrevious();

    [[nodiscard]] const render::Viewport& viewport() const noexcept { return viewport_; }

    /// Restore a view saved earlier, when switching back to a drawing.
    ///
    /// The window size is re-applied rather than restored: the saved view came
    /// from whatever size the window was then, and the drawing has to fit the
    /// one it has now.
    void setViewport(const render::Viewport& view);

    /// Statistics from the last frame, for the status bar.
    [[nodiscard]] std::size_t lastVisibleCount() const noexcept { return lastVisible_; }
    [[nodiscard]] double lastBuildMilliseconds() const noexcept { return lastBuildMs_; }
    [[nodiscard]] double lastPaintMilliseconds() const noexcept { return lastPaintMs_; }

    /// Running object snap modes, as an OSMODE bitmask.
    void setSnapModes(std::uint32_t modes);
    [[nodiscard]] std::uint32_t snapModes() const noexcept { return snapModes_; }

    /// Use this snap for the next point only, whatever is running.
    ///
    /// other CAD software's snap override: the answer to "that endpoint, not whatever is
    /// nearest". It lasts one point and then the running snaps resume, which is
    /// what makes it usable mid-command without having to put anything back.
    /// `SnapMode::None` overrides with *nothing*, for placing a point free of
    /// snapping in a crowded drawing.
    void setSnapOverride(SnapMode mode);

    /// How close a click has to be, in pixels, to catch something.
    ///
    /// Worth exposing: the right value depends on the pointing device and the
    /// eyesight of the person using it. Too small and picking a line feels like
    /// a test of aim; too large and dense geometry becomes ambiguous.
    void setAperturePixels(double pixels);
    [[nodiscard]] double aperturePixels() const noexcept { return aperturePixels_; }

    /// Outline the object under the cursor before a click.
    void setRolloverHighlight(bool enabled);
    [[nodiscard]] bool rolloverHighlight() const noexcept { return rolloverHighlight_; }

    /// Crosshair arm length as a percentage of the window, as other CAD software's
    /// CURSORSIZE does it. 100 spans the whole drawing area, which some
    /// drafters prefer for lining work up across a sheet.
    void setCrosshairPercent(double percent);
    [[nodiscard]] double crosshairPercent() const noexcept { return crosshairPercent_; }

    /// Height in pixels below which text is drawn as a bar rather than glyphs.
    ///
    /// Set it to 1 to always render real characters, however small; raise it
    /// to greek sooner on a large drawing where the annotation is dense.
    void setTextGreekingPixels(double pixels);

    /// Draw the frame around raster images, as the `IMAGEFRAME` setting in other CAD software does.
    void setShowImageFrames(bool show);

    /// Show, move or hide the in-place text editor to match what the running
    /// command is asking for. Called whenever the command state changes.
    void syncInPlaceEditor();

    /// Step to the next grip mode, as the space bar does in other CAD software.
    void cycleGripMode();

    /// The mode's name for the prompt -- `STRETCH`, `MOVE` -- or empty when no
    /// grip is held.
    [[nodiscard]] QString gripModeName() const;

    /// The grip modes by name, in the order the Space bar cycles them.
    ///
    /// The menu names all five rather than offering "next", because picking
    /// Rotate out of a list is one gesture and cycling to it is three -- and
    /// the drafter has to know the order to count.
    [[nodiscard]] static QStringList gripModes();

    /// The mode a held grip is in, undecorated, for matching against
    /// `gripModes()`. `gripModeName` adds what the prompt needs and so cannot
    /// be compared against.
    [[nodiscard]] QString currentGripMode() const;

    /// Switch a held grip to a named mode. An unknown name is ignored.
    void setGripMode(const QString& name);

    /// Apply one of the grip prompt's options by name: `Copy`, `Base point`,
    /// `Undo`, `eXit`. Shared by the typed prompt and the context menu, so the
    /// two cannot offer different things.
    void applyGripOption(const QString& option);

    /// Finish a grip edit from something typed, rather than from a click.
    ///
    /// The other CAD software reflex: grab a grip, point the way you want it to go, type
    /// the distance, press Enter. Accepts what that habit produces --
    /// `250` for a distance along the direction the pointer is indicating,
    /// `@dx,dy` for a relative move, `x,y` for an absolute point.
    ///
    /// @returns false when there is no grip held or the text is not one of
    ///   those, so the command line can go on to treat it as a command.
    bool supplyGripInput(const QString& text);

    /// Whether points are being constrained to the horizontal and vertical.
    ///
    /// other CAD software's ORTHO, on F8. The drawing carries the setting because it is a
    /// drafting mode of the document, like the running snaps, and a drafter who
    /// leaves it on expects it on when they come back.
    [[nodiscard]] bool orthoEnabled() const;
    void setOrthoEnabled(bool enabled);

    /// Whether points are being pulled onto regular angles from their origin.
    ///
    /// other CAD software's POLAR, on F10, and the counterpart to ortho: ortho allows two
    /// directions and polar allows every multiple of an increment. Turning one
    /// on turns the other off, because a point cannot be constrained twice and
    /// other CAD software makes the same choice.
    [[nodiscard]] bool polarEnabled() const;
    void setPolarEnabled(bool enabled);

    /// The polar increment in radians, as the drawing has it.
    [[nodiscard]] double polarIncrement() const;

    /// Dimensional input: the boxes at the cursor that take an exact size.
    ///
    /// other CAD software's dynamic input. While a command is asking for a point, the
    /// distance and angle from the point it is rubber-banding from are shown
    /// beside the crosshair and can be typed into directly. Tab locks the field
    /// being left and moves to the other, so "250, Tab, 30, Enter" places a
    /// point exactly 250 away at exactly 30 degrees without the drafter
    /// looking away from the geometry.
    ///
    /// Which two fields depends on whether there is anything to measure from:
    /// a distance and an angle when there is, absolute X and Y when there is
    /// not, because the first point of a line has no length yet.
    enum class DimensionField : std::uint8_t { First, Second };

    /// True when a point is being asked for and the boxes are up.
    [[nodiscard]] bool isEnteringDimension() const;

    /// Lock the active field at `typed` -- or at its live value when nothing
    /// was typed -- and move to the other. Returns false when no point is
    /// being entered, so Tab falls through to whatever it usually does.
    bool advanceDimensionField(const QString& typed);

    /// Take typed text as the answer to a point prompt, using whatever fields
    /// are locked. False when it is not dimensional input, leaving the command
    /// line to parse it as it always has.
    bool supplyDimensionInput(const QString& text);

    /// Mirror the command line into the active box, so it shows what is being
    /// typed into it.
    void setDimensionTyped(const QString& text);

    /// Forget any locked field. Called when a point is supplied or a command
    /// ends, so the next point does not inherit the last one's constraint.
    void clearDimensionInput();

    /// What the boxes currently read, for the tests and the status bar.
    [[nodiscard]] QString dimensionSummary() const;

    /// Object snap tracking -- other CAD software's F11.
    [[nodiscard]] bool trackingEnabled() const;
    void setTrackingEnabled(bool enabled);

    /// True while a grip is being edited, by drag or by click.
    ///
    /// Asked by the window, which must not put a command prompt under the
    /// cursor while the grip readout is there.
    [[nodiscard]] bool isEditingGrip() const noexcept { return gripDrag_.has_value(); }

    /// Back out of a grip edit, or of the grips marked for one.
    ///
    /// Escape unwinds one step at a time, the way it does in other CAD software: an edit
    /// in progress is abandoned first and the grips stay marked, so a mis-aimed
    /// drag costs the drag and not the picking that set it up. Returns false
    /// when there was nothing to back out of, leaving Escape to whoever wants
    /// it next.
    bool cancelGripEdit();

    /// True while the in-place text editor is open.
    [[nodiscard]] bool isEditingText() const;

    /// Forget every decoded image. Needed when a reference is repointed: the
    /// cache is keyed on the path, and the old file is still in it.
    void clearImageCache() { imageCache_.clear(); }
    [[nodiscard]] bool showImageFrames() const noexcept { return showImageFrames_; }
    [[nodiscard]] double textGreekingPixels() const noexcept { return textGreekingPixels_; }

    /// Step to the next object under the cursor, when several overlap.
    ///
    /// Bound to Shift+Space, as other CAD software binds it. Overlapping geometry is the
    /// normal state of a real drawing — a wall line under a hatch boundary
    /// under a viewport edge — and without this the topmost object is the only
    /// one reachable by clicking.
    void cycleHovered();

    /// The hovered object as a person would name it: type and layer, e.g.
    /// "Line on A-WALL". Two overlapping lines are indistinguishable by
    /// highlight alone, so cycling needs words.
    [[nodiscard]] QString hoveredDescription() const;

    /// How many objects are stacked under the cursor.
    [[nodiscard]] std::size_t hoverCandidateCount() const noexcept {
        return hoverCandidates_.size();
    }

    /// Show or hide the prompt that follows the cursor.
    ///
    /// Driven by the window from the command line's state, so there is one
    /// buffer rather than two that can disagree.
    void showDynamicInput(const QString& prompt, const QString& text);
    void hideDynamicInput();

    /// Where the cursor-following prompt should appear: at the crosshair, or
    /// at the bottom-left of the canvas when the cursor is elsewhere.
    [[nodiscard]] QPoint promptAnchor() const;

Q_SIGNALS:
    /// Emitted as the cursor moves, in world coordinates, for the status bar.
    void cursorMoved(double worldX, double worldY);

    /// Emitted whenever the view changes, so the status bar can show the zoom.
    void viewChanged();

    /// Emitted when the selection changes, so panels can follow it.
    void selectionChanged();

    /// A block reference was double-clicked; its definition should be opened.
    ///
    /// Reported rather than acted on, because opening a block changes what the
    /// tab bar, the title and the panels are describing, and the canvas is not
    /// the thing that knows about any of them.
    void blockEditRequested(Handle blockRecord);

    /// The right button was held rather than clicked: show what can be done
    /// here. The point is in global coordinates, ready for a menu.
    void contextMenuRequested(const QPoint& globalPosition);

    /// Shift with the right button: offer the object snaps for the next point.
    void snapOverrideRequested(const QPoint& globalPosition);

    /// Emitted when a command's state changes, so the prompt can be shown.
    void commandStateChanged();

    /// Emitted while cycling overlapping objects, with the one-based position
    /// and the total, so the shell can say "2 of 4" somewhere visible.
    void hoverCycled(int index, int total);


protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void timerEvent(QTimerEvent* event) override;

private:
    void syncViewportSize();

    /// Convert a widget position to world coordinates, applying any snap.
    [[nodiscard]] Vec3 worldAt(const QPointF& position) const;

    /// The world point a click or a move means, with ortho applied.
    ///
    /// Separate from `worldAt` because ortho needs to know what the point is
    /// being measured *from*: the running command's base point, or the grip
    /// being held. A constraint with no origin is not a constraint.
    [[nodiscard]] Vec3 constrainedWorldAt(const QPointF& position) const;

    /// Where the current point is being measured from, if from anywhere.
    [[nodiscard]] std::optional<Vec3> constraintOrigin() const;

    /// The two numbers the boxes would show if nothing were typed or locked.
    [[nodiscard]] std::array<double, 2> liveDimensions() const;

    /// The cursor pulled onto an alignment path from an acquired point, if it
    /// is near one. Records the path in `trackingHit_` for drawing.
    [[nodiscard]] std::optional<Vec3> alignedToTracking(const Vec3& world) const;

    /// Remember a snap point as an origin to align to later.
    void acquireTrackingPoint(const Vec3& point);

    /// Recompute the active snap for a cursor position.
    void updateSnap(const QPointF& position);

    /// Send a picked point to the running command.
    void supplyPoint(const Vec3& world);

    /// Send the entity under the cursor to a command that asked for one.
    void supplyEntityAt(const QPointF& position);

    /// Hand the current selection to a command that asked for a set.
    void supplyCurrentSelection();

    /// Handle a click that builds the selection, whether or not a command is
    /// running: a command asking for a set uses the same picking behaviour.
    void handleSelectionClick(const QPointF& position, bool subtractive);

    // ------------------------------------------------------------------ grips
    //
    // Direct editing: drag a square on a selected entity and its geometry
    // follows. The whole interaction lives here rather than in a command,
    // because there is no prompt sequence — it is a press, a move and a
    // release, and routing it through the executor would mean inventing a
    // command that exists only to be interrupted.

    /// A grip being dragged, and the clone showing where it would end up.
    /// What a held grip does with the point it is taken to.
    ///
    /// other CAD software's grip modes, in other CAD software's order, stepped with the space bar.
    /// Every one after the first is a transform about the grip, which is why
    /// they cost so little to offer and why not offering them would be the
    /// odd choice: a drafter who holds a grip and presses space expects the
    /// next one, and getting nothing is a dead end in a familiar road.
    enum class GripMode : std::uint8_t { Stretch, Move, Rotate, Scale, Mirror };

    struct GripDrag {
        Handle entity;
        GripPoint grip;
        GripMode mode = GripMode::Stretch;
        /// Distance from the grip to the pointer when Scale was entered, which
        /// is the 1.0 a factor is measured against.
        double reference = 0.0;
        /// A base point picked instead of the grip's own.
        ///
        /// the `Base point` setting in other CAD software option. Rotating a part about a corner while
        /// holding the grip at its edge is the usual reason: the grip says
        /// *which* object and the base says *about what*, and conflating them
        /// means the only available centre is wherever you happened to grab.
        std::optional<Vec3> base;
        /// Set while the next click is the new base rather than the destination.
        bool awaitingBase = false;
        /// Set when a press has been spent on something other than placing the
        /// grip, so the release that follows does not finish the edit.
        bool ignoreRelease = false;
        /// Leave the original where it is and place copies instead.
        ///
        /// the `Copy` setting in other CAD software option, and it stays on: one `C` then a click for
        /// every copy, until Escape. A drafter placing eight of something types
        /// the option once, which is the whole point of it being a mode rather
        /// than a modifier.
        bool copying = false;
        /// A copy of the entity with the grip already moved. Drawn as the
        /// preview, so the database is untouched until the edit is finished
        /// and a cancelled one needs no rollback.
        std::unique_ptr<Entity> preview;
        /// Where the button went down, so a click can be told from a drag.
        QPointF pressedAt;
        /// True once the button has been released without the pointer having
        /// moved: the grip is now held by the *click*, and the next click puts
        /// it down. other CAD software works this way and it is much the better idiom --
        /// dragging a grip across a drawing while holding the button is a
        /// steadiness test, and it fails at the moment of release, which is
        /// exactly when precision matters.
        bool heldByClick = false;
        /// The entity as it was, for reporting what the edit changed.
        std::unique_ptr<Entity> original;

        /// The other grips going along with this one.
        ///
        /// other CAD software calls them hot grips: shift-click several and they all move
        /// together, which is how a wall shared by two rooms is lengthened
        /// without leaving a gap. Held beside the primary rather than folded in
        /// with it, because the primary is also the base point and the readout
        /// subject -- one of the held grips is special and pretending otherwise
        /// would mean deciding which one every time it is asked.
        ///
        /// Grouped by object before anything is applied -- see `hotGripGroups`.
        /// Several grips on one object are one edit of that object, not
        /// several, and treating them separately drew a stretch as a bow tie
        /// and moved an object once per grip.
        struct Companion {
            Handle entity;
            GripPoint grip;
            std::unique_ptr<Entity> preview;
            std::unique_ptr<Entity> original;
        };
        std::vector<Companion> companions;
    };

    /// What the grip edit currently amounts to, for the readout under the
    /// cursor: how far it has moved and what the shape now measures.
    ///
    /// Per type, because the useful number differs: a line has a length, a
    /// circle a radius, a rectangle two sides. Reporting a coordinate for all
    /// of them would be uniform and useless -- the question a drafter is
    /// answering while holding a grip is "how long is it now", not "where is
    /// this point".
    [[nodiscard]] QString gripReadout() const;



    /// Grips shift-clicked to go along with the next edit.
    ///
    /// Emptied when the edit finishes or the selection changes: a hot grip that
    /// outlived the objects it belonged to would move something the user had
    /// stopped thinking about.
    std::vector<std::pair<Handle, GripPoint>> hotGrips_;

    /// Add or remove a grip from the hot set.
    void toggleHotGrip(Handle entity, const GripPoint& grip);

    /// True when this grip is in the hot set.
    [[nodiscard]] bool isHotGrip(Handle entity, const GripPoint& grip) const;

    /// The grip under the cursor, if the cursor is over one.
    [[nodiscard]] std::optional<std::pair<Handle, GripPoint>> gripAt(
        const QPointF& position) const;

    /// Refresh which grip is highlighted. Returns true when it changed.
    bool updateHotGrip(const QPointF& position);

    void beginGripDrag(Handle handle, const GripPoint& grip);
    void updateGripDrag(const Vec3& world);
    void commitGripDrag();

    /// Finish the edit with the grip at `destination`.
    ///
    /// Separate from `commitGripDrag` because a typed distance names a point
    /// the pointer is not at, and the commit must use the one it was given.
    void commitGripDragAt(const Vec3& destination);

    /// Add a copy of the held entity, edited to `destination`, keeping the grip.
    void placeGripCopy(const Vec3& destination);

    /// Apply a grip edit to `target`, taking `grip` to `destination`.
    ///
    /// One place, used by both the preview and the commit, so what is shown and
    /// what is done cannot drift apart. The mode and the scale reference are
    /// arguments rather than reads of the held grip, because the commit has to
    /// let go of the grip *before* it edits — and the version that read the
    /// member did nothing at all once it had.
    static void applyGripEdit(Entity& target, const GripPoint& grip, GripMode mode,
                              double reference, const Vec3& destination, const Vec3& base);
    /// The hot grips gathered by the object they belong to, held one first.
    ///
    /// Two grips on the same object are one edit of it. Applied separately the
    /// preview showed one ghost per grip -- a rectangle with two corners hot
    /// drew as a bow tie -- and the transform modes ran once per grip, so two
    /// hot grips moved an object exactly twice as far as asked.
    [[nodiscard]] std::vector<std::pair<Handle, std::vector<GripPoint>>> hotGripGroups() const;

    /// Apply every hot grip of one object to it, for a pointer at `destination`.
    ///
    /// Static, and told everything rather than reading `gripDrag_`: the commit
    /// releases the drag before it modifies anything, and a version that read
    /// the member found it already gone.
    static void applyHotGrips(Entity& target, const std::vector<GripPoint>& grips, GripMode mode,
                              double reference, const Vec3& heldGrip, const Vec3& destination,
                              const Vec3& base);

    /// Where a companion grip goes when the held one is taken to `destination`.
    [[nodiscard]] static Vec3 companionDestination(GripMode mode, const Vec3& heldGrip,
                                                   const Vec3& companionGrip,
                                                   const Vec3& destination);
    void cancelGripDrag();

    void drawGripPreview(QPainter& painter) const;

    /// Draw the sheet and its printable area, in paper space only.
    void drawSheet(QPainter& painter) const;

    /// Complete a rubber-band selection rectangle.
    void finishSelectionRectangle(const QPointF& position, bool subtractive);

    /// The pick aperture in world units at the current zoom.
    [[nodiscard]] double apertureWorld() const;

    /// Recompute which entity the cursor is over. Returns true when it changed.
    bool updateHoveredEntity(const QPointF& position);

    void drawScene(QPainter& painter);
    void drawGrid(QPainter& painter);

    /// Outline the entity under the cursor, before any click.
    ///
    /// other CAD software calls this rollover highlighting, and it answers the question
    /// every pick starts with: *which* of these overlapping objects am I about
    /// to get? Snap markers do not answer it — they say where the point would
    /// land, not what would be selected.
    void drawHoverHighlight(QPainter& painter);
    void drawSelectionHighlight(QPainter& painter);

    /// Trace an entity's outline with the painter's current pen.
    ///
    /// Shared by the hover and selection passes so the two cannot disagree
    /// about an entity's shape — including the fallback to extents for text and
    /// block references, which is also the region a click tests.
    void outlineEntity(QPainter& painter, const Entity& entity) const;
    void drawPreview(QPainter& painter);
    void drawSnapMarker(QPainter& painter) const;
    void drawSelectionRectangle(QPainter& painter) const;
    void drawCrosshair(QPainter& painter) const;

    Document* document_ = nullptr;
    DynamicInput* dynamicInput_ = nullptr;
    std::unique_ptr<render::SceneBuilder> sceneBuilder_;
    render::Viewport viewport_;

    /// Model to paper through the active viewport, and back; identity when
    /// none is active. `spaceScale_` is model units per paper unit.
    Mat4 spaceToPaper_ = Mat4::identity();
    Mat4 paperToSpace_ = Mat4::identity();
    double spaceScale_ = 1.0;
    void refreshSpaceTransform();
    [[nodiscard]] ViewportEntity* activeWindow() const;
    void drawActiveViewportFrame(QPainter& painter) const;
    SnapEngine snapEngine_;

    // --- interaction state
    bool panning_ = false;
    QPointF lastMousePosition_;
    QPointF cursorPosition_;
    bool hasCursor_ = false;

    /// Rubber-band selection, armed on left-press and resolved on release.
    bool selecting_ = false;
    QPointF selectionOrigin_;

    /// The entity under the cursor, outlined so it is clear what a click gets.
    std::optional<Handle> hoveredEntity_;

    /// Everything under the cursor, nearest first, and which of them is being
    /// offered. Recomputed whenever the cursor moves to a different object, so
    /// cycling always walks the current stack rather than a stale one.
    std::vector<Handle> hoverCandidates_;
    std::size_t hoverIndex_ = 0;

    /// Recently hovered entities, kept as snap candidates wherever the cursor
    /// goes next.
    ///
    /// The EXTENSION snap is about a point *off* the object — carrying its line
    /// past its own end — so the object is by definition further from the
    /// cursor than the snap aperture. Searching the index around the cursor can
    /// never find it, which is why extension appeared to do nothing.
    ///
    /// other CAD software solves this by acquisition: hover an object, move away, and its
    /// extension becomes available. That is both the familiar behaviour and the
    /// affordable one — a handful of remembered objects rather than an
    /// unbounded search radius on every mouse move.
    std::deque<Handle> acquiredEntities_;

    /// Snap points the pointer has rested on, kept as origins to align to.
    ///
    /// other CAD software calls this acquiring a point, and it is what lets a drafter put
    /// a corner level with one thing and in line with another without drawing
    /// construction lines to find where that is. Most recent first, and few:
    /// every snap passed over becomes one, so an unbounded list would fill with
    /// everything the pointer had ever crossed.
    /// A vector rather than a deque: it holds at most three, and `cad_core`
    /// takes a span of them.
    std::vector<Vec3> trackedPoints_;
    /// The snap point the pointer is resting on and has not yet acquired.
    /// Acquisition waits for a moment's rest, so passing over a corner does
    /// not leave a path behind; the timer says when the moment is up.
    std::optional<Vec3> pendingAcquire_;
    QTimer acquireTimer_;

    /// Expanded block geometry, kept between mouse moves.
    ///
    /// Snapping asks every nearby object for its curves on every move, and a
    /// block has to be walked to answer -- one of them in the sample drawing
    /// holds two thousand objects. Expanding that on each move would make the
    /// pointer stutter over it. Cleared whenever the drawing changes shape, in
    /// `reload`, which is also when the spatial index is rebuilt.
    mutable std::unordered_map<std::uint64_t, std::vector<CurvePtr>> blockCurveCache_;
    mutable std::uint64_t cacheGeneration_ = 0;

    /// The curves of a block reference, from the cache or freshly expanded.
    [[nodiscard]] const std::vector<CurvePtr>& cachedBlockCurves(
        const InsertEntity& insert) const;

    /// The alignment path the point is currently being held on, so it can be
    /// drawn. Without the path on screen a constrained point looks like a bug.
    struct TrackingHit {
        Vec3 point;
        /// The acquired points feeding it: one for a single path, two where
        /// the point is their crossing.
        std::vector<Vec3> origins;
    };
    mutable std::optional<TrackingHit> trackingHit_;

    /// The field the next keystroke goes into, and the value of the one left
    /// behind. Only ever one lock: with both locked there is nothing left for
    /// the pointer to decide and the point is simply typed.
    DimensionField dimensionField_ = DimensionField::First;
    std::optional<double> lockedDimension_;
    /// Mirrors the command line while a point is being entered, so the active
    /// box can show what is being typed into it.
    QString dimensionTyped_;
    void acquireForSnapping(Handle handle);

    /// The grip under the cursor, highlighted so it is clear it can be grabbed.
    std::optional<std::pair<Handle, GripPoint>> hotGrip_;
    std::optional<GripDrag> gripDrag_;

    std::optional<SnapPoint> activeSnap_;
    std::uint32_t snapModes_ = kDefaultSnapModes;
    /// A snap chosen for the next point only, and cleared once it is used.
    std::optional<SnapMode> snapOverride_;
    double aperturePixels_ = 10.0;

    /// How long the right button must be held before it means "menu" rather
    /// than "Enter". other CAD software's own default is a quarter of a second.
    static constexpr int kRightHoldMilliseconds = 250;
    QBasicTimer rightHoldTimer_;
    QPointF rightPressAt_;
    bool rightHeld_ = false;
    double crosshairPercent_ = 5.0;
    /// Scene-building choices the user controls.
    [[nodiscard]] render::SceneOptions sceneOptions() const;

    double textGreekingPixels_ = 3.0;
    bool showImageFrames_ = false;
    bool rolloverHighlight_ = true;

    /// The editor that appears over a label being edited, when the running
    /// command names the object whose text it wants.
    InPlaceTextEditor* textEditor_ = nullptr;
    /// The request the editor is currently open for, so it is opened once
    /// rather than reset on every repaint.
    const InputRequest* editorRequest_ = nullptr;

    /// Where an entity's text sits on screen, and how big, when it is the kind
    /// of entity that has text. Empty when it is not, or when it is off-screen.
    [[nodiscard]] std::optional<InPlaceTextEditor::Placement> textPlacementFor(
        Handle handle) const;

    /// Decoded attached images, kept across frames.
    ///
    /// Mutable because painting is const and a cache that could not be filled
    /// during a paint would be filled during nothing: the size an image is
    /// needed at is known only once the view is known.
    mutable ImageCache imageCache_;

    /// Views to return to, most recent last. Bounded because a long session
    /// would otherwise accumulate one per zoom for as long as it runs.
    void rememberView();
    std::vector<render::Viewport> viewHistory_;

    std::size_t lastVisible_ = 0;
    double lastBuildMs_ = 0.0;
    double lastPaintMs_ = 0.0;
};

}  // namespace cad::app
