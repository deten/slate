#include "CanvasWidget.hpp"

#include <QRegularExpression>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <QElapsedTimer>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include "ScenePainter.hpp"
#include "Theme.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/Layer.hpp"
#include "cad/db/ViewportEntity.hpp"
#include "cad/db/LeaderEntity.hpp"
#include "cad/math/Angle.hpp"
#include "cad/math/Mat4.hpp"
#include "cad/db/BlockGeometry.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/db/TextStyle.hpp"
#include "cad/render/Picking.hpp"

namespace cad::app {
namespace {

/// Round a world step to the nearest 1, 2 or 5 times a power of ten, so grid
/// values stay readable at every zoom.
[[nodiscard]] double niceStep(double target) {
    if (!(target > 0.0)) {
        return 1.0;
    }
    const double magnitude = std::pow(10.0, std::floor(std::log10(target)));
    const double normalized = target / magnitude;
    const double stepped =
        normalized <= 1.0 ? 1.0 : (normalized <= 2.0 ? 2.0 : (normalized <= 5.0 ? 5.0 : 10.0));
    return stepped * magnitude;
}

/// A one-pixel line on an integer coordinate straddles two pixel rows and looks
/// blurry; a half-pixel offset puts it on a pixel centre.
[[nodiscard]] double snapHalf(double value) {
    return std::round(value) + 0.5;
}

/// Snap marker size, in device-independent pixels. The pick aperture is a
/// setting rather than a constant — see `setAperturePixels`.
constexpr double kSnapMarkerPixels = 7.0;

}  // namespace

CanvasWidget::CanvasWidget(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    // Deliberately never takes the keyboard.
    //
    // The command line owns typed input entirely: it holds the buffer, the
    // prompt, the history and the parsing. Splitting the keyboard between two
    // widgets and moving focus on the first keystroke looks equivalent and is
    // not — Qt applies the focus change asynchronously, so typing `m` then
    // space fast enough delivers the space to whichever widget the handover
    // has not finished leaving. With no handover there is no race, and clicks
    // reach the canvas regardless of who holds focus.
    setFocusPolicy(Qt::NoFocus);
    // The canvas paints every pixel it owns; letting Qt clear it first is a
    // wasted fill.
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
    // Blank: the canvas draws its own crosshair. The system cross would sit on
    // top of ours at a size we do not control -- two cursors, and a
    // contradiction of the size setting.
    setCursor(Qt::BlankCursor);
    dynamicInput_ = new DynamicInput{this};

    // The one widget on the canvas that *does* take the keyboard, and only
    // while it is open. Editing a paragraph needs Enter, arrow keys and a
    // caret, none of which a shared command line can offer over the drawing.
    textEditor_ = new InPlaceTextEditor{this};
    connect(textEditor_, &InPlaceTextEditor::committed, this,
            [this](const QString& text) {
                if (document_ == nullptr || !document_->executor().isRunning()) {
                    return;
                }
                editorRequest_ = nullptr;
                // Already MText contents, coded: the editor owns that
                // conversion now, because only it knows the formatting the user
                // applied.
                document_->executor().supply(InputValue::makeText(text.toStdString()));
                document_->reindex();
                Q_EMIT commandStateChanged();
                update();
            });
    connect(textEditor_, &InPlaceTextEditor::cancelled, this, [this] {
        editorRequest_ = nullptr;
        if (document_ != nullptr && document_->executor().isRunning()) {
            document_->executor().supply(InputValue::makeCancel());
            document_->reindex();
            Q_EMIT commandStateChanged();
        }
        update();
    });

    syncViewportSize();
}

CanvasWidget::~CanvasWidget() = default;

QPoint CanvasWidget::promptAnchor() const {
    if (hasCursor_) {
        return cursorPosition_.toPoint();
    }
    // No cursor over the canvas: the bottom-left corner, above the command
    // line, which is where the eye goes when it is not on the drawing.
    return QPoint{16, std::max(0, height() - 56)};
}

void CanvasWidget::showDynamicInput(const QString& prompt, const QString& text) {
    dynamicInput_->showAt(promptAnchor(), prompt, text);
}

void CanvasWidget::hideDynamicInput() {
    dynamicInput_->hideInput();
}

void CanvasWidget::setDocument(Document* document) {
    document_ = document;
    sceneBuilder_ = document == nullptr
                        ? nullptr
                        : std::make_unique<render::SceneBuilder>(document->db(),
                                                                 document->index());
    if (sceneBuilder_ != nullptr) {
        // A sheet's viewports look into the model, not into the sheet.
        sceneBuilder_->setModelIndex(&document->modelIndex());
    }
    activeSnap_.reset();
    reload();
}

void CanvasWidget::setSnapOverride(SnapMode mode) {
    snapOverride_ = mode;
    activeSnap_.reset();
    // Recomputed at once, so the marker under the cursor changes as soon as the
    // menu closes rather than on the next twitch of the mouse.
    if (hasCursor_) {
        updateSnap(cursorPosition_);
    }
    update();
}

void CanvasWidget::setSnapModes(std::uint32_t modes) {
    snapModes_ = modes;
    activeSnap_.reset();
    update();
}

void CanvasWidget::syncViewportSize() {
    viewport_ = viewport_.withSize(std::max(1, width()), std::max(1, height()))
                    .withPixelRatio(devicePixelRatioF());
}

void CanvasWidget::reload() {
    if (document_ != nullptr) {
        document_->reindex();
    }
    refreshSpaceTransform();
    zoomExtents();
}

void CanvasWidget::rememberView() {
    // Bounded: a long session would otherwise keep one view per zoom for as
    // long as it runs.
    constexpr std::size_t kMaxHistory = 32;
    viewHistory_.push_back(viewport_);
    if (viewHistory_.size() > kMaxHistory) {
        viewHistory_.erase(viewHistory_.begin());
    }
}

void CanvasWidget::zoomToBox(const Box3& box) {
    if (box.isEmpty()) {
        return;
    }
    if (viewportActive()) {
        // Inside a viewport, zooming moves the model behind the frame; the
        // sheet itself stays where it is on screen.
        viewWindow(box);
        return;
    }
    rememberView();
    syncViewportSize();
    viewport_ = viewport_.zoomToFit(box);
    Q_EMIT viewChanged();
    update();
}

void CanvasWidget::panBy(const Vec3& displacement) {
    if (ViewportEntity* window = activeWindow()) {
        window->viewCenter = window->viewCenter - displacement;
        refreshSpaceTransform();
        Q_EMIT viewChanged();
        update();
        return;
    }
    rememberView();
    viewport_ = viewport_.withCenter(viewport_.center() - displacement);
    Q_EMIT viewChanged();
    update();
}

bool CanvasWidget::zoomPrevious() {
    if (viewHistory_.empty()) {
        return false;
    }
    viewport_ = viewHistory_.back();
    viewHistory_.pop_back();
    syncViewportSize();
    Q_EMIT viewChanged();
    update();
    return true;
}

void CanvasWidget::setViewport(const render::Viewport& view) {
    viewport_ = view;
    syncViewportSize();
    Q_EMIT viewChanged();
    update();
}

void CanvasWidget::zoomExtents() {
    if (document_ != nullptr && viewportActive()) {
        // The model's extents into the frame, as MVIEW fits them.
        const Box3 model = render::spaceExtents(document_->db(), document_->db().modelSpace(),
                                                &document_->modelIndex());
        if (!model.isEmpty()) {
            viewWindow(model);
        }
        return;
    }
    rememberView();
    syncViewportSize();
    if (document_ == nullptr) {
        update();
        return;
    }

    Box3 extents;
    Box3 fallback{Vec3::zero(), Vec3{420.0, 297.0, 0.0}};

    if (document_->inPaperSpace()) {
        // A sheet fits to the *paper*, not to its contents. Zooming to the
        // geometry would frame the title block and cut off the page, which is
        // the opposite of what paper space is for; and the index holds model
        // space, so it is the wrong thing to ask either way.
        if (const Box3 sheet = document_->activeLayout().sheetRect();
            sheet.width() > 0.0 && sheet.height() > 0.0) {
            fallback = sheet;
        }
        extents = fallback;
    } else {
        extents = render::spaceExtents(document_->db(), document_->activeSpace(),
                                       &document_->index());
    }

    // An empty drawing gets a sensible sheet-sized view rather than an
    // undefined one. A margin keeps the extents off the window edge.
    viewport_ = viewport_.zoomToFit(extents.isEmpty() ? fallback : extents);

    Q_EMIT viewChanged();
    update();
}

double CanvasWidget::apertureWorld() const {
    // In the units picks are made in: model units when a viewport is active.
    return viewport_.pixelsToWorld(aperturePixels_) * spaceScale_;
}

// ------------------------------------------------------------ active viewport

Vec3 CanvasWidget::toWorld(const render::ScreenPoint& screen) const {
    return paperToSpace_.transformPoint(viewport_.screenToWorld(screen));
}

render::ScreenPoint CanvasWidget::toScreen(const Vec3& world) const {
    return viewport_.worldToScreen(spaceToPaper_.transformPoint(world));
}

bool CanvasWidget::viewportActive() const {
    return document_ != nullptr && document_->activeViewport() != nullptr;
}

void CanvasWidget::refreshSpaceTransform() {
    // The identity on the Model tab and on a sheet; through the active
    // viewport, model coordinates on the way in and paper on the way out.
    spaceToPaper_ = Mat4::identity();
    paperToSpace_ = Mat4::identity();
    spaceScale_ = 1.0;
    const ViewportEntity* window = document_ != nullptr ? document_->activeViewport() : nullptr;
    if (window == nullptr) {
        return;
    }
    spaceToPaper_ = window->modelToPaper();
    if (const auto inverse = spaceToPaper_.inverted()) {
        paperToSpace_ = *inverse;
    }
    spaceScale_ = window->modelPerPaper();
}

bool CanvasWidget::activateViewport(Handle handle) {
    if (document_ == nullptr || !document_->inPaperSpace()) {
        return false;
    }
    const auto* window = document_->db().get<ViewportEntity>(handle);
    if (window == nullptr || window->erased || window->isPaperSpaceViewport() ||
        window->ownerHandle != document_->activeLayout().blockRecordHandle) {
        return false;
    }
    document_->clearSelection();
    document_->setActiveViewport(handle);
    document_->reindex();
    refreshSpaceTransform();
    Q_EMIT selectionChanged();
    Q_EMIT viewChanged();
    update();
    return true;
}

bool CanvasWidget::activateSoleViewport() {
    if (document_ == nullptr || !document_->inPaperSpace()) {
        return false;
    }
    Handle only;
    int count = 0;
    const auto* sheet = document_->db().get<BlockTableRecord>(document_->activeLayout().blockRecordHandle);
    if (sheet == nullptr) {
        return false;
    }
    for (const Entity* entity : document_->db().entitiesIn(*sheet)) {
        const auto* window = dynamic_cast<const ViewportEntity*>(entity);
        if (window != nullptr && !window->erased && !window->isPaperSpaceViewport() && window->isOn()) {
            only = window->handle;
            ++count;
        }
    }
    return count == 1 && activateViewport(only);
}

bool CanvasWidget::activateViewportAt(const render::ScreenPoint& screen) {
    if (document_ == nullptr || !document_->inPaperSpace()) {
        return false;
    }
    const Vec3 paper = viewport_.screenToWorld(screen);
    const auto* sheet = document_->db().get<BlockTableRecord>(document_->activeLayout().blockRecordHandle);
    if (sheet == nullptr) {
        return false;
    }
    // The last one drawn is on top, so it wins where frames overlap.
    Handle hit;
    for (const Entity* entity : document_->db().entitiesIn(*sheet)) {
        const auto* window = dynamic_cast<const ViewportEntity*>(entity);
        if (window != nullptr && !window->erased && !window->isPaperSpaceViewport() &&
            window->isOn() && window->paperRect().containsPoint(paper)) {
            hit = window->handle;
        }
    }
    return !hit.isNull() && activateViewport(hit);
}

void CanvasWidget::exitViewport() {
    if (document_ == nullptr || document_->activeViewport() == nullptr) {
        return;
    }
    document_->clearSelection();
    document_->setActiveViewport(Handle{});
    document_->reindex();
    refreshSpaceTransform();
    Q_EMIT selectionChanged();
    Q_EMIT viewChanged();
    update();
}

ViewportEntity* CanvasWidget::activeWindow() const {
    if (document_ == nullptr) {
        return nullptr;
    }
    const ViewportEntity* window = document_->activeViewport();
    return window == nullptr ? nullptr : document_->db().get<ViewportEntity>(window->handle);
}

void CanvasWidget::viewWindow(const Box3& modelBox) {
    // The model region a viewport shows is a centre and a height; the width
    // follows from the frame. Fitted both ways round, as MVIEW fits.
    ViewportEntity* window = activeWindow();
    if (window == nullptr || modelBox.isEmpty() || window->height <= 0.0 || window->width <= 0.0) {
        return;
    }
    window->viewCenter = modelBox.center() - window->viewTarget;
    window->viewHeight = std::max(modelBox.height(),
                                  modelBox.width() * window->height / window->width);
    if (window->viewHeight <= 0.0) {
        window->viewHeight = window->height;
    }
    refreshSpaceTransform();
    Q_EMIT viewChanged();
    update();
}

void CanvasWidget::drawActiveViewportFrame(QPainter& painter) const {
    const ViewportEntity* window = document_ != nullptr ? document_->activeViewport() : nullptr;
    if (window == nullptr) {
        return;
    }
    // A heavier frame in the selection colour says which window the model
    // is being worked in through, as other CAD software's thick border does.
    const Box3 frame = window->paperRect();
    const render::ScreenPoint low = viewport_.worldToScreen(frame.min);
    const render::ScreenPoint high = viewport_.worldToScreen(frame.max);
    QPen pen{currentTheme().selection};
    pen.setWidthF(3.0);
    pen.setCosmetic(true);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(QRectF{QPointF{low.x, low.y}, QPointF{high.x, high.y}}.normalized());
}

void CanvasWidget::setAperturePixels(double pixels) {
    // Clamped rather than trusted: a zero aperture makes picking impossible and
    // a huge one makes it meaningless, and both are reachable by typing a
    // number into a settings box.
    aperturePixels_ = std::clamp(pixels, 2.0, 40.0);
    activeSnap_.reset();
    update();
}

void CanvasWidget::setCrosshairPercent(double percent) {
    crosshairPercent_ = std::clamp(percent, 1.0, 100.0);
    update();
}

void CanvasWidget::setTextGreekingPixels(double pixels) {
    textGreekingPixels_ = std::clamp(pixels, 1.0, 20.0);
    update();
}

void CanvasWidget::setShowImageFrames(bool show) {
    showImageFrames_ = show;
    update();
}

render::SceneOptions CanvasWidget::sceneOptions() const {
    render::SceneOptions options;
    options.showImageFrames = showImageFrames_;
    return options;
}

void CanvasWidget::setRolloverHighlight(bool enabled) {
    rolloverHighlight_ = enabled;
    if (!enabled) {
        hoveredEntity_.reset();
        hoverCandidates_.clear();
        hoverIndex_ = 0;
    }
    update();
}

void CanvasWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    syncViewportSize();
    Q_EMIT viewChanged();
}

// ------------------------------------------------------------------- snapping

const std::vector<CurvePtr>& CanvasWidget::cachedBlockCurves(const InsertEntity& insert) const {
    // Thrown away whenever anything is committed, rather than tracking which
    // edits could matter: moving a reference, editing its definition and
    // erasing something inside it all invalidate this, and missing one would
    // leave the pointer snapping to geometry that is no longer there.
    if (cacheGeneration_ != document_->generation()) {
        blockCurveCache_.clear();
        cacheGeneration_ = document_->generation();
    }
    const auto key = insert.handle.value();
    const auto found = blockCurveCache_.find(key);
    if (found != blockCurveCache_.end()) {
        return found->second;
    }
    return blockCurveCache_.emplace(key, blockReferenceCurves(document_->db(), insert))
        .first->second;
}

void CanvasWidget::updateSnap(const QPointF& position) {
    activeSnap_.reset();
    if (document_ == nullptr || snapModes_ == 0) {
        return;
    }

    const Vec3 raw = toWorld({position.x(), position.y()});
    const double aperture = apertureWorld();

    // Only entities the index says are nearby can contribute a snap, which is
    // what keeps this affordable on every mouse move.
    std::vector<Handle> nearby = document_->index().searchNear(raw, aperture * 2.0);

    // Plus anything recently hovered. EXTENSION reports a point off the end of
    // an object, so the object itself is always outside the aperture and the
    // search above can never return it.
    for (const Handle handle : acquiredEntities_) {
        if (std::find(nearby.begin(), nearby.end(), handle) == nearby.end()) {
            nearby.push_back(handle);
        }
    }
    if (nearby.empty()) {
        return;
    }

    // The curves must outlive the query, since SnapCandidate holds spans over
    // them rather than copying.
    std::vector<std::vector<CurvePtr>> ownedCurves;
    std::vector<Vec3> insertionPoints;
    ownedCurves.reserve(nearby.size());
    insertionPoints.reserve(nearby.size());

    std::vector<SnapCandidate> candidates;
    candidates.reserve(nearby.size());

    for (const Handle handle : nearby) {
        const Entity* entity = document_->db().getEntity(handle);
        if (entity == nullptr || entity->erased) {
            continue;
        }
        SnapCandidate candidate;
        candidate.handle = handle;

        if (const auto* insert = dynamic_cast<const InsertEntity*>(entity)) {
            // A block draws the lines of its definition, and those lines have
            // endpoints and midpoints like any others -- a drafter snapping to
            // the corner of a door symbol is doing the ordinary thing. Asking
            // the reference for its curves returns nothing, so nothing inside a
            // block could be snapped to at all.
            const std::vector<CurvePtr>& expanded = cachedBlockCurves(*insert);
            ownedCurves.emplace_back();
            candidate.curves = expanded;
            // Its insertion point too, which is other CAD software's INSert snap and the
            // one grip the reference offers.
            insertionPoints.push_back(insert->position);
            candidate.insertionPoint = &insertionPoints.back();
        } else {
            ownedCurves.push_back(entity->toCurves());
            insertionPoints.push_back(entity->bounds().center());
            candidate.curves = ownedCurves.back();
            candidate.isNode = entity->objectType() == "Point";
            if (candidate.isNode) {
                insertionPoints.back() = entity->bounds().min;
                candidate.insertionPoint = &insertionPoints.back();
            }
        }
        candidates.push_back(candidate);
    }
    if (candidates.empty()) {
        return;
    }

    SnapQuery query;
    query.cursor = raw;
    query.aperture = aperture;
    // An override replaces the running snaps for this one point. `None` means
    // no snapping at all, which is a real answer rather than the absence of
    // one: a point in a crowded drawing sometimes has to go exactly where it
    // was pointed.
    query.modes = snapOverride_.has_value()
                      ? (*snapOverride_ == SnapMode::None ? 0U : toMask(*snapOverride_))
                      : snapModes_;
    query.candidates = candidates;

    // PERPENDICULAR and TANGENT are meaningless without knowing where the line
    // is being drawn *from*, so the running command's last point supplies it.
    const InputRequest* request =
        document_->executor().isRunning() ? document_->executor().currentRequest() : nullptr;
    if (request != nullptr && request->basePoint.has_value()) {
        query.basePoint = &*request->basePoint;
    }

    activeSnap_ = snapEngine_.findBest(query);

    // Pointing at a snap acquires it. other CAD software asks for a moment's hover; the
    // marker here appears at once, so acquiring at once keeps the two in step
    // -- and the list holds only the last few, so passing over a crowd of
    // snaps leaves the ones that were pointed at most recently.
    if (trackingEnabled() && activeSnap_.has_value()) {
        acquireTrackingPoint(activeSnap_->position);
    }
}

Vec3 CanvasWidget::worldAt(const QPointF& position) const {
    if (activeSnap_.has_value()) {
        return activeSnap_->position;
    }
    return toWorld({position.x(), position.y()});
}

bool CanvasWidget::orthoEnabled() const {
    return document_ != nullptr && document_->db().settings.orthoEnabled;
}

void CanvasWidget::setOrthoEnabled(bool enabled) {
    if (document_ != nullptr) {
        document_->db().settings.orthoEnabled = enabled;
    }
    update();
}

bool CanvasWidget::polarEnabled() const {
    return document_ != nullptr && document_->db().settings.polarTrackingEnabled;
}

void CanvasWidget::setPolarEnabled(bool enabled) {
    if (document_ != nullptr) {
        document_->db().settings.polarTrackingEnabled = enabled;
        // Two constraints cannot both hold. other CAD software turns ortho off when polar
        // goes on and the reverse, rather than silently letting one win.
        if (enabled) {
            document_->db().settings.orthoEnabled = false;
        }
    }
    update();
}

double CanvasWidget::polarIncrement() const {
    if (document_ == nullptr) {
        return kPi / 4.0;
    }
    const double increment = document_->db().settings.polarAngleIncrement;
    // A zero or negative increment would divide the circle into nothing.
    return increment > 1e-6 ? increment : kPi / 4.0;
}

bool CanvasWidget::trackingEnabled() const {
    return document_ != nullptr && document_->db().settings.objectSnapTrackingEnabled;
}

void CanvasWidget::setTrackingEnabled(bool enabled) {
    if (document_ != nullptr) {
        document_->db().settings.objectSnapTrackingEnabled = enabled;
    }
    if (!enabled) {
        // Turning it off has to forget what was acquired, or turning it back on
        // resurrects paths from wherever the pointer happened to have been.
        trackedPoints_.clear();
        trackingHit_.reset();
    }
    update();
}

void CanvasWidget::acquireTrackingPoint(const Vec3& point) {
    constexpr std::size_t kMaxTracked = 3;
    const auto existing = std::find_if(
        trackedPoints_.begin(), trackedPoints_.end(),
        [&](const Vec3& held) { return held.equals(point); });
    if (existing != trackedPoints_.end()) {
        // Move it to the front rather than adding it twice: pointing at the
        // same corner again means it matters, not that there are two of them.
        trackedPoints_.erase(existing);
    }
    trackedPoints_.insert(trackedPoints_.begin(), point);
    if (trackedPoints_.size() > kMaxTracked) {
        trackedPoints_.resize(kMaxTracked);
    }
}

std::optional<Vec3> CanvasWidget::alignedToTracking(const Vec3& world) const {
    trackingHit_.reset();
    if (!trackingEnabled() || trackedPoints_.empty()) {
        return std::nullopt;
    }

    // The angles paths are drawn at. Polar's increment when polar is on, and
    // the four square directions otherwise -- which is what other CAD software tracks at
    // with polar off, and the pair that gets used for "level with that".
    const double increment = polarEnabled() ? polarIncrement() : kPi / 2.0;

    // The geometry lives in `cad_core`, where it can be tested without a window
    // to put it in. `apertureWorld` keeps the pull the same size on screen at
    // every zoom.
    std::optional<TrackingAlignment> aligned =
        alignToAcquired(trackedPoints_, world, increment, apertureWorld());
    if (!aligned) {
        return std::nullopt;
    }
    trackingHit_ = TrackingHit{aligned->point, std::move(aligned->origins)};
    return trackingHit_->point;
}

bool CanvasWidget::isEnteringDimension() const {
    if (document_ == nullptr) {
        return false;
    }
    // A held grip is a point being placed like any other, so it gets the boxes
    // too -- "adjust this corner to exactly 250" is the same question as
    // "put this corner exactly 250 away", and the answer should be typed the
    // same way. Not while a base point is being named: that click is choosing
    // what to measure *from*, and there is nothing to measure yet.
    if (gripDrag_) {
        return !gripDrag_->awaitingBase;
    }
    if (!document_->executor().isRunning()) {
        return false;
    }
    const InputRequest* request = document_->executor().currentRequest();
    return request != nullptr && request->kind == InputRequest::Kind::Point;
}

void CanvasWidget::setDimensionTyped(const QString& text) {
    if (dimensionTyped_ == text) {
        return;
    }
    dimensionTyped_ = text;
    update();
}

void CanvasWidget::clearDimensionInput() {
    dimensionField_ = DimensionField::First;
    lockedDimension_.reset();
    dimensionTyped_.clear();
}

bool CanvasWidget::advanceDimensionField(const QString& typed) {
    if (!isEnteringDimension()) {
        return false;
    }
    // Tab locks what the field says *now*, whether that was typed or came from
    // the pointer. Locking only typed values would make Tab do nothing on a
    // field the drafter is happy with, which is most of them.
    bool ok = false;
    const double value = typed.trimmed().toDouble(&ok);
    if (ok) {
        lockedDimension_ = value;
    } else {
        const std::array<double, 2> live = liveDimensions();
        lockedDimension_ = live[dimensionField_ == DimensionField::First ? 0 : 1];
    }
    dimensionField_ = dimensionField_ == DimensionField::First ? DimensionField::Second
                                                               : DimensionField::First;
    dimensionTyped_.clear();
    update();
    return true;
}

std::array<double, 2> CanvasWidget::liveDimensions() const {
    if (!hasCursor_) {
        return {0.0, 0.0};
    }
    const Vec3 world = constrainedWorldAt(cursorPosition_);
    const std::optional<Vec3> origin = constraintOrigin();
    if (!origin) {
        // Nothing to measure from, so the fields are the coordinates
        // themselves: the first point of a line has no length yet.
        return {world.x, world.y};
    }
    const double dx = world.x - origin->x;
    const double dy = world.y - origin->y;
    double degrees = radToDeg(std::atan2(dy, dx));
    if (degrees < 0.0) {
        degrees += 360.0;
    }
    return {std::hypot(dx, dy), degrees};
}

bool CanvasWidget::supplyDimensionInput(const QString& text) {
    if (!isEnteringDimension() || !lockedDimension_) {
        // Without a locked field there is nothing here the command line cannot
        // already do: a bare number is direct distance entry and a pair is a
        // coordinate, both of which it parses.
        return false;
    }
    bool ok = false;
    const double typed = text.trimmed().toDouble(&ok);
    if (!ok) {
        return false;
    }

    // The locked field belongs to whichever box Tab was pressed in, and the
    // typed one to the box it moved to.
    const bool lockedIsFirst = dimensionField_ == DimensionField::Second;
    const double first = lockedIsFirst ? *lockedDimension_ : typed;
    const double second = lockedIsFirst ? typed : *lockedDimension_;

    const std::optional<Vec3> origin = constraintOrigin();
    const Vec3 point = origin ? Vec3{origin->x + first * std::cos(degToRad(second)),
                                     origin->y + first * std::sin(degToRad(second)), origin->z}
                              : Vec3{first, second, 0.0};
    // A held grip is finished by moving it there, not by handing a point to a
    // command: no command is running, and `supplyPoint` would have nothing to
    // give it to.
    if (gripDrag_) {
        clearDimensionInput();
        commitGripDragAt(point);
        return true;
    }
    clearDimensionInput();
    supplyPoint(point);
    return true;
}

QString CanvasWidget::dimensionSummary() const {
    if (!isEnteringDimension()) {
        return {};
    }
    const std::array<double, 2> live = liveDimensions();
    const bool polar = constraintOrigin().has_value();
    const auto shown = [&](int index) {
        if (lockedDimension_ &&
            ((index == 0) == (dimensionField_ == DimensionField::Second))) {
            return QString::number(*lockedDimension_, 'f', 3);
        }
        if (index == (dimensionField_ == DimensionField::First ? 0 : 1) &&
            !dimensionTyped_.isEmpty()) {
            return dimensionTyped_;
        }
        return QString::number(live[static_cast<std::size_t>(index)], 'f', 3);
    };
    return polar ? tr("%1 < %2").arg(shown(0), shown(1))
                 : tr("%1, %2").arg(shown(0), shown(1));
}

std::optional<Vec3> CanvasWidget::constraintOrigin() const {
    // A grip is measured from where it started; a command's point from the
    // point it is rubber-banding from.
    if (gripDrag_) {
        return gripDrag_->grip.position;
    }
    if (document_ != nullptr && document_->executor().isRunning()) {
        if (const InputRequest* request = document_->executor().currentRequest();
            request != nullptr && request->basePoint.has_value()) {
            return *request->basePoint;
        }
    }
    return std::nullopt;
}

Vec3 CanvasWidget::constrainedWorldAt(const QPointF& position) const {
    const Vec3 world = worldAt(position);

    // A locked dimension box holds its half of the point and lets the pointer
    // decide the rest. Locking the length and then sweeping the angle is the
    // gesture the feature exists for, and it outranks every other constraint:
    // the drafter typed an exact number, which is more definite than anything
    // ortho or a snap can infer.
    if (lockedDimension_ && isEnteringDimension()) {
        const std::optional<Vec3> origin = constraintOrigin();
        const bool lockedIsFirst = dimensionField_ == DimensionField::Second;
        if (!origin) {
            // No base point, so the boxes are absolute X and Y.
            return lockedIsFirst ? Vec3{*lockedDimension_, world.y, world.z}
                                 : Vec3{world.x, *lockedDimension_, world.z};
        }
        const double dx = world.x - origin->x;
        const double dy = world.y - origin->y;
        const double distance = lockedIsFirst ? *lockedDimension_ : std::hypot(dx, dy);
        const double angle = lockedIsFirst ? std::atan2(dy, dx) : degToRad(*lockedDimension_);
        return Vec3{origin->x + distance * std::cos(angle),
                    origin->y + distance * std::sin(angle), origin->z};
    }

    // An object snap outranks every constraint, as it does in other CAD software: a
    // drafter who points at an endpoint means *that* endpoint, and silently
    // sliding it onto an axis would put geometry somewhere nothing is.
    if (activeSnap_.has_value()) {
        trackingHit_.reset();
        return world;
    }
    // Tracking is checked before ortho and polar, and wins when the pointer is
    // on a path. Being on an alignment is a deliberate act -- the drafter went
    // and acquired that point -- where the polar angle is only ever the nearest
    // one to wherever the pointer drifted.
    if (const std::optional<Vec3> aligned = alignedToTracking(world)) {
        return *aligned;
    }
    if (!orthoEnabled() && !polarEnabled()) {
        return world;
    }
    const std::optional<Vec3> origin = constraintOrigin();
    if (!origin) {
        return world;
    }

    const double dx = world.x - origin->x;
    const double dy = world.y - origin->y;

    if (orthoEnabled()) {
        // Whichever axis the pointer has travelled further along. The other
        // component is taken from the origin rather than zeroed, so ortho works
        // away from the world origin as well as at it.
        return std::abs(dx) >= std::abs(dy) ? Vec3{world.x, origin->y, origin->z}
                                            : Vec3{origin->x, world.y, origin->z};
    }

    // Polar: the nearest multiple of the increment, keeping the distance the
    // pointer is at. Rounding the *angle* and not the position is what makes
    // the point slide along the ray rather than jump to a fixed spot on it.
    const double distance = std::hypot(dx, dy);
    if (distance < 1e-9) {
        return world;
    }
    const double increment = polarIncrement();
    const double snapped = std::round(std::atan2(dy, dx) / increment) * increment;
    return Vec3{origin->x + distance * std::cos(snapped),
                origin->y + distance * std::sin(snapped), origin->z};
}

// ---------------------------------------------------------------------- input

void CanvasWidget::supplyPoint(const Vec3& world) {
    if (document_ == nullptr) {
        return;
    }
    // Spent with the point it was entered for. A lock that outlived its point
    // would silently hold the *next* one at the same length.
    clearDimensionInput();
    // Spent. An override that outlived the point it was chosen for would be a
    // running snap the user never turned on.
    snapOverride_.reset();
    document_->executor().supply(InputValue::makePoint(world));
    // A command that finished may have created or erased geometry, so the
    // index must catch up before the next frame culls against it.
    document_->reindex();
    Q_EMIT commandStateChanged();
    update();
}

void CanvasWidget::supplyEntityAt(const QPointF& position) {
    if (document_ == nullptr) {
        return;
    }
    const Vec3 world = toWorld({position.x(), position.y()});
    const auto hit = render::pickEntity(document_->db(), document_->index(), world,
                                        apertureWorld());
    if (!hit.has_value()) {
        // Nothing under the cursor. The command keeps asking rather than
        // failing, which is what a missed pick should cost.
        return;
    }
    document_->setSelection({hit->handle});
    document_->executor().supply(InputValue::makeEntity(hit->handle));
    document_->reindex();
    // After the reindex, so the rollover is asked about where the geometry
    // ended up rather than where it started. It is frozen during a hold -- see
    // `updateHoveredEntity` -- and so is stale as soon as the hold ends.
    updateHoveredEntity(cursorPosition_);
    Q_EMIT selectionChanged();
    Q_EMIT commandStateChanged();
    update();
}

std::optional<InPlaceTextEditor::Placement> CanvasWidget::textPlacementFor(
    Handle handle) const {
    if (document_ == nullptr) {
        return std::nullopt;
    }
    const Entity* entity = document_->db().getEntity(handle);
    if (entity == nullptr) {
        return std::nullopt;
    }

    // The entity's own extents, which already account for a TEXT's
    // justification and an MTEXT's attachment corner. Re-deriving the box here
    // would be a second implementation of a rule that is easy to get subtly
    // wrong and hard to notice.
    const Box3 bounds = entity->bounds();
    if (bounds.isEmpty()) {
        return std::nullopt;
    }

    double worldHeight = 0.0;
    bool multiLine = false;
    Handle styleHandle;
    if (const auto* mtext = dynamic_cast<const MTextEntity*>(entity)) {
        worldHeight = mtext->height;
        multiLine = true;
        styleHandle = mtext->styleHandle;
    } else if (const auto* text = dynamic_cast<const TextEntity*>(entity)) {
        worldHeight = text->height;
        styleHandle = text->styleHandle;
    } else {
        return std::nullopt;
    }

    const render::ScreenPoint low = toScreen(bounds.min);
    const render::ScreenPoint high = toScreen(bounds.max);

    InPlaceTextEditor::Placement placement;
    placement.rect = QRectF{QPointF{std::min(low.x, high.x), std::min(low.y, high.y)},
                            QPointF{std::max(low.x, high.x), std::max(low.y, high.y)}};
    placement.pixelHeight = worldHeight * viewport_.scale();
    placement.multiLine = multiLine;
    // MText has no standalone bold code — weight is part of a font selection —
    // so the editor needs a family to make text bold *of*.
    if (const auto* style = document_->db().get<TextStyle>(styleHandle)) {
        placement.defaultFont = QString::fromStdString(
            style->typeface.empty() ? style->fontFile : style->typeface);
    }

    const ResolvedProperties resolved =
        entity->resolveProperties(document_->db().resolutionContextFor(*entity));
    placement.color = displayColor(resolved.rgb, 255, currentTheme());
    return placement;
}

bool CanvasWidget::isEditingText() const {
    return textEditor_ != nullptr && textEditor_->isEditing();
}

void CanvasWidget::syncInPlaceEditor() {
    if (textEditor_ == nullptr || document_ == nullptr) {
        return;
    }

    const InputRequest* request = document_->executor().isRunning()
                                      ? document_->executor().currentRequest()
                                      : nullptr;

    // Only when a command is asking for text *and* has said whose text it is.
    // Everything else — a distance, a point, no command at all — belongs to the
    // command line, and an editor that opened for those would be in the way.
    const bool wanted = request != nullptr && request->kind == InputRequest::Kind::Text &&
                        request->editText.has_value() && !request->editEntity.isNull();
    if (!wanted) {
        if (textEditor_->isEditing()) {
            textEditor_->abandon();
        }
        editorRequest_ = nullptr;
        return;
    }
    if (request == editorRequest_) {
        return;  // Already open for this one; reopening would discard the typing.
    }

    const std::optional<InPlaceTextEditor::Placement> placement =
        textPlacementFor(request->editEntity);
    if (!placement) {
        // Nothing to sit over — the object has no text geometry, or it is a
        // type the editor cannot place. The command line still has the prompt,
        // so the edit is possible either way.
        editorRequest_ = nullptr;
        return;
    }

    const bool paragraph = placement->multiLine;
    QString text = QString::fromStdString(*request->editText);
    if (paragraph) {
        text = QString::fromStdString(MTextEntity::paragraphsToLines(text.toStdString()));
    }
    textEditor_->beginEdit(*placement, text);
    editorRequest_ = request;
}

void CanvasWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    // On a sheet, a double-click inside a viewport steps into it, and one
    // outside the active viewport steps back out -- other CAD software's gesture for
    // MSPACE and PSPACE.
    if (event->button() == Qt::LeftButton && document_ != nullptr && document_->inPaperSpace()) {
        const render::ScreenPoint at{event->position().x(), event->position().y()};
        if (const ViewportEntity* window = document_->activeViewport()) {
            if (!window->paperRect().containsPoint(viewport_.screenToWorld(at))) {
                exitViewport();
                event->accept();
                return;
            }
        } else if (activateViewportAt(at)) {
            event->accept();
            return;
        }
    }
    // Double-clicking a label to change its words is how everybody edits text,
    // in CAD and out of it. Without it the only route was the properties
    // palette, which is data entry rather than drafting.
    if (event->button() != Qt::LeftButton || document_ == nullptr ||
        document_->executor().isRunning()) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }

    const Vec3 world = toWorld({event->position().x(), event->position().y()});
    const auto hit =
        render::pickEntity(document_->db(), document_->index(), world, apertureWorld());
    if (!hit.has_value()) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }

    Entity* picked = document_->db().getEntity(hit->handle);
    if (picked == nullptr) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }

    // Selecting is what a double-click always does. Starting a command is what
    // it does only when there is a command for what was clicked.
    document_->setSelection({hit->handle});

    // A block reference opens its definition, which is what double-clicking one
    // means everywhere: show me what this is made of.
    if (const auto* insert = dynamic_cast<const InsertEntity*>(picked)) {
        Q_EMIT selectionChanged();
        Q_EMIT blockEditRequested(insert->blockHandle);
        update();
        return;
    }

    // Anything else -- a wall, a hatch -- is selected and left alone. Running
    // TEXTEDIT on it opened an annotation command with nothing to annotate,
    // which then sat there asking the user to "select an annotation object to
    // edit": a prompt that appeared for no reason they could see, in answer to
    // a question they had not asked.
    const CommandDefinition* definition =
        hasEditableText(*picked) ? document_->commands().lookup("TEXTEDIT") : nullptr;
    if (definition == nullptr) {
        Q_EMIT selectionChanged();
        Q_EMIT commandStateChanged();
        update();
        return;
    }

    document_->executor().run(*definition);
    document_->reindex();
    Q_EMIT selectionChanged();
    Q_EMIT commandStateChanged();
    update();
}

void CanvasWidget::supplyCurrentSelection() {
    if (document_ == nullptr) {
        return;
    }
    document_->executor().supply(InputValue::makeSelection(document_->selection()));
    // The executor clears the selection when the command finished with it.
    Q_EMIT selectionChanged();
    document_->reindex();
    Q_EMIT commandStateChanged();
    update();
}

// ---------------------------------------------------------------------- grips

std::optional<std::pair<Handle, GripPoint>> CanvasWidget::gripAt(
    const QPointF& position) const {
    if (document_ == nullptr) {
        return std::nullopt;
    }
    // Grips belong to the selection, so this is bounded by what the user has
    // picked rather than by the size of the drawing.
    //
    // Wider than the grip is drawn, deliberately. A target that must be hit to
    // the pixel is one a drafter misses and then wonders why nothing happened,
    // and the only thing under a grip is the object it belongs to -- which is
    // already selected, so grabbing the grip is what the click meant anyway.
    constexpr double kGrabPixels = 9.0;
    double best = kGrabPixels;
    std::optional<std::pair<Handle, GripPoint>> found;

    for (const Handle handle : document_->selection()) {
        const Entity* entity = document_->db().getEntity(handle);
        if (entity == nullptr || entity->erased) {
            continue;
        }
        for (const GripPoint& grip : entity->gripPoints()) {
            const render::ScreenPoint screen = toScreen(grip.position);
            const double distance =
                std::hypot(screen.x - position.x(), screen.y - position.y());
            if (distance <= best) {
                best = distance;
                found = std::pair{handle, grip};
            }
        }
    }
    return found;
}

bool CanvasWidget::updateHotGrip(const QPointF& position) {
    const auto found = gripAt(position);
    const bool changed = found.has_value() != hotGrip_.has_value() ||
                         (found && hotGrip_ && (found->first != hotGrip_->first ||
                                                found->second.index != hotGrip_->second.index));
    hotGrip_ = found;
    if (changed) {
        // A hand cursor is the affordance: it says "this is draggable" before
        // the user has to guess.
        setCursor(hotGrip_ ? Qt::OpenHandCursor : Qt::CrossCursor);
    }
    return changed;
}

bool CanvasWidget::isHotGrip(Handle entity, const GripPoint& grip) const {
    return std::any_of(hotGrips_.begin(), hotGrips_.end(), [&](const auto& held) {
        return held.first == entity && held.second.index == grip.index;
    });
}

void CanvasWidget::toggleHotGrip(Handle entity, const GripPoint& grip) {
    const auto at = std::find_if(hotGrips_.begin(), hotGrips_.end(), [&](const auto& held) {
        return held.first == entity && held.second.index == grip.index;
    });
    if (at == hotGrips_.end()) {
        hotGrips_.emplace_back(entity, grip);
    } else {
        hotGrips_.erase(at);
    }
    update();
}

void CanvasWidget::beginGripDrag(Handle handle, const GripPoint& grip) {
    const Entity* entity = document_->db().getEntity(handle);
    if (entity == nullptr) {
        return;
    }
    // A locked layer is visible but not editable, and dragging is exactly the
    // kind of direct edit that has to respect it.
    if (const auto* layer = document_->db().get<Layer>(entity->layerHandle);
        layer != nullptr && layer->isLocked) {
        return;
    }

    GripDrag drag;
    drag.entity = handle;
    drag.grip = grip;
    // The preview is a clone, so nothing in the database changes until the
    // drag is released — which is what makes cancelling free rather than an
    // undo.
    auto clone = entity->clone();
    drag.preview.reset(dynamic_cast<Entity*>(clone.release()));
    if (drag.preview == nullptr) {
        return;
    }
    // Kept so the readout can say what changed rather than only what is.
    auto before = entity->clone();
    drag.original.reset(dynamic_cast<Entity*>(before.release()));
    // Filled in by the caller, which is the only place that knows where the
    // button actually went down. `lastMousePosition_` is a *move* position and
    // a click need not be preceded by one -- taking it from there compared the
    // release against wherever the pointer had last travelled, so every click
    // looked like a long drag and committed the grip where it already was.
    drag.pressedAt = QPointF{};
    // Everything else that was made hot comes along -- but only if the grip
    // being grabbed is one of them.
    //
    // other CAD software's rule, and it matters: marking grips and then taking hold of a
    // *different* one starts over with just that grip. Treating the grabbed
    // grip as one more hot grip made "mark two ends, then drag the middle" move
    // three grips at once, which on an arc is a rigid translation -- not at all
    // what the gesture looks like it should do.
    const bool grabbedIsHot = isHotGrip(handle, grip);
    if (!grabbedIsHot) {
        hotGrips_.clear();
    }
    for (const auto& [otherEntity, otherGrip] : hotGrips_) {
        if (otherEntity == handle && otherGrip.index == grip.index) {
            continue;
        }
        const Entity* source = document_->db().getEntity(otherEntity);
        if (source == nullptr || source->erased) {
            continue;
        }
        auto snapshot = source->clone();
        std::unique_ptr<Entity> kept{dynamic_cast<Entity*>(snapshot.release())};
        if (kept == nullptr) {
            continue;
        }
        drag.companions.push_back(
            GripDrag::Companion{otherEntity, otherGrip, nullptr, std::move(kept)});
    }

    gripDrag_ = std::move(drag);
    setCursor(Qt::ClosedHandCursor);
    // The command line shows what is being held, so it has to be told. Nothing
    // else changes command state here, which is why it never found out.
    Q_EMIT commandStateChanged();
}

namespace {

/// Total length of a curve set, which is what "how long is it now" means for a
/// line or a polyline.
[[nodiscard]] double totalLength(const Entity& entity) {
    double length = 0.0;
    for (const CurvePtr& curve : entity.toCurves()) {
        if (curve != nullptr) {
            length += curve->length();
        }
    }
    return length;
}

/// A number as a drafter reads it: no trailing zeros, no exponent.
[[nodiscard]] QString measure(double value) {
    return QString::number(value, 'f', 3).remove(QRegularExpression{R"(\.?0+$)"});
}

/// `+12.5` or `-3`, because the sign is the point.
[[nodiscard]] QString signedMeasure(double value) {
    const QString text = measure(std::abs(value));
    return (value < 0 ? QStringLiteral("-") : QStringLiteral("+")) + text;
}

}  // namespace

QString CanvasWidget::gripReadout() const {
    if (!gripDrag_ || gripDrag_->preview == nullptr || gripDrag_->original == nullptr) {
        return {};
    }
    const Entity& now = *gripDrag_->preview;
    const Entity& before = *gripDrag_->original;

    // A mode other than Stretch is doing one thing to the whole object, and
    // that one thing is the number worth showing. Reporting a rotated
    // rectangle's *bounding box* is arithmetic about the wrong question: the
    // drafter turning it wants the angle.
    if (gripDrag_->mode != GripMode::Stretch) {
        const Vec3 origin = gripDrag_->grip.position;
        const Vec3 offset = constrainedWorldAt(cursorPosition_) - origin;
        const double degrees = radToDeg(std::atan2(offset.y, offset.x));
        const double positive = degrees < 0 ? degrees + 360.0 : degrees;
        switch (gripDrag_->mode) {
            case GripMode::Move:
                return QObject::tr("moved %1, %2   distance %3")
                    .arg(signedMeasure(offset.x), signedMeasure(offset.y),
                         measure(offset.length()));
            case GripMode::Rotate:
                return QObject::tr("rotate %1°").arg(measure(positive));
            case GripMode::Scale: {
                const double reference =
                    gripDrag_->reference > 1e-9 ? gripDrag_->reference : 1.0;
                return QObject::tr("scale ×%1").arg(measure(offset.length() / reference));
            }
            case GripMode::Mirror:
                return QObject::tr("mirror about %1°").arg(measure(positive));
            case GripMode::Stretch:
                break;
        }
    }

    // A circle is a radius, and saying so is more use than saying where its
    // quadrant point went.
    if (const auto* circle = dynamic_cast<const CircleEntity*>(&now)) {
        const auto* was = dynamic_cast<const CircleEntity*>(&before);
        const double delta = was == nullptr ? 0.0 : circle->radius - was->radius;
        return QObject::tr("R %1  (%2)   ⌀ %3")
            .arg(measure(circle->radius), signedMeasure(delta),
                 measure(circle->radius * 2.0));
    }

    // An arc has a radius and a sweep, and moving an end grip changes the
    // sweep while moving the centre does not.
    if (const auto* arc = dynamic_cast<const ArcEntity*>(&now)) {
        const double sweep = std::abs(radToDeg(arc->endAngle - arc->startAngle));
        return QObject::tr("R %1   %2°   length %3")
            .arg(measure(arc->radius), measure(sweep), measure(totalLength(now)));
    }

    // A rectangle is two sides; any other polyline is a run of segments whose
    // total is the useful figure. Told apart by shape rather than by how it
    // was drawn, because a four-cornered closed polyline *is* a rectangle to
    // the person adjusting it, whatever command made it.
    if (const auto* poly = dynamic_cast<const PolylineEntity*>(&now)) {
        const Box3 box = now.bounds();
        if (poly->closed && poly->vertices.size() == 4) {
            const Box3 wasBox = before.bounds();
            return QObject::tr("%1 × %2   (%3, %4)")
                .arg(measure(box.width()), measure(box.height()),
                     signedMeasure(box.width() - wasBox.width()),
                     signedMeasure(box.height() - wasBox.height()));
        }
        const double length = totalLength(now);
        return QObject::tr("length %1  (%2)   %3 segments")
            .arg(measure(length), signedMeasure(length - totalLength(before)))
            .arg(poly->vertices.size() > 1 ? poly->vertices.size() - (poly->closed ? 0 : 1) : 0);
    }

    // Everything with a length: a line, and anything else that reduces to
    // curves. The distance the grip itself moved is reported alongside, since
    // "extend this by exactly 250" is the question being answered.
    const double length = totalLength(now);
    if (length > 0.0) {
        const double travelled = gripDrag_->grip.position.distanceTo(
            now.gripPoints().size() > gripDrag_->grip.index
                ? now.gripPoints()[gripDrag_->grip.index].position
                : gripDrag_->grip.position);
        return QObject::tr("length %1  (%2)   moved %3")
            .arg(measure(length), signedMeasure(length - totalLength(before)),
                 measure(travelled));
    }
    return {};
}

bool CanvasWidget::supplyGripInput(const QString& text) {
    if (!gripDrag_ || document_ == nullptr) {
        return false;
    }
    const QString entry = text.trimmed();
    if (entry.isEmpty()) {
        return false;
    }

    // The options other CAD software offers at a grip prompt, by their initials, which is
    // how they are actually typed.
    const QString option = entry.toLower();
    if (option == "c" || option == "copy" || option == "x" || option == "exit" ||
        option == "u" || option == "undo" || option == "b" || option == "base") {
        applyGripOption(option);
        return true;
    }

    // Measured from the base point, which is the grip's own unless one was
    // named. Using the grip regardless made a typed angle meaningless the
    // moment `Base point` was used: 90 degrees about a base four hundred units
    // away came out as a fifth of a degree, and the line collapsed.
    const Vec3 from = gripDrag_->base.value_or(gripDrag_->grip.position);
    const auto number = [](const QString& part, bool& ok) {
        return part.trimmed().toDouble(&ok);
    };

    // `x,y` and `@dx,dy` — a point, absolutely or from where the grip started.
    const bool relative = entry.startsWith('@');
    const QString body = relative ? entry.mid(1) : entry;
    if (const QStringList parts = body.split(','); parts.size() == 2) {
        bool okX = false;
        bool okY = false;
        const double x = number(parts[0], okX);
        const double y = number(parts[1], okY);
        if (!okX || !okY) {
            return false;
        }
        commitGripDragAt(relative ? Vec3{from.x + x, from.y + y, from.z} : Vec3{x, y, from.z});
        return true;
    }

    // A bare number, meaning whatever the mode is asking for. This is the one
    // that matters: it is how a drafter extends a wall by exactly 250, or turns
    // a part exactly 90, without computing where that lands.
    bool ok = false;
    const double value = number(entry, ok);
    if (!ok) {
        return false;
    }

    // Rotate takes an angle and Scale a factor, as they do at other CAD software's own
    // prompts. Feeding either of them a distance would be treating the number
    // as though the mode had not been chosen.
    if (gripDrag_->mode == GripMode::Rotate) {
        commitGripDragAt(from + Vec3{std::cos(degToRad(value)), std::sin(degToRad(value)), 0.0});
        return true;
    }
    if (gripDrag_->mode == GripMode::Scale) {
        if (value <= 1e-9) {
            // A factor of zero collapses the object to a point, which is never
            // what was meant and cannot be undone by typing another number.
            return false;
        }
        const double reference = gripDrag_->reference > 1e-9 ? gripDrag_->reference : 1.0;
        Vec3 heading = worldAt(cursorPosition_) - from;
        if (heading.length() < 1e-9) {
            heading = Vec3{1.0, 0.0, 0.0};
        }
        commitGripDragAt(from + heading.normalized() * (value * reference));
        return true;
    }

    const double distance = value;
    Vec3 direction = worldAt(cursorPosition_) - from;
    if (direction.length() < 1e-9) {
        // No direction to go in: the pointer is still on the grip. Refused
        // rather than guessed, because guessing would move geometry somewhere
        // the user did not point.
        return false;
    }
    direction = direction.normalized();
    commitGripDragAt(from + direction * distance);
    return true;
}

void CanvasWidget::applyGripEdit(Entity& target, const GripPoint& grip, GripMode mode,
                                 double reference, const Vec3& destination, const Vec3& base) {
    const Vec3 origin = base;
    const Vec3 offset = destination - origin;

    switch (mode) {
        case GripMode::Stretch:
            target.moveGrip(grip, destination);
            return;
        case GripMode::Move:
            target.transformBy(Mat4::translation(offset));
            return;
        case GripMode::Rotate:
            // About the grip, by the angle from it to the point. The grip is
            // the base point, which is what holding one already means.
            if (offset.length() > 1e-9) {
                target.transformBy(Mat4::rotationZ(std::atan2(offset.y, offset.x), origin));
            }
            return;
        case GripMode::Scale: {
            // Measured against how far the pointer was when Scale was entered,
            // so the shape starts at its own size rather than jumping to
            // whatever fraction of a drawing unit the cursor happened to be at.
            const double unit = reference > 1e-9 ? reference : 1.0;
            const double factor = offset.length() / unit;
            if (factor > 1e-9) {
                target.transformBy(Mat4::scaling(factor, origin));
            }
            return;
        }
        case GripMode::Mirror:
            if (offset.length() > 1e-9) {
                target.transformBy(Mat4::mirror2(origin, offset));
            }
            return;
    }
}

std::vector<std::pair<Handle, std::vector<GripPoint>>> CanvasWidget::hotGripGroups() const {
    std::vector<std::pair<Handle, std::vector<GripPoint>>> groups;
    if (!gripDrag_) {
        return groups;
    }
    // The held grip's object first, so it stays the readout subject and the
    // one whose preview the rest are drawn against.
    groups.emplace_back(gripDrag_->entity, std::vector<GripPoint>{gripDrag_->grip});
    for (const GripDrag::Companion& companion : gripDrag_->companions) {
        const auto existing = std::find_if(
            groups.begin(), groups.end(),
            [&](const auto& group) { return group.first == companion.entity; });
        if (existing == groups.end()) {
            groups.emplace_back(companion.entity, std::vector<GripPoint>{companion.grip});
            continue;
        }
        // Already gathered under its object. Guarded against the same grip
        // arriving twice, which would move it twice under a stretch.
        const bool already = std::any_of(
            existing->second.begin(), existing->second.end(),
            [&](const GripPoint& grip) { return grip.index == companion.grip.index; });
        if (!already) {
            existing->second.push_back(companion.grip);
        }
    }
    return groups;
}

void CanvasWidget::applyHotGrips(Entity& target, const std::vector<GripPoint>& grips,
                                 GripMode mode, double reference, const Vec3& heldGrip,
                                 const Vec3& destination, const Vec3& base) {
    if (grips.empty()) {
        return;
    }

    if (mode != GripMode::Stretch) {
        // Once per object, however many of its grips are hot. Move, Rotate,
        // Scale and Mirror transform the whole object, so running one per grip
        // moved a two-grip selection exactly twice as far as it was dragged.
        applyGripEdit(target, grips.front(), mode, reference, destination, base);
        return;
    }

    // The grips that carry the whole object go first.
    //
    // A circle's centre moves everything, including its quadrants; a quadrant
    // only sets a radius, and it sets it *from the centre*. With both hot and
    // the centre applied second, the quadrant had already re-measured the
    // radius against the old centre and a circle dragged by its middle came out
    // smaller. Moved first, the centre carries the quadrant with it and the
    // radius is left alone -- which is what dragging a circle by its centre
    // should do however many of its grips are marked.
    //
    // Beyond that, highest index first, purely so the order is deterministic.
    const auto carriesObject = [](GripKind kind) {
        return kind == GripKind::Center || kind == GripKind::Insertion;
    };
    // The held grip goes last, and is the one grip applied come what may.
    //
    // On shapes whose grips are not independent -- an arc's endpoints share a
    // radius, so moving either moves the other -- an earlier grip can carry the
    // held one somewhere the pointer is not. Applying it last, and without the
    // position check the others get, keeps the one invariant a drafter would
    // notice: the grip you are dragging ends up under the cursor. The rest
    // follow as far as the shape allows.
    const auto isHeld = [&](const GripPoint& grip) {
        return grip.position.equals(heldGrip) && !carriesObject(grip.kind);
    };
    std::vector<GripPoint> ordered = grips;
    std::sort(ordered.begin(), ordered.end(),
              [&](const GripPoint& a, const GripPoint& b) {
                  if (carriesObject(a.kind) != carriesObject(b.kind)) {
                      return carriesObject(a.kind);
                  }
                  if (isHeld(a) != isHeld(b)) {
                      return !isHeld(a);
                  }
                  return a.index > b.index;
              });

    std::vector<GripMove> moves;
    moves.reserve(ordered.size());
    for (const GripPoint& grip : ordered) {
        moves.push_back({grip, companionDestination(GripMode::Stretch, heldGrip, grip.position,
                                                    destination)});
    }
    // Handed over as one edit. The entity decides how to satisfy them, which
    // for a shape whose grips share a centre and a radius means solving rather
    // than applying them in turn -- see `ArcEntity::moveGrips`.
    target.moveGrips(moves);
}

Vec3 CanvasWidget::companionDestination(GripMode mode, const Vec3& heldGrip,
                                        const Vec3& companionGrip, const Vec3& destination) {
    // A stretch moves every hot grip by the same displacement: three wall ends
    // picked together and pulled twelve inches all move twelve inches. Sending
    // them all to the same point instead would pinch them into one, which is a
    // different operation and not one anybody asks for by dragging a grip.
    if (mode != GripMode::Stretch) {
        // The rest are transforms about a shared base, so they already act
        // alike on every object and the destination passes through untouched.
        return destination;
    }
    return companionGrip + (destination - heldGrip);
}

void CanvasWidget::placeGripCopy(const Vec3& destination) {
    if (!gripDrag_ || document_ == nullptr || gripDrag_->original == nullptr) {
        return;
    }
    // From the *original* rather than from the last copy, so eight copies are
    // eight edits of one shape rather than a chain in which each is derived
    // from the one before and the error compounds.
    auto clone = gripDrag_->original->clone();
    std::unique_ptr<Entity> copy{dynamic_cast<Entity*>(clone.release())};
    if (copy == nullptr) {
        return;
    }
    applyGripEdit(*copy, gripDrag_->grip, gripDrag_->mode, gripDrag_->reference, destination,
                  gripDrag_->base.value_or(gripDrag_->grip.position));

    document_->transactions().run("Grip copy", [&](Transaction& transaction) {
        transaction.addEntity(std::move(copy), document_->activeSpace());
    });
    document_->reindex();
    Q_EMIT commandStateChanged();
    update();
}

void CanvasWidget::applyGripOption(const QString& option) {
    if (!gripDrag_ || document_ == nullptr) {
        return;
    }
    const QString name = option.toLower();
    if (name == "c" || name == "copy") {
        gripDrag_->copying = true;
    } else if (name == "b" || name == "base" || name == "base point") {
        // The *next* click names the base rather than finishing the edit.
        gripDrag_->awaitingBase = true;
    } else if (name == "u" || name == "undo") {
        // Takes back the last copy placed, which is what is reached for after
        // putting one in the wrong place halfway through a run.
        document_->transactions().undo();
        document_->reindex();
    } else if (name == "x" || name == "exit") {
        cancelGripDrag();
        return;
    }
    Q_EMIT commandStateChanged();
    update();
}

void CanvasWidget::cycleGripMode() {
    if (!gripDrag_) {
        return;
    }
    constexpr std::array<GripMode, 5> kOrder{GripMode::Stretch, GripMode::Move, GripMode::Rotate,
                                             GripMode::Scale, GripMode::Mirror};
    const auto at = std::find(kOrder.begin(), kOrder.end(), gripDrag_->mode);
    const auto next = at == kOrder.end() || at + 1 == kOrder.end() ? kOrder.begin() : at + 1;
    gripDrag_->mode = *next;

    if (gripDrag_->mode == GripMode::Scale) {
        // The distance now is the 1.0 the factor is measured from.
        gripDrag_->reference = (worldAt(cursorPosition_) - gripDrag_->grip.position).length();
    }

    // The preview has to be rebuilt: the same pointer position means something
    // entirely different in the new mode.
    updateGripDrag(constrainedWorldAt(cursorPosition_));
    Q_EMIT commandStateChanged();
    update();
}

QStringList CanvasWidget::gripModes() {
    // The order the Space bar cycles, so the menu and the key agree about what
    // "next" means.
    return {QStringLiteral("STRETCH"), QStringLiteral("MOVE"), QStringLiteral("ROTATE"),
            QStringLiteral("SCALE"), QStringLiteral("MIRROR")};
}

QString CanvasWidget::currentGripMode() const {
    if (!gripDrag_) {
        return {};
    }
    switch (gripDrag_->mode) {
        case GripMode::Stretch: return QStringLiteral("STRETCH");
        case GripMode::Move:    return QStringLiteral("MOVE");
        case GripMode::Rotate:  return QStringLiteral("ROTATE");
        case GripMode::Scale:   return QStringLiteral("SCALE");
        case GripMode::Mirror:  return QStringLiteral("MIRROR");
    }
    return {};
}

void CanvasWidget::setGripMode(const QString& name) {
    if (!gripDrag_) {
        return;
    }
    const int at = gripModes().indexOf(name.toUpper());
    if (at < 0) {
        return;
    }
    constexpr std::array<GripMode, 5> kOrder{GripMode::Stretch, GripMode::Move, GripMode::Rotate,
                                             GripMode::Scale, GripMode::Mirror};
    gripDrag_->mode = kOrder.at(static_cast<std::size_t>(at));

    if (gripDrag_->mode == GripMode::Scale) {
        // The distance now is the 1.0 the factor is measured from -- the same
        // reading Space takes, for the same reason: without it the shape jumps
        // to whatever fraction of a unit the cursor happens to sit at.
        gripDrag_->reference = (worldAt(cursorPosition_) - gripDrag_->grip.position).length();
    }

    // The pointer has not moved but now means something else, so the preview
    // has to be rebuilt from it.
    updateGripDrag(constrainedWorldAt(cursorPosition_));
    Q_EMIT commandStateChanged();
    update();
}

QString CanvasWidget::gripModeName() const {
    if (!gripDrag_) {
        return {};
    }
    QString suffix;
    if (gripDrag_->awaitingBase) {
        suffix = tr(" — pick base point");
    } else if (gripDrag_->copying) {
        suffix = tr(" (multiple)");
    }
    return currentGripMode() + suffix;
}

void CanvasWidget::updateGripDrag(const Vec3& world) {
    if (!gripDrag_ || document_ == nullptr) {
        return;
    }
    // Rebuilt from the original each time rather than nudged, so a drag that
    // wanders does not accumulate the intermediate positions.
    const Entity* entity = document_->db().getEntity(gripDrag_->entity);
    if (entity == nullptr) {
        cancelGripDrag();
        return;
    }
    // One preview per *object*, with all of that object's hot grips applied to
    // it. Previewing per grip drew a rectangle with two hot corners as a bow
    // tie: two ghosts, each with one corner moved and the other still home.
    const std::vector<std::pair<Handle, std::vector<GripPoint>>> groups = hotGripGroups();
    std::size_t companionIndex = 0;
    for (const auto& [handle, grips] : groups) {
        const Entity* source = document_->db().getEntity(handle);
        if (source == nullptr) {
            continue;
        }
        auto copy = source->clone();
        std::unique_ptr<Entity> shadow{dynamic_cast<Entity*>(copy.release())};
        if (shadow == nullptr) {
            continue;
        }
        applyHotGrips(*shadow, grips, gripDrag_->mode, gripDrag_->reference,
                      gripDrag_->grip.position, world,
                      gripDrag_->base.value_or(gripDrag_->grip.position));
        if (handle == gripDrag_->entity) {
            gripDrag_->preview = std::move(shadow);
            continue;
        }
        // The remaining groups are drawn from the companion slots. There is one
        // slot per hot grip and one group per object, so the slots beyond the
        // groups are left empty and simply draw nothing.
        while (companionIndex < gripDrag_->companions.size() &&
               gripDrag_->companions[companionIndex].entity == gripDrag_->entity) {
            gripDrag_->companions[companionIndex].preview.reset();
            ++companionIndex;
        }
        if (companionIndex < gripDrag_->companions.size()) {
            gripDrag_->companions[companionIndex].preview = std::move(shadow);
            ++companionIndex;
        }
    }
    // Any slot not filled this pass belongs to a grip already folded into a
    // group, and must not keep last frame's ghost.
    for (std::size_t i = companionIndex; i < gripDrag_->companions.size(); ++i) {
        gripDrag_->companions[i].preview.reset();
    }
}

void CanvasWidget::commitGripDrag() {
    // Where the pointer is, which is what a click means. A typed distance names
    // its own destination instead -- see `commitGripDragAt`, which this is a
    // thin wrapper over. Recomputing the cursor position *inside* the commit
    // was why a typed distance did nothing: the number moved the preview and
    // the commit then threw the preview away and used the pointer.
    if (gripDrag_ && document_ != nullptr) {
        commitGripDragAt(constrainedWorldAt(cursorPosition_));
    }
}

void CanvasWidget::commitGripDragAt(const Vec3& destination) {
    if (!gripDrag_ || document_ == nullptr) {
        return;
    }
    // Emitted whichever way this returns, so the prompt stops saying a grip is
    // held the moment one is not.
    const struct Notify {
        CanvasWidget* canvas;
        ~Notify() { Q_EMIT canvas->commandStateChanged(); }
    } notify{this};
    const Handle handle = gripDrag_->entity;
    const GripPoint grip = gripDrag_->grip;
    const GripMode mode = gripDrag_->mode;
    const double reference = gripDrag_->reference;
    const Vec3 base = gripDrag_->base.value_or(grip.position);
    // Gathered before the drag is released, because the grouping reads it.
    const std::vector<std::pair<Handle, std::vector<GripPoint>>> groups = hotGripGroups();
    // Spent with the edit they were marked for.
    hotGrips_.clear();

    // Copying keeps hold of the grip: the original stays where it is, a copy
    // lands here, and the next click lands another. Ending after the first one
    // would make `Copy` a synonym for "do it once and leave the original",
    // which is not what it is for.
    if (gripDrag_->copying) {
        placeGripCopy(destination);
        return;
    }

    gripDrag_.reset();
    // Blank: the canvas draws its own crosshair. The system cross would sit on
    // top of ours at a size we do not control -- two cursors, and a
    // contradiction of the size setting.
    setCursor(Qt::BlankCursor);

    Entity* entity = document_->db().getEntity(handle);
    if (entity == nullptr || (mode == GripMode::Stretch && grip.position.equals(destination))) {
        // Dropped where it started: a click on a grip, not a drag. Leaving no
        // undo entry is the difference between a tidy history and one full of
        // "Grip edit" steps that changed nothing.
        update();
        return;
    }

    Transaction& transaction = document_->transactions().begin("Grip edit");
    try {
        // One modification per object, carrying all of that object's hot grips.
        // All in one transaction, so several grips moved together undo
        // together: separate ones would make a single gesture take as many
        // undos as it happened to touch objects.
        for (const auto& [target, grips] : groups) {
            Entity* other = document_->db().getEntity(target);
            if (other == nullptr || other->erased) {
                continue;
            }
            const std::vector<GripPoint>& applied = grips;
            transaction.modifyEntity(*other, [&](Entity& subject) {
                applyHotGrips(subject, applied, mode, reference, grip.position, destination,
                              base);
            });
        }
        document_->transactions().commit(transaction);
    } catch (const std::exception&) {
        document_->transactions().abort(transaction);
    }

    document_->reindex();
    Q_EMIT selectionChanged();
    Q_EMIT commandStateChanged();
    update();
}

bool CanvasWidget::cancelGripEdit() {
    if (gripDrag_) {
        cancelGripDrag();
        return true;
    }
    if (!hotGrips_.empty()) {
        hotGrips_.clear();
        update();
        return true;
    }
    return false;
}

void CanvasWidget::cancelGripDrag() {
    gripDrag_.reset();
    // The rollover is frozen while a grip is held -- see `updateHoveredEntity`
    // -- so it is stale the moment the hold ends, and would keep highlighting
    // whatever the drag started on until the pointer next moved.
    updateHoveredEntity(cursorPosition_);
    // Blank: the canvas draws its own crosshair. The system cross would sit on
    // top of ours at a size we do not control -- two cursors, and a
    // contradiction of the size setting.
    setCursor(Qt::BlankCursor);
    update();
}

void CanvasWidget::drawGripPreview(QPainter& painter) const {
    if (!gripDrag_ || gripDrag_->preview == nullptr) {
        return;
    }
    QPen pen{currentTheme().highlight};
    pen.setWidthF(1.5);
    pen.setCosmetic(true);
    pen.setStyle(Qt::DashLine);
    painter.setPen(pen);

    TessellationOptions tessellation;
    tessellation.chordTolerance = render::chordToleranceFor(viewport_);

    const auto outline = [&](const Entity& entity) {
        for (const CurvePtr& curve : entity.toCurves()) {
            const QPolygonF polygon = toPolygon(viewport_, curve->tessellate(tessellation));
            if (curve->isClosed()) {
                painter.drawPolygon(polygon);
            } else {
                painter.drawPolyline(polygon);
            }
        }
    };

    outline(*gripDrag_->preview);
    // The companions are drawn the same way: several hot grips are one gesture,
    // and showing only the one under the pointer would hide most of it.
    for (const GripDrag::Companion& companion : gripDrag_->companions) {
        if (companion.preview != nullptr) {
            outline(*companion.preview);
        }
    }
}

void CanvasWidget::handleSelectionClick(const QPointF& position, bool subtractive) {
    if (document_ == nullptr) {
        return;
    }

    // Whatever the rollover outline is showing is what a click gets — including
    // after cycling. Re-picking here would quietly select the topmost object
    // again and make Shift+Space look broken.
    std::optional<Handle> hit;
    if (hoveredEntity_.has_value()) {
        hit = hoveredEntity_;
    } else {
        const Vec3 world = toWorld({position.x(), position.y()});
        if (const auto found = render::pickEntity(document_->db(), document_->index(), world,
                                                  apertureWorld())) {
            hit = found->handle;
        }
    }

    // other CAD software's rule, because the muscle memory is worth more here than the
    // Windows one: picks accumulate, and Shift takes one back out. A drafter
    // who has to hold a modifier to select a second object has to relearn the
    // most frequent gesture in the program.
    if (!hit.has_value()) {
        if (!subtractive) {
            document_->clearSelection();
        }
    } else if (subtractive) {
        if (document_->isSelected(*hit)) {
            document_->toggleSelection(*hit);
        }
    } else if (!document_->isSelected(*hit)) {
        document_->toggleSelection(*hit);
    }

    Q_EMIT selectionChanged();
    update();
}

void CanvasWidget::finishSelectionRectangle(const QPointF& position, bool subtractive) {
    if (document_ == nullptr) {
        return;
    }

    const Vec3 origin = toWorld({selectionOrigin_.x(), selectionOrigin_.y()});
    const Vec3 corner = toWorld({position.x(), position.y()});

    // Dragging right-to-left is a crossing selection, left-to-right a window.
    // The distinction is not cosmetic — drafters rely on it constantly.
    const bool crossing = position.x() < selectionOrigin_.x();

    std::vector<Handle> hits = render::pickWindow(
        document_->db(), document_->index(), Box3::fromCorners(origin, corner), crossing);

    // Same rule as a single pick: a window adds to what is already picked, and
    // Shift+window removes. Replacing the selection would undo the box the
    // drafter drew a moment ago.
    for (const Handle handle : hits) {
        if (document_->isSelected(handle) == subtractive) {
            document_->toggleSelection(handle);
        }
    }

    Q_EMIT selectionChanged();
    update();
}

void CanvasWidget::mousePressEvent(QMouseEvent* event) {
    const QPointF position = event->position();

    // A click somewhere else on the drawing finishes an in-place edit. This is
    // the event that actually means "done" — see InPlaceTextEditor::commit for
    // why losing focus is not it. The click is then consumed, because a user
    // whose click ended an edit did not also mean to select whatever was under
    // it.
    if (textEditor_ != nullptr && textEditor_->isEditing()) {
        textEditor_->commitNow();
        return;
    }

    // No setFocus: clicking the drawing must not take the keyboard away from
    // the command line, or the coordinate the user types next would go
    // nowhere.

    // Middle-drag pans, matching every CAD application; left is reserved for
    // picking and command input.
    if (event->button() == Qt::MiddleButton ||
        (event->button() == Qt::LeftButton && (event->modifiers() & Qt::AltModifier))) {
        panning_ = true;
        lastMousePosition_ = position;
        setCursor(Qt::ClosedHandCursor);
        return;
    }

    if (event->button() == Qt::LeftButton && document_ != nullptr) {
        if (document_->executor().isRunning()) {
            // What a click means depends on what the command asked for. A
            // command wanting an object gets the one under the cursor, not the
            // coordinates of the click.
            const InputRequest* request = document_->executor().currentRequest();
            const bool subtractive = (event->modifiers() & Qt::ShiftModifier) != 0;
            if (request != nullptr && request->kind == InputRequest::Kind::Entity) {
                supplyEntityAt(position);
                return;
            }
            if (request != nullptr && request->kind == InputRequest::Kind::Selection) {
                // Clicks build the selection; Enter or right-click hands it
                // over, matching how selection works everywhere else.
                handleSelectionClick(position, subtractive);
                return;
            }
            updateSnap(position);
            supplyPoint(constrainedWorldAt(position));
            return;
        }

        // Shift over a grip marks it hot rather than adding to the selection:
        // the pointer is on a grip, and the object it belongs to is already
        // selected, so "add this object" is not what the click can mean.
        if (!gripDrag_ && (event->modifiers() & Qt::ShiftModifier) != 0) {
            if (const auto grip = gripAt(position)) {
                toggleHotGrip(grip->first, grip->second);
                return;
            }
        }

        // Naming a base point, rather than finishing the edit.
        if (gripDrag_ && gripDrag_->awaitingBase) {
            updateSnap(position);
            gripDrag_->base = worldAt(position);
            gripDrag_->awaitingBase = false;
            // This press is spent on the base point, so the release that
            // follows it must not also finish the edit. Re-arming the click
            // makes the release measure no travel and keep the grip held --
            // without it, naming a base point committed the edit at the base
            // point, and everything typed afterwards arrived too late.
            gripDrag_->pressedAt = position;
            gripDrag_->heldByClick = true;
            gripDrag_->ignoreRelease = true;
            updateGripDrag(constrainedWorldAt(position));
            Q_EMIT commandStateChanged();
            update();
            return;
        }

        // A grip already held by a click is put down by the next one.
        if (gripDrag_ && gripDrag_->heldByClick) {
            updateSnap(position);
            updateGripDrag(constrainedWorldAt(position));
            commitGripDrag();
            // The release that follows must not commit as well. Usually the
            // commit ends the hold and there is nothing left to commit, but
            // `Copy` deliberately keeps holding so the next click makes
            // another -- and the release of *this* click was making one too,
            // so every click placed two copies on top of each other.
            if (gripDrag_) {
                gripDrag_->ignoreRelease = true;
            }
            return;
        }

        // A press on a grip starts a direct edit. Checked before the rubber
        // band, because a grip sits on top of the entity it belongs to and the
        // user aiming at it does not want a selection rectangle.
        if (const auto grip = gripAt(position)) {
            beginGripDrag(grip->first, grip->second);
            if (gripDrag_) {
                gripDrag_->pressedAt = position;
                return;
            }
        }

        // No command running: arm a rubber band. Whether it becomes a click or
        // a drag is decided on release, so a click never has to wait.
        selecting_ = true;
        selectionOrigin_ = position;
        return;
    }

    if (event->button() == Qt::RightButton) {
        if (document_ == nullptr) {
            return;
        }
        // Shift with the right button asks *how* to place the next point --
        // other CAD software's snap override menu, and the fastest way to say "that
        // endpoint, not whatever is nearest".
        if ((event->modifiers() & Qt::ShiftModifier) != 0) {
            Q_EMIT snapOverrideRequested(event->globalPosition().toPoint());
            return;
        }

        // Time-sensitive, as other CAD software's right button is: a quick click means
        // Enter, and holding it means "show me what I can do here". Deciding on
        // release would make the menu arrive after the user had already waited
        // for it, so the timer fires while the button is still down.
        rightPressAt_ = event->position();
        rightHeld_ = true;
        rightHoldTimer_.start(kRightHoldMilliseconds, this);
        return;
    }

    QWidget::mousePressEvent(event);
}

void CanvasWidget::timerEvent(QTimerEvent* event) {
    if (event->timerId() != rightHoldTimer_.timerId()) {
        QWidget::timerEvent(event);
        return;
    }
    rightHoldTimer_.stop();
    if (!rightHeld_) {
        return;
    }
    // Held long enough to be a request for the menu rather than an Enter. The
    // button is still down; releasing it must not then also repeat a command.
    rightHeld_ = false;
    Q_EMIT contextMenuRequested(mapToGlobal(rightPressAt_.toPoint()));
}

void CanvasWidget::mouseMoveEvent(QMouseEvent* event) {
    const QPointF position = event->position();
    cursorPosition_ = position;
    hasCursor_ = true;

    if (panning_) {
        const QPointF delta = position - lastMousePosition_;
        if (ViewportEntity* window = activeWindow()) {
            // The model slides behind the frame by what the cursor moved.
            const Vec3 before = toWorld({lastMousePosition_.x(), lastMousePosition_.y()});
            const Vec3 after = toWorld({position.x(), position.y()});
            window->viewCenter = window->viewCenter - (after - before);
            refreshSpaceTransform();
        } else {
            viewport_ = viewport_.panByPixels(delta.x(), delta.y());
        }
        lastMousePosition_ = position;
        Q_EMIT viewChanged();
        update();
        return;
    }

    if (gripDrag_) {
        // Snapping applies to a grip edit exactly as it does to a picked
        // point; putting an endpoint onto another one is the whole reason
        // endpoint snap exists.
        updateSnap(position);
        updateGripDrag(constrainedWorldAt(position));
        update();
    } else if (document_ != nullptr && document_->executor().isRunning()) {
        updateSnap(position);
        // The rollover has to stay live while a command is picking objects.
        // `handleSelectionClick` takes whatever the rollover is showing -- that
        // is how Shift+Space cycling survives the click -- so leaving it
        // unrefreshed meant every click during a selection prompt resolved to
        // whatever had been hovered *before the command started*. Hover one
        // object, type ERASE, click a different one, and the first was erased.
        if (const InputRequest* request = document_->executor().currentRequest();
            request != nullptr && (request->kind == InputRequest::Kind::Selection ||
                                   request->kind == InputRequest::Kind::Entity)) {
            updateHoveredEntity(position);
        }
        // Feeding the cursor refreshes the command's rubber-band preview.
        document_->executor().moveCursor(constrainedWorldAt(position));
        update();
    } else if (selecting_) {
        update();
    } else if (document_ != nullptr) {
        // Snap indicators are shown even outside a command, so the user can
        // see what a click would latch onto — and the rollover outline says
        // *which object* it would latch onto, which the markers do not.
        updateSnap(position);
        updateHotGrip(position);
        updateHoveredEntity(position);
        update();
    }

    // The prompt follows the cursor while it is showing.
    if (dynamicInput_->isVisible()) {
        dynamicInput_->move(promptAnchor() + QPoint{18, 14});
    }

    // The constrained point, not the raw pointer: the readout should say where
    // a click would put something. Reporting the pointer meant that, held on a
    // tracking path or a polar angle, the numbers disagreed with the geometry
    // about to be drawn -- and the readout is what a drafter checks to find out
    // exactly that.
    const Vec3 world = constrainedWorldAt(position);
    Q_EMIT cursorMoved(world.x, world.y);
    QWidget::mouseMoveEvent(event);
}

void CanvasWidget::mouseReleaseEvent(QMouseEvent* event) {
    const QPointF position = event->position();

    if (panning_) {
        panning_ = false;
        // Blank: the canvas draws its own crosshair. The system cross would sit on
    // top of ours at a size we do not control -- two cursors, and a
    // contradiction of the size setting.
    setCursor(Qt::BlankCursor);
        return;
    }

    if (event->button() == Qt::RightButton && rightHeld_) {
        rightHeld_ = false;
        rightHoldTimer_.stop();
        if (document_ == nullptr) {
            return;
        }
        if (document_->executor().isRunning()) {
            // While a command is selecting, right-click finishes the selection
            // rather than repeating anything — other CAD software reflex.
            const InputRequest* request = document_->executor().currentRequest();
            if (request != nullptr && request->kind == InputRequest::Kind::Selection) {
                supplyCurrentSelection();
            }
            return;
        }
        // Right-click repeats the last command, as other CAD software does.
        document_->executor().repeatLast();
        Q_EMIT commandStateChanged();
        update();
        return;
    }

    if (gripDrag_ && event->button() == Qt::LeftButton && gripDrag_->ignoreRelease) {
        // The press this belongs to was spent naming a base point. Letting it
        // through would finish the edit *at* the base point, which is the one
        // place the grip was never going.
        gripDrag_->ignoreRelease = false;
        return;
    }

    if (gripDrag_ && event->button() == Qt::LeftButton) {
        // A release the pointer has not travelled from is a *click*, and a
        // click picks the grip up rather than putting it down. The next click
        // places it. Dragging still works for anyone who prefers it: travel
        // further than the pick aperture before letting go and the release
        // finishes the edit, exactly as it used to.
        const QPointF travel = event->position() - gripDrag_->pressedAt;
        const double moved = std::hypot(travel.x(), travel.y());
        if (!gripDrag_->heldByClick && moved <= aperturePixels_) {
            gripDrag_->heldByClick = true;
            setCursor(Qt::BlankCursor);
            update();
            return;
        }
        commitGripDrag();
        return;
    }

    if (selecting_ && event->button() == Qt::LeftButton) {
        selecting_ = false;
        const bool subtractive = (event->modifiers() & Qt::ShiftModifier) != 0;

        // A drag shorter than the aperture is a click, not a rectangle. Without
        // this, a click with one pixel of hand tremor selects nothing.
        const QPointF delta = position - selectionOrigin_;
        if (std::hypot(delta.x(), delta.y()) < aperturePixels_ / 2.0) {
            handleSelectionClick(position, subtractive);
        } else {
            finishSelectionRectangle(position, subtractive);
        }
        return;
    }

    QWidget::mouseReleaseEvent(event);
}

void CanvasWidget::wheelEvent(QWheelEvent* event) {
    const double steps = event->angleDelta().y() / 120.0;
    if (steps == 0.0) {
        return;
    }
    // Zoom about the cursor, so the world point under it stays put. Zooming
    // about the view centre would force a pan after every wheel notch.
    const QPointF position = event->position();
    const double factor = std::pow(1.2, steps);
    if (ViewportEntity* window = activeWindow()) {
        // Inside a viewport the model zooms behind a fixed frame: the model
        // point under the cursor stays under it, the frame does not move.
        const Vec3 anchor = toWorld({position.x(), position.y()});
        const Vec3 center = window->viewTarget + window->viewCenter;
        const Vec3 kept = anchor + (center - anchor) * (1.0 / factor);
        window->viewCenter = kept - window->viewTarget;
        window->viewHeight /= factor;
        refreshSpaceTransform();
        Q_EMIT viewChanged();
        update();
        event->accept();
        return;
    }
    viewport_ = viewport_.zoomAt({position.x(), position.y()}, factor);

    Q_EMIT viewChanged();
    update();
    event->accept();
}

void CanvasWidget::keyPressEvent(QKeyEvent* event) {
    // The canvas does not take focus, so this runs only if something has
    // deliberately given it the keyboard. Escape still has to work then,
    // because that is how a grip drag is abandoned.
    if (event->key() == Qt::Key_Escape && gripDrag_) {
        cancelGripDrag();
        return;
    }
    QWidget::keyPressEvent(event);
}

// -------------------------------------------------------------------- drawing

void CanvasWidget::paintEvent(QPaintEvent* /*event*/) {
    QElapsedTimer timer;
    timer.start();

    QPainter painter{this};
    painter.fillRect(rect(), currentTheme().background);
    painter.setRenderHint(QPainter::Antialiasing, true);

    drawSheet(painter);
    drawGrid(painter);
    drawScene(painter);
    // Under the selection: an object that is both hovered and selected should
    // read as selected.
    drawHoverHighlight(painter);
    drawActiveViewportFrame(painter);
    drawSelectionHighlight(painter);
    // The alignment ray a polar constraint is holding the point on.
    //
    // Without it polar is invisible: the point stops following the pointer and
    // nothing says why. other CAD software draws the same line for the same reason, and a
    // drafter reads it as "you are on this angle" before reading any number.
    if (polarEnabled() && hasCursor_) {
        if (const std::optional<Vec3> origin = constraintOrigin()) {
            const Vec3 held = constrainedWorldAt(cursorPosition_);
            const render::ScreenPoint from = toScreen(*origin);
            const render::ScreenPoint to = toScreen(held);
            // Carried past the point rather than stopping at it, which is what
            // makes it read as a direction rather than as a drawn segment.
            const double dx = to.x - from.x;
            const double dy = to.y - from.y;
            const double length = std::hypot(dx, dy);
            if (length > 1.0) {
                const double extend = 4000.0 / length;
                QPen pen{currentTheme().accent};
                pen.setStyle(Qt::DotLine);
                pen.setWidthF(1.0);
                painter.setPen(pen);
                painter.drawLine(QPointF{from.x - dx * extend, from.y - dy * extend},
                                 QPointF{from.x + dx * extend, from.y + dy * extend});
            }
        }
    }

    // The alignment path an acquired point is holding the cursor on, and a
    // marker on the point it came from. Both are needed: the path says which
    // way, and the marker says which corner it was measured from -- with three
    // points acquired, a path alone leaves that ambiguous.
    //
    // Drawn after the polar ray so the two do not fight over the same pixels
    // when a tracking path happens to lie along a polar angle.
    if (trackingHit_ && hasCursor_) {
        const render::ScreenPoint to = toScreen(trackingHit_->point);
        QPen pen{currentTheme().highlight};
        pen.setStyle(Qt::DashLine);
        pen.setWidthF(1.0);
        painter.setPen(pen);

        for (const Vec3& origin : trackingHit_->origins) {
            const render::ScreenPoint from = toScreen(origin);
            const double dx = to.x - from.x;
            const double dy = to.y - from.y;
            const double length = std::hypot(dx, dy);
            if (length <= 1.0) {
                continue;
            }
            // Carried past the point, so it reads as a direction being held
            // rather than as a segment being drawn.
            const double extend = 4000.0 / length;
            painter.drawLine(QPointF{from.x, from.y},
                             QPointF{from.x + dx * extend, from.y + dy * extend});

            // A small cross on the acquired point, which is how other CAD software marks
            // one and the only thing that separates it from bare geometry.
            constexpr double kArm = 5.0;
            painter.drawLine(QPointF{from.x - kArm, from.y - kArm},
                             QPointF{from.x + kArm, from.y + kArm});
            painter.drawLine(QPointF{from.x - kArm, from.y + kArm},
                             QPointF{from.x + kArm, from.y - kArm});
        }
    }

    drawGripPreview(painter);
    // What a grip edit currently measures, beside the cursor.
    //
    // Painted rather than put in a widget. The dynamic input is a widget and
    // has to be sized before it can be positioned, which Qt will not do until
    // the next event loop turn -- so it was placed using the width it had for
    // the *previous* number and hung off the edge of the window. Here the text
    // is measured and drawn in the same breath, and the box can be kept inside
    // the canvas because both numbers are known at once.
    // The dimensional input boxes: what a point would measure, and what can be
    // typed instead. Two of them side by side, the active one outlined, so it
    // is obvious which box a keystroke lands in -- with one box the drafter has
    // to remember, and with neither outlined they have to guess.
    if (isEnteringDimension() && hasCursor_) {
        const std::array<double, 2> live = liveDimensions();
        const bool polar = constraintOrigin().has_value();
        const bool lockedIsFirst = dimensionField_ == DimensionField::Second;

        struct Box {
            QString label;
            QString value;
            bool active = false;
            bool locked = false;
        };
        std::array<Box, 2> boxes;
        boxes[0].label = polar ? tr("Length") : tr("X");
        boxes[1].label = polar ? tr("Angle") : tr("Y");
        for (int i = 0; i < 2; ++i) {
            Box& box = boxes[static_cast<std::size_t>(i)];
            box.locked = lockedDimension_.has_value() && (i == 0) == lockedIsFirst;
            box.active = !box.locked &&
                         i == (dimensionField_ == DimensionField::First ? 0 : 1);
            if (box.locked) {
                box.value = QString::number(*lockedDimension_, 'f', 2);
            } else if (box.active && !dimensionTyped_.isEmpty()) {
                box.value = dimensionTyped_;
            } else {
                box.value = QString::number(live[static_cast<std::size_t>(i)], 'f', 2);
            }
            if (i == 1 && polar) {
                box.value += QStringLiteral("°");
            }
        }

        QFont font = painter.font();
        font.setPixelSize(12);
        painter.setFont(font);
        const QFontMetricsF metrics{font};
        constexpr double kPad = 7.0;
        constexpr double kGap = 6.0;

        std::array<double, 2> widths{};
        double total = 0.0;
        for (int i = 0; i < 2; ++i) {
            const Box& box = boxes[static_cast<std::size_t>(i)];
            const QString text = box.label + QStringLiteral("  ") + box.value +
                                 (box.active ? QStringLiteral("_") : QString{});
            widths[static_cast<std::size_t>(i)] = metrics.horizontalAdvance(text) + kPad * 2.0;
            total += widths[static_cast<std::size_t>(i)];
        }
        total += kGap;
        const double boxHeight = metrics.height() + 8.0;

        // Beside the crosshair, flipped rather than clamped when it would run
        // off: clamped, the boxes sit on top of the point being placed.
        double x = cursorPosition_.x() + 18.0;
        if (x + total > width()) {
            x = cursorPosition_.x() - 18.0 - total;
        }
        double y = cursorPosition_.y() + 16.0;
        if (y + boxHeight > height()) {
            y = cursorPosition_.y() - 16.0 - boxHeight;
        }
        x = std::clamp(x, 0.0, std::max(0.0, width() - total));
        y = std::clamp(y, 0.0, std::max(0.0, height() - boxHeight));

        // The prompt above them rather than beside: it is the one thing here
        // that is prose, and putting it in the row would make the drafter read
        // past a sentence to reach the number they are typing.
        //
        // While a grip is held that line carries the mode and what the object
        // is becoming -- "STRETCH   length 402.856 (+2.856)" -- which is not
        // the same information as the boxes. They say what to type to place the
        // grip; this says what the object will be once it is placed.
        QString prompt;
        if (gripDrag_) {
            prompt = gripModeName();
            if (const QString readout = gripReadout(); !readout.isEmpty()) {
                prompt += QStringLiteral("   ") + readout;
            }
        } else if (const InputRequest* request = document_->executor().currentRequest();
                   request != nullptr) {
            prompt = QString::fromStdString(request->prompt);
        }
        if (!prompt.isEmpty()) {
            const double promptWidth = metrics.horizontalAdvance(prompt) + kPad * 2.0;
            const QRectF promptRect{x, y - boxHeight - 4.0, promptWidth, boxHeight};
            if (promptRect.top() >= 0.0) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor{0, 0, 0, 205});
                painter.drawRoundedRect(promptRect, 4.0, 4.0);
                painter.setBrush(Qt::NoBrush);
                painter.setPen(currentTheme().textMuted);
                painter.drawText(promptRect, Qt::AlignCenter, prompt);
            }
        }

        for (int i = 0; i < 2; ++i) {
            const Box& box = boxes[static_cast<std::size_t>(i)];
            const QRectF rect{x, y, widths[static_cast<std::size_t>(i)], boxHeight};
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor{0, 0, 0, 205});
            painter.drawRoundedRect(rect, 4.0, 4.0);
            painter.setBrush(Qt::NoBrush);
            // A locked field is drawn in the highlight colour and the active
            // one in the accent, so "this one is settled" and "this one is
            // listening" do not look the same.
            painter.setPen(QPen{box.locked      ? currentTheme().highlight
                                : box.active    ? currentTheme().accent
                                                : currentTheme().textMuted,
                                box.active ? 1.6 : 1.0});
            painter.drawRoundedRect(rect, 4.0, 4.0);

            painter.setPen(currentTheme().textMuted);
            const QRectF inner = rect.adjusted(kPad, 0.0, -kPad, 0.0);
            painter.drawText(inner, Qt::AlignVCenter | Qt::AlignLeft, box.label);
            painter.setPen(box.locked ? currentTheme().highlight : currentTheme().foreground);
            painter.drawText(inner, Qt::AlignVCenter | Qt::AlignRight,
                             box.value + (box.active ? QStringLiteral("_") : QString{}));
            x += widths[static_cast<std::size_t>(i)] + kGap;
        }
    }

    // Only when the dimension boxes are not up. They carry the same readout on
    // their prompt line, and both anchor to the crosshair -- drawn together,
    // this one landed on top of the numbers being typed into.
    if (gripDrag_ && hasCursor_ && !isEnteringDimension()) {
        QString readout = gripReadout();
        // The angle the constraint is holding, in other CAD software's own `< 135°` form.
        // Polar without an angle tells a drafter the point is being held but
        // not what it is being held at, which is the one thing they need.
        if (!readout.isEmpty() && polarEnabled()) {
            if (const std::optional<Vec3> origin = constraintOrigin()) {
                const Vec3 held = constrainedWorldAt(cursorPosition_);
                const double degrees =
                    radToDeg(std::atan2(held.y - origin->y, held.x - origin->x));
                readout += tr("   < %1°").arg(degrees < 0 ? degrees + 360.0 : degrees, 0, 'f', 0);
            }
        }
        if (!readout.isEmpty()) {
            QFont font = painter.font();
            font.setPixelSize(12);
            const QFontMetricsF metrics{font};
            const double boxWidth = metrics.horizontalAdvance(readout) + 16.0;
            const double boxHeight = metrics.height() + 8.0;
            // Beside the crosshair, and flipped to its other side rather than
            // clamped when that would run off: a readout clamped to the edge
            // sits on top of the very point being placed.
            double x = cursorPosition_.x() + 18.0;
            if (x + boxWidth > width()) {
                x = cursorPosition_.x() - 18.0 - boxWidth;
            }
            double y = cursorPosition_.y() + 16.0;
            if (y + boxHeight > height()) {
                y = cursorPosition_.y() - 16.0 - boxHeight;
            }
            const QRectF box{std::clamp(x, 0.0, std::max(0.0, width() - boxWidth)),
                             std::clamp(y, 0.0, std::max(0.0, height() - boxHeight)), boxWidth,
                             boxHeight};

            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor{0, 0, 0, 205});
            painter.drawRoundedRect(box, 4.0, 4.0);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen{currentTheme().accent, 1.0});
            painter.drawRoundedRect(box, 4.0, 4.0);
            painter.setFont(font);
            painter.setPen(currentTheme().foreground);
            painter.drawText(box, Qt::AlignCenter, readout);
        }
    }

    drawPreview(painter);
    drawSelectionRectangle(painter);
    drawSnapMarker(painter);
    drawCrosshair(painter);

    lastPaintMs_ = static_cast<double>(timer.nsecsElapsed()) / 1e6;
}

void CanvasWidget::drawGrid(QPainter& painter) {
    const Theme& theme = currentTheme();
    const Box3 visible = viewport_.visibleBounds();

    // Spacing adapts to zoom: a fixed world spacing becomes an unreadable smear
    // when zoomed out and invisible when zoomed in.
    const double step = niceStep(viewport_.pixelsToWorld(70.0));
    if (!(step > 0.0)) {
        return;
    }
    if (visible.width() / step > 400.0 || visible.height() / step > 400.0) {
        return;
    }

    const double startX = std::floor(visible.min.x / step) * step;
    const double startY = std::floor(visible.min.y / step) * step;

    for (int pass = 0; pass < 2; ++pass) {
        const bool wantMajor = pass == 1;
        painter.setPen(QPen{wantMajor ? theme.gridMajor : theme.grid, 1});

        for (double x = startX; x <= visible.max.x; x += step) {
            // Every tenth line is major, which gives the grid a readable rhythm.
            if ((std::llround(x / step) % 10 == 0) != wantMajor) {
                continue;
            }
            const double sx = snapHalf(viewport_.worldToScreen(Vec3{x, visible.min.y, 0.0}).x);
            painter.drawLine(QPointF{sx, 0.0}, QPointF{sx, static_cast<double>(height())});
        }
        for (double y = startY; y <= visible.max.y; y += step) {
            if ((std::llround(y / step) % 10 == 0) != wantMajor) {
                continue;
            }
            const double sy = snapHalf(viewport_.worldToScreen(Vec3{visible.min.x, y, 0.0}).y);
            painter.drawLine(QPointF{0.0, sy}, QPointF{static_cast<double>(width()), sy});
        }
    }
}

void CanvasWidget::drawScene(QPainter& painter) {
    if (sceneBuilder_ == nullptr || document_ == nullptr) {
        lastVisible_ = 0;
        lastBuildMs_ = 0.0;
        return;
    }

    // A sheet and the model are built by different paths: the model through
    // the spatial index, a sheet directly with its viewports querying that
    // index for the region each one shows.
    const render::Scene scene =
        document_->inPaperSpace()
            ? sceneBuilder_->buildLayout(document_->activeLayout(), viewport_, sceneOptions())
            : sceneBuilder_->build(document_->activeSpace(), viewport_, sceneOptions());
    lastVisible_ = scene.items.size();
    lastBuildMs_ = scene.buildMilliseconds;

    // The same painting the plotter uses. A printed drawing that went through
    // different code would differ from the screen in ways nobody discovers
    // until the paper is in their hand.
    ScenePaintOptions options;
    options.minimumTextPixels = textGreekingPixels_;
    options.images = &imageCache_;
    paintScene(painter, scene, viewport_, currentTheme(), options);
}

void CanvasWidget::drawSheet(QPainter& painter) const {
    if (document_ == nullptr || !document_->inPaperSpace()) {
        return;
    }
    // Showing the paper is most of what makes paper space legible. Without it
    // a sheet is geometry floating in the same void as the model, and the one
    // question the user has — does this fit on the page — is unanswerable.
    const Layout& layout = document_->activeLayout();
    if (layout.plot.paperWidth <= 0.0 || layout.plot.paperHeight <= 0.0) {
        return;
    }

    const auto toRect = [this](const Box3& box) {
        // Paper coordinates, whatever viewport is active: the sheet is drawn
        // where it is, and the model moves behind its windows.
        const render::ScreenPoint low = viewport_.worldToScreen(box.min);
        const render::ScreenPoint high = viewport_.worldToScreen(box.max);
        return QRectF{QPointF{std::min(low.x, high.x), std::min(low.y, high.y)},
                      QPointF{std::max(low.x, high.x), std::max(low.y, high.y)}};
    };

    // The layout's own rectangle, not the plot settings' paper size: the stored
    // size is millimetres at 1:1, and where the page lands in the coordinates
    // the title block is drawn in depends on the plot scale and origin too.
    const Box3 sheet = layout.sheetRect();
    const QRectF sheetRect = toRect(sheet);

    const Theme& theme = currentTheme();
    // A soft drop shadow, then the page. The shadow is what makes the sheet
    // read as sitting on the background rather than being a rectangle drawn on
    // it.
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor{0, 0, 0, 60});
    painter.drawRect(sheetRect.translated(3.0, 3.0));
    painter.setBrush(theme.sheet);
    painter.drawRect(sheetRect);
    painter.setBrush(Qt::NoBrush);

    // The printable area, dashed, because geometry outside it will not plot
    // and that is worth knowing before the plot rather than after.
    const Box3 printable = layout.printableRect();
    if (!printable.isEmpty() && printable.width() < sheet.width()) {
        QPen pen{theme.textMuted};
        pen.setStyle(Qt::DashLine);
        pen.setCosmetic(true);
        painter.setPen(pen);
        painter.drawRect(toRect(printable));
    }
}

bool CanvasWidget::updateHoveredEntity(const QPointF& position) {
    std::vector<Handle> candidates;

    // Not while dragging a grip or rubber-banding a selection: the answer to
    // "what would a click get" is already settled during those.
    if (document_ != nullptr && !gripDrag_ && !selecting_ && !panning_) {
        const Vec3 world = toWorld({position.x(), position.y()});
        // pickAll rather than pickEntity, so cycling has the whole stack to
        // walk. Its first entry is what pickEntity would have returned, so the
        // outline still matches an unmodified click.
        for (const render::PickResult& hit :
             render::pickAll(document_->db(), document_->index(), world, apertureWorld())) {
            candidates.push_back(hit.handle);
        }
    }

    if (candidates == hoverCandidates_) {
        return false;
    }
    // A different stack means a different question, so cycling starts over.
    // Keeping the index would offer the third object under a place the user has
    // only just moved to.
    hoverCandidates_ = std::move(candidates);
    hoverIndex_ = 0;
    hoveredEntity_ = hoverCandidates_.empty() ? std::optional<Handle>{}
                                              : std::optional<Handle>{hoverCandidates_.front()};
    if (hoveredEntity_.has_value()) {
        acquireForSnapping(*hoveredEntity_);
    }
    return true;
}

void CanvasWidget::cycleHovered() {
    if (hoverCandidates_.size() < 2) {
        return;
    }
    hoverIndex_ = (hoverIndex_ + 1) % hoverCandidates_.size();
    hoveredEntity_ = hoverCandidates_[hoverIndex_];
    acquireForSnapping(*hoveredEntity_);
    Q_EMIT hoverCycled(static_cast<int>(hoverIndex_) + 1,
                       static_cast<int>(hoverCandidates_.size()));
    update();
}

QString CanvasWidget::hoveredDescription() const {
    if (document_ == nullptr || !hoveredEntity_.has_value()) {
        return {};
    }
    const Entity* entity = document_->db().getEntity(*hoveredEntity_);
    if (entity == nullptr) {
        return {};
    }
    QString description = QString::fromStdString(entity->objectType());
    if (const auto* layer = document_->db().get<Layer>(entity->layerHandle)) {
        description += tr(" on %1").arg(QString::fromStdString(layer->name));
    }
    return description;
}

void CanvasWidget::acquireForSnapping(Handle handle) {
    // Bounded and most-recent-first. More than a few would make extension lines
    // appear from objects the user has long since moved away from, which reads
    // as the cursor sticking to nothing.
    constexpr std::size_t kMaxAcquired = 3;

    const auto existing = std::find(acquiredEntities_.begin(), acquiredEntities_.end(), handle);
    if (existing != acquiredEntities_.end()) {
        acquiredEntities_.erase(existing);
    }
    acquiredEntities_.push_front(handle);
    while (acquiredEntities_.size() > kMaxAcquired) {
        acquiredEntities_.pop_back();
    }
}

void CanvasWidget::drawHoverHighlight(QPainter& painter) {
    if (document_ == nullptr || !rolloverHighlight_ || !hoveredEntity_.has_value()) {
        return;
    }
    // Already outlined more strongly as part of the selection; drawing both
    // would just thicken it and say nothing.
    if (document_->isSelected(*hoveredEntity_)) {
        return;
    }
    const Entity* entity = document_->db().getEntity(*hoveredEntity_);
    if (entity == nullptr || entity->erased) {
        return;
    }

    // Thicker than the geometry but softer than a selection: it is telling the
    // user what a click would get, not what they already have.
    QColor colour = currentTheme().highlight;
    colour.setAlpha(170);
    QPen pen{colour};
    pen.setWidthF(4.0);
    pen.setCosmetic(true);
    pen.setJoinStyle(Qt::RoundJoin);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);

    outlineEntity(painter, *entity);

    // A badge when several objects are stacked here. Cycling is worthless if
    // nobody knows there is anything to cycle *to* — this is the only signal
    // that the thing under the cursor is not the only thing under the cursor.
    if (hoverCandidates_.size() < 2 || !hasCursor_) {
        return;
    }
    const QString badge = tr("%1/%2  ⇧Space")
                              .arg(hoverIndex_ + 1)
                              .arg(hoverCandidates_.size());

    QFont font = painter.font();
    font.setPixelSize(11);
    const QFontMetricsF metrics{font};
    const QRectF box{cursorPosition_.x() + 16.0, cursorPosition_.y() - 26.0,
                     metrics.horizontalAdvance(badge) + 10.0, metrics.height() + 4.0};

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor{0, 0, 0, 180});
    painter.drawRoundedRect(box, 3.0, 3.0);
    painter.setBrush(Qt::NoBrush);
    painter.setFont(font);
    painter.setPen(currentTheme().highlight);
    painter.drawText(box, Qt::AlignCenter, badge);
}

void CanvasWidget::outlineEntity(QPainter& painter, const Entity& entity) const {
    TessellationOptions tessellation;
    tessellation.chordTolerance = render::chordToleranceFor(viewport_);

    std::vector<CurvePtr> curves = entity.toCurves();

    // A block reference draws the lines of its definition, so that is what a
    // selected one has to light up. Outlining its extents instead drew a box
    // around nothing -- a rectangle in mid-air with no edge of it on any real
    // geometry -- and left the user guessing what they had picked.
    if (document_ != nullptr) {
        if (const auto* insert = dynamic_cast<const InsertEntity*>(&entity)) {
            curves = blockReferenceCurves(document_->db(), *insert);
        }
    }

    if (curves.empty()) {
        // What is left has no curve geometry at all -- text, or a reference
        // whose definition is missing -- and is outlined by its extents, which
        // is also how it is picked, so the outline shows the region a click
        // actually tests.
        const Box3 bounds = entity.bounds();
        if (bounds.isEmpty()) {
            return;
        }
        painter.drawPolygon(toPolygon(viewport_, {bounds.corners2()[0], bounds.corners2()[1],
                                                  bounds.corners2()[2], bounds.corners2()[3]}));
        return;
    }

    for (const CurvePtr& curve : curves) {
        const QPolygonF polygon = toPolygon(viewport_, curve->tessellate(tessellation));
        if (curve->isClosed()) {
            painter.drawPolygon(polygon);
        } else {
            painter.drawPolyline(polygon);
        }
    }
}

void CanvasWidget::drawSelectionHighlight(QPainter& painter) {
    if (document_ == nullptr || document_->selection().empty()) {
        return;
    }

    // Selection is drawn as a second pass so it is never hidden beneath
    // unselected geometry drawn later.
    QPen pen{currentTheme().selection};
    pen.setWidthF(2.5);
    pen.setCosmetic(true);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);

    TessellationOptions tessellation;
    tessellation.chordTolerance = render::chordToleranceFor(viewport_);

    for (const Handle handle : document_->selection()) {
        const Entity* entity = document_->db().getEntity(handle);
        if (entity == nullptr || entity->erased) {
            continue;
        }
        // Not drawn, not highlighted: a selected object whose layer has just
        // gone off or frozen must not linger as an outline over nothing.
        if (const auto* layer = document_->db().get<Layer>(entity->layerHandle);
            layer != nullptr && (layer->isOff || layer->isFrozen)) {
            continue;
        }

        outlineEntity(painter, *entity);

        // Grips: the small squares that make direct editing discoverable.
        for (const GripPoint& grip : entity->gripPoints()) {
            const render::ScreenPoint p = toScreen(grip.position);
            const bool hot = hotGrip_ && hotGrip_->first == handle &&
                             hotGrip_->second.index == grip.index;
            const bool dragging = gripDrag_ && gripDrag_->entity == handle &&
                                  gripDrag_->grip.index == grip.index;

            // The one under the cursor is drawn larger and in the highlight
            // colour, so it is obvious which grip a press would grab — a row
            // of identical squares gives the user nothing to aim at.
            // Six pixels across is a small thing to aim at, and a grip is
            // meant to be clicked. Sized to what a pointer can hit without
            // care, and grown further when the cursor is on it so the target
            // confirms itself before the click rather than after.
            const double half = hot || dragging ? 5.5 : 4.0;
            painter.setBrush(hot || dragging ? currentTheme().highlight
                                             : currentTheme().selection);
            painter.drawRect(QRectF{p.x - half, p.y - half, half * 2.0, half * 2.0});

            // A grip marked to come along gets a ring, so a drafter can see
            // what the next drag will take with it. Marking several and then
            // being unable to tell which were marked would make the feature
            // worse than not having it.
            if (isHotGrip(handle, grip)) {
                painter.setBrush(Qt::NoBrush);
                QPen ring{currentTheme().highlight};
                ring.setWidthF(1.5);
                painter.setPen(ring);
                painter.drawRect(
                    QRectF{p.x - half - 3.0, p.y - half - 3.0, (half + 3.0) * 2.0,
                           (half + 3.0) * 2.0});
                // Put back the pen the pass runs on. Leaving the ring's pen
                // here -- or clearing it -- outlines whatever is drawn next in
                // the wrong colour or in none at all, and the next thing is
                // another selected object.
                painter.setPen(pen);
            }
        }
        painter.setBrush(Qt::NoBrush);
    }
}

void CanvasWidget::drawPreview(QPainter& painter) {
    if (document_ == nullptr) {
        return;
    }
    const auto& preview = document_->executor().preview();
    if (preview.empty()) {
        return;
    }

    QPen pen{currentTheme().highlight};
    pen.setWidthF(1.0);
    pen.setCosmetic(true);
    pen.setStyle(Qt::DashLine);
    painter.setPen(pen);

    TessellationOptions tessellation;
    tessellation.chordTolerance = render::chordToleranceFor(viewport_);

    for (const std::unique_ptr<Entity>& entity : preview) {
        for (const CurvePtr& curve : entity->toCurves()) {
            const QPolygonF polygon = toPolygon(viewport_, curve->tessellate(tessellation));
            if (curve->isClosed()) {
                painter.drawPolygon(polygon);
            } else {
                painter.drawPolyline(polygon);
            }
        }

        // A leader's arrowhead, which its curves do not include: it is a filled
        // triangle rather than a stroked path, and a preview of a leader
        // without it is a dashed line whose pointed end is anybody's guess --
        // which is the one thing the first two picks decide.
        if (const auto* leader = dynamic_cast<const LeaderEntity*>(entity.get())) {
            const LeaderGeometry shape =
                leader->geometry(document_->db().dimStyleByHandle(leader->styleHandle));
            for (const std::array<Vec3, 3>& triangle : shape.arrowheads) {
                QPolygonF head;
                for (const Vec3& corner : triangle) {
                    const render::ScreenPoint at = toScreen(corner);
                    head << QPointF{at.x, at.y};
                }
                painter.setBrush(currentTheme().highlight);
                painter.drawPolygon(head);
                painter.setBrush(Qt::NoBrush);
            }
        }
    }
}

void CanvasWidget::drawSelectionRectangle(QPainter& painter) const {
    if (!selecting_ || !hasCursor_) {
        return;
    }

    const QRectF rectangle = QRectF{selectionOrigin_, cursorPosition_}.normalized();
    const bool crossing = cursorPosition_.x() < selectionOrigin_.x();
    const Theme& theme = currentTheme();

    // Crossing and window are visually distinct so the mode is never in doubt:
    // dashed for crossing, solid for window, matching other CAD software's convention.
    QPen pen{crossing ? theme.snapMarker : theme.selection};
    pen.setWidthF(1.0);
    pen.setStyle(crossing ? Qt::DashLine : Qt::SolidLine);
    painter.setPen(pen);

    QColor fill = pen.color();
    fill.setAlpha(28);
    painter.setBrush(fill);
    painter.drawRect(rectangle);
    painter.setBrush(Qt::NoBrush);
}

void CanvasWidget::drawSnapMarker(QPainter& painter) const {
    if (!activeSnap_.has_value()) {
        return;
    }

    const render::ScreenPoint p = toScreen(activeSnap_->position);
    const Theme& theme = currentTheme();
    QPen pen{theme.snapMarker};
    pen.setWidthF(1.8);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    // Each snap gets a distinct glyph, so the user can tell *what* is being
    // snapped to without reading a tooltip — the convention every CAD user
    // already knows.
    const double s = kSnapMarkerPixels;
    switch (activeSnap_->mode) {
        case SnapMode::Endpoint:
            painter.drawRect(QRectF{p.x - s, p.y - s, s * 2, s * 2});
            break;
        case SnapMode::Midpoint: {
            const QPolygonF triangle{
                {QPointF{p.x, p.y - s}, QPointF{p.x + s, p.y + s}, QPointF{p.x - s, p.y + s}}};
            painter.drawPolygon(triangle);
            break;
        }
        case SnapMode::Center:
            painter.drawEllipse(QPointF{p.x, p.y}, s, s);
            break;
        case SnapMode::Intersection:
        case SnapMode::ApparentIntersection:
            painter.drawLine(QPointF{p.x - s, p.y - s}, QPointF{p.x + s, p.y + s});
            painter.drawLine(QPointF{p.x + s, p.y - s}, QPointF{p.x - s, p.y + s});
            break;
        case SnapMode::Node:
            painter.drawEllipse(QPointF{p.x, p.y}, s, s);
            painter.drawLine(QPointF{p.x - s, p.y}, QPointF{p.x + s, p.y});
            painter.drawLine(QPointF{p.x, p.y - s}, QPointF{p.x, p.y + s});
            break;
        case SnapMode::Perpendicular:
            painter.drawLine(QPointF{p.x - s, p.y - s}, QPointF{p.x - s, p.y + s});
            painter.drawLine(QPointF{p.x - s, p.y + s}, QPointF{p.x + s, p.y + s});
            painter.drawLine(QPointF{p.x, p.y + s}, QPointF{p.x, p.y});
            painter.drawLine(QPointF{p.x, p.y}, QPointF{p.x - s, p.y});
            break;
        case SnapMode::Tangent:
            painter.drawEllipse(QPointF{p.x, p.y}, s, s);
            painter.drawLine(QPointF{p.x - s, p.y - s}, QPointF{p.x + s, p.y - s});
            break;

        case SnapMode::Quadrant: {
            // A diamond, as other CAD software draws it.
            const QPolygonF diamond{{QPointF{p.x, p.y - s}, QPointF{p.x + s, p.y},
                                     QPointF{p.x, p.y + s}, QPointF{p.x - s, p.y}}};
            painter.drawPolygon(diamond);
            break;
        }

        case SnapMode::Nearest: {
            // An hourglass: two triangles meeting at the point, which reads as
            // "somewhere along here" rather than a specific feature.
            const QPolygonF hourglass{{QPointF{p.x - s, p.y - s}, QPointF{p.x + s, p.y - s},
                                       QPointF{p.x - s, p.y + s}, QPointF{p.x + s, p.y + s}}};
            painter.drawPolygon(hourglass);
            break;
        }

        case SnapMode::Extension: {
            // Three dots along the direction of travel — other CAD software's ellipsis,
            // meaning "carrying on past the end of the object".
            painter.setBrush(theme.snapMarker);
            for (const double offset : {-s, 0.0, s}) {
                painter.drawEllipse(QPointF{p.x + offset, p.y}, 1.2, 1.2);
            }
            painter.setBrush(Qt::NoBrush);
            break;
        }

        case SnapMode::Insertion: {
            // Two offset boxes, the conventional glyph for a placement point.
            painter.drawRect(QRectF{p.x - s, p.y - s, s * 1.4, s * 1.4});
            painter.drawRect(QRectF{p.x - s * 0.4, p.y - s * 0.4, s * 1.4, s * 1.4});
            break;
        }

        case SnapMode::Parallel: {
            painter.drawLine(QPointF{p.x - s, p.y - s * 0.4}, QPointF{p.x + s, p.y - s * 0.4});
            painter.drawLine(QPointF{p.x - s, p.y + s * 0.4}, QPointF{p.x + s, p.y + s * 0.4});
            break;
        }

        case SnapMode::None:
        case SnapMode::Clear:
            break;
    }
}

void CanvasWidget::drawCrosshair(QPainter& painter) const {
    // Drawn whenever the pointer is over the drawing, not only mid-command.
    //
    // It used to appear only while a command asked for a point. That hid the
    // aperture box exactly when a user is deciding what to click, which is when
    // knowing how close they have to be matters most — and it made the cursor
    // settings impossible to judge, since you could not see what you were
    // adjusting.
    if (!hasCursor_ || document_ == nullptr) {
        return;
    }

    QPen pen{currentTheme().crosshair};
    pen.setWidthF(1.0);
    pen.setCosmetic(true);
    painter.setPen(pen);

    const double x = snapHalf(cursorPosition_.x());
    const double y = snapHalf(cursorPosition_.y());

    // other CAD software's CURSORSIZE: arm length as a percentage of the screen, where 100
    // means full width. Measured against the smaller dimension so the crosshair
    // is square rather than stretched on a wide window.
    const double shorter = std::min(width(), height());
    const double arm = crosshairPercent_ >= 100.0
                           ? std::max(width(), height())
                           : shorter * crosshairPercent_ / 100.0;

    // The box is the *target*: how close a click has to be to catch something,
    // and how close the cursor has to be for a snap. The arms stop at its edge
    // rather than crossing it, so the box reads as an aperture rather than as
    // decoration sitting on a line.
    const double half = aperturePixels_;

    painter.drawLine(QPointF{x - arm, y}, QPointF{x - half, y});
    painter.drawLine(QPointF{x + half, y}, QPointF{x + arm, y});
    painter.drawLine(QPointF{x, y - arm}, QPointF{x, y - half});
    painter.drawLine(QPointF{x, y + half}, QPointF{x, y + arm});

    painter.drawRect(QRectF{x - half, y - half, half * 2.0, half * 2.0});
}

}  // namespace cad::app
