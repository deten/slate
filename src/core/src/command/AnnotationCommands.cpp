/// The annotation commands: the dimension family and HATCH.
///
/// These differ from the drawing commands in that what they create is
/// *derived*. A dimension stores a few points and reads its own measurement; a
/// hatch stores a boundary and generates its own fill. So these commands are
/// mostly about capturing the right points and the right boundary — the
/// appearance is the entity's business, and the style's.

#include <algorithm>
#include <cctype>

#include "cad/command/StandardCommands.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/DimensionEntity.hpp"
#include "cad/db/HatchEntity.hpp"
#include "cad/db/LeaderEntity.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/geom/Arc.hpp"
#include "cad/geom/EllipseArc.hpp"
#include "cad/geom/LineSegment.hpp"

namespace cad {
namespace {

const CommandKeyword kTextKeyword{"Text", "T", "Override the measurement text"};
const CommandKeyword kAngleKeyword{"Angle", "A", "Rotate the dimension text"};
const CommandKeyword kRotatedKeyword{"Rotated", "R", "Measure along a given angle"};
const CommandKeyword kSolidKeyword{"Solid", "S", "Fill solid instead of hatching"};

[[nodiscard]] bool matchesKeyword(const std::string& typed, char initial) {
    return !typed.empty() &&
           std::toupper(static_cast<unsigned char>(typed.front())) == initial;
}

/// The style a new dimension should take.
///
/// The drawing's current style, falling back to Standard. Resolved when the
/// dimension is created rather than stored per command, so changing the current
/// style takes effect immediately.
[[nodiscard]] Handle currentDimStyle(CommandContext& context) {
    Database& db = context.db();
    if (const DimStyle* current = db.findDimStyle(db.settings.currentDimStyleName)) {
        return current->handle;
    }
    return db.ensureDimStyle(kDimStyleStandard).handle;
}

/// Shared behaviour for the two-point dimension commands.
///
/// DIMLINEAR and DIMALIGNED differ only in how they choose their measurement
/// axis, so they are one class with a flag rather than two nearly identical
/// state machines.
class LinearDimensionCommand final : public CommandInstance {
public:
    explicit LinearDimensionCommand(bool aligned) : aligned_(aligned) {}

    StepResult start(CommandContext& /*context*/) override {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = "Specify first extension line origin";
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }

        if (input.kind == InputValue::Kind::Keyword) {
            if (matchesKeyword(input.text, 'T')) {
                InputRequest request;
                request.kind = InputRequest::Kind::Text;
                request.prompt = "Enter dimension text (<> is the measurement)";
                request.allowEmpty = true;
                awaitingText_ = true;
                return StepResult::ask(request);
            }
            if (matchesKeyword(input.text, 'R') && !aligned_) {
                InputRequest request;
                request.kind = InputRequest::Kind::Angle;
                request.prompt = "Specify angle of dimension line";
                awaitingAngle_ = true;
                return StepResult::ask(request);
            }
            return StepResult::ask(locationRequest());
        }

        if (awaitingText_) {
            awaitingText_ = false;
            textOverride_ = input.kind == InputValue::Kind::Text ? input.text : std::string{};
            return StepResult::ask(locationRequest());
        }
        if (awaitingAngle_) {
            awaitingAngle_ = false;
            if (input.kind == InputValue::Kind::Number) {
                rotation_ = input.number;
                hasRotation_ = true;
            }
            return StepResult::ask(locationRequest());
        }

        if (input.kind != InputValue::Kind::Point) {
            return StepResult::ask(currentRequest());
        }

        switch (picked_) {
            case 0: {
                first_ = input.point;
                ++picked_;
                InputRequest request;
                request.kind = InputRequest::Kind::Point;
                request.prompt = "Specify second extension line origin";
                request.basePoint = first_;
                return StepResult::ask(request);
            }
            case 1: {
                second_ = input.point;
                if (first_.equals(second_)) {
                    return StepResult::failed("The two extension line origins are the same.");
                }
                ++picked_;
                return StepResult::ask(locationRequest());
            }
            default: {
                auto dimension = build(input.point, context);
                context.transaction().addToCurrentSpace(std::move(dimension));
                return StepResult::done();
            }
        }
    }

    [[nodiscard]] std::vector<std::unique_ptr<Entity>> preview(
        const Vec3& cursor, CommandContext& context) const override {
        if (picked_ < 2) {
            return {};
        }
        std::vector<std::unique_ptr<Entity>> items;
        items.push_back(build(cursor, context));
        return items;
    }

private:
    [[nodiscard]] InputRequest currentRequest() const {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = picked_ == 0 ? "Specify first extension line origin"
                                      : "Specify second extension line origin";
        if (picked_ == 1) {
            request.basePoint = first_;
        }
        return request;
    }

    [[nodiscard]] InputRequest locationRequest() const {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = "Specify dimension line location";
        request.basePoint = first_.midpoint(second_);
        request.keywords.push_back(kTextKeyword);
        if (!aligned_) {
            request.keywords.push_back(kRotatedKeyword);
        }
        return request;
    }

    [[nodiscard]] std::unique_ptr<DimensionEntity> build(const Vec3& location,
                                                         CommandContext& context) const {
        auto dimension = std::make_unique<DimensionEntity>();
        dimension->point13 = first_;
        dimension->point14 = second_;
        dimension->textOverride = textOverride_;
        dimension->styleHandle = currentDimStyle(context);

        // Group 10 is where the dimension line meets the second extension
        // line: that is how other CAD software stores it, and how it reads it back. The
        // picked point only says which line the dimension sits on, so it is
        // projected there rather than stored as picked.
        const auto settle = [&](double angle) {
            const Vec3 along{std::cos(angle), std::sin(angle), 0.0};
            const Vec3 across{-along.y, along.x, 0.0};
            dimension->definitionPoint = second_ + across * (location - second_).dot(across);
        };

        if (aligned_) {
            dimension->type = DimensionType::Aligned;
            settle((second_ - first_).angle2());
            return dimension;
        }

        dimension->type = DimensionType::Rotated;
        if (hasRotation_) {
            dimension->rotation = rotation_;
            settle(rotation_);
            return dimension;
        }

        // Which axis is measured follows the drag, as it does in other CAD software:
        // pulling the dimension line above or below the points measures their
        // horizontal separation, pulling it to one side measures the vertical.
        const Vec3 offset = location - first_.midpoint(second_);
        dimension->rotation = std::abs(offset.y) >= std::abs(offset.x) ? 0.0 : kPi / 2.0;
        settle(dimension->rotation);
        return dimension;
    }

    bool aligned_ = false;
    int picked_ = 0;
    Vec3 first_;
    Vec3 second_;
    std::string textOverride_;
    double rotation_ = 0.0;
    bool hasRotation_ = false;
    bool awaitingText_ = false;
    bool awaitingAngle_ = false;
};

/// DIMRADIUS and DIMDIAMETER.
class RadialDimensionCommand final : public CommandInstance {
public:
    explicit RadialDimensionCommand(bool diameter) : diameter_(diameter) {}

    StepResult start(CommandContext& /*context*/) override {
        InputRequest request;
        request.kind = InputRequest::Kind::Entity;
        request.prompt = "Select an arc or a circle";
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }

        if (!hasCircle_) {
            Handle handle = input.entity;
            if (input.kind == InputValue::Kind::Selection && !input.selection.empty()) {
                handle = input.selection.front();
            }
            const Entity* entity = context.db().getEntity(handle);
            if (entity == nullptr) {
                return StepResult::failed("Nothing was selected.");
            }
            if (const auto* circle = dynamic_cast<const CircleEntity*>(entity)) {
                center_ = circle->center;
                radius_ = circle->radius;
            } else if (const auto* arc = dynamic_cast<const ArcEntity*>(entity)) {
                center_ = arc->center;
                radius_ = arc->radius;
            } else {
                return StepResult::failed("A radial dimension needs an arc or a circle.");
            }
            hasCircle_ = true;

            InputRequest request;
            request.kind = InputRequest::Kind::Point;
            request.prompt = "Specify dimension line location";
            request.basePoint = center_;
            request.keywords.push_back(kTextKeyword);
            return StepResult::ask(request);
        }

        if (input.kind == InputValue::Kind::Keyword && matchesKeyword(input.text, 'T')) {
            InputRequest request;
            request.kind = InputRequest::Kind::Text;
            request.prompt = "Enter dimension text (<> is the measurement)";
            request.allowEmpty = true;
            awaitingText_ = true;
            return StepResult::ask(request);
        }
        if (awaitingText_) {
            awaitingText_ = false;
            textOverride_ = input.kind == InputValue::Kind::Text ? input.text : std::string{};
            InputRequest request;
            request.kind = InputRequest::Kind::Point;
            request.prompt = "Specify dimension line location";
            request.basePoint = center_;
            return StepResult::ask(request);
        }
        if (input.kind != InputValue::Kind::Point) {
            return StepResult::cancelled();
        }

        context.transaction().addToCurrentSpace(build(input.point, context));
        return StepResult::done();
    }

    [[nodiscard]] std::vector<std::unique_ptr<Entity>> preview(
        const Vec3& cursor, CommandContext& context) const override {
        if (!hasCircle_) {
            return {};
        }
        std::vector<std::unique_ptr<Entity>> items;
        items.push_back(build(cursor, context));
        return items;
    }

private:
    [[nodiscard]] std::unique_ptr<DimensionEntity> build(const Vec3& location,
                                                         CommandContext& context) const {
        // The dimension points at whichever side of the circle the user
        // dragged towards, so the leader never crosses the part.
        Vec3 direction = location - center_;
        if (direction.lengthSquared() < 1e-18) {
            direction = Vec3::unitX();
        }
        direction = direction.normalized();
        const Vec3 near = center_ + direction * radius_;

        auto dimension = std::make_unique<DimensionEntity>();
        dimension->textOverride = textOverride_;
        dimension->styleHandle = currentDimStyle(context);
        // The picked point *is* the text position for a radial dimension,
        // and the file says so: without bit 128 a reader recomputes the
        // position from the style and the pick is lost. other CAD software sets it for
        // its own radial dimensions, which is how the difference showed.
        dimension->textMidPoint = location;
        dimension->textPositionIsUserDefined = true;

        if (diameter_) {
            dimension->type = DimensionType::Diameter;
            // Both stored points are on the circumference; the centre is
            // implied by them, which is why it is not stored.
            dimension->definitionPoint = center_ - direction * radius_;
            dimension->point15 = near;
        } else {
            dimension->type = DimensionType::Radius;
            dimension->definitionPoint = center_;
            dimension->point15 = near;
            dimension->leaderLength = std::max(0.0, location.distanceTo(near));
        }
        return dimension;
    }

    bool diameter_ = false;
    bool hasCircle_ = false;
    Vec3 center_;
    double radius_ = 0.0;
    std::string textOverride_;
    bool awaitingText_ = false;
};

/// DIMANGULAR, in its three-point form.
class AngularDimensionCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& /*context*/) override {
        return StepResult::ask(promptFor(0));
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }
        if (input.kind != InputValue::Kind::Point) {
            return StepResult::ask(promptFor(picked_));
        }

        if (picked_ < 3) {
            points_[static_cast<std::size_t>(picked_)] = input.point;
            ++picked_;
            if (picked_ == 2 && points_[0].equals(points_[1])) {
                return StepResult::failed("The first arm has no direction.");
            }
            return StepResult::ask(promptFor(picked_));
        }

        context.transaction().addToCurrentSpace(build(input.point, context));
        return StepResult::done();
    }

    [[nodiscard]] std::vector<std::unique_ptr<Entity>> preview(
        const Vec3& cursor, CommandContext& context) const override {
        if (picked_ < 3) {
            return {};
        }
        std::vector<std::unique_ptr<Entity>> items;
        items.push_back(build(cursor, context));
        return items;
    }

private:
    [[nodiscard]] InputRequest promptFor(int step) const {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        switch (step) {
            case 0: request.prompt = "Specify angle vertex"; break;
            case 1:
                request.prompt = "Specify first angle endpoint";
                request.basePoint = points_[0];
                break;
            case 2:
                request.prompt = "Specify second angle endpoint";
                request.basePoint = points_[0];
                break;
            default:
                request.prompt = "Specify dimension arc location";
                request.basePoint = points_[0];
                break;
        }
        return request;
    }

    [[nodiscard]] std::unique_ptr<DimensionEntity> build(const Vec3& location,
                                                         CommandContext& context) const {
        auto dimension = std::make_unique<DimensionEntity>();
        dimension->type = DimensionType::Angular3Point;
        dimension->point15 = points_[0];  // vertex
        dimension->point13 = points_[1];
        dimension->point14 = points_[2];
        // The arc passes through the picked point, which is what sets its
        // radius — the user places the arc rather than typing an offset.
        dimension->definitionPoint = location;
        dimension->styleHandle = currentDimStyle(context);
        return dimension;
    }

    int picked_ = 0;
    std::array<Vec3, 3> points_{};
};

/// DIMORDINATE.
class OrdinateDimensionCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& /*context*/) override {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = "Specify feature location";
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }
        if (input.kind != InputValue::Kind::Point) {
            return StepResult::cancelled();
        }

        if (!hasFeature_) {
            feature_ = input.point;
            hasFeature_ = true;
            InputRequest request;
            request.kind = InputRequest::Kind::Point;
            request.prompt = "Specify leader endpoint";
            request.basePoint = feature_;
            return StepResult::ask(request);
        }

        context.transaction().addToCurrentSpace(build(input.point, context));
        return StepResult::done();
    }

    [[nodiscard]] std::vector<std::unique_ptr<Entity>> preview(
        const Vec3& cursor, CommandContext& context) const override {
        if (!hasFeature_) {
            return {};
        }
        std::vector<std::unique_ptr<Entity>> items;
        items.push_back(build(cursor, context));
        return items;
    }

private:
    [[nodiscard]] std::unique_ptr<DimensionEntity> build(const Vec3& leader,
                                                         CommandContext& context) const {
        auto dimension = std::make_unique<DimensionEntity>();
        dimension->type = DimensionType::Ordinate;
        // The datum is the drawing's origin, which is what group 10 means for
        // an ordinate dimension.
        dimension->definitionPoint = context.db().settings.insertionBase;
        dimension->point13 = feature_;
        dimension->point14 = leader;
        // Which axis is measured follows the drag: leading the label away
        // vertically measures X, horizontally measures Y.
        const Vec3 offset = leader - feature_;
        dimension->isXDatum = std::abs(offset.y) > std::abs(offset.x);
        dimension->styleHandle = currentDimStyle(context);
        return dimension;
    }

    bool hasFeature_ = false;
    Vec3 feature_;
};

// ---------------------------------------------------------------- boundaries

constexpr double kChainTolerance = 1e-7;

/// Turn a curve into the hatch edge that represents it exactly.
///
/// Exactly, not approximately: a circular boundary stored as a polyline would
/// hatch with visibly faceted edges and would not round-trip.
[[nodiscard]] std::optional<HatchEdge> edgeFromCurve(const Curve& curve, bool reversed) {
    if (const auto* line = dynamic_cast<const LineSegment*>(&curve)) {
        return HatchEdge::line(reversed ? line->end : line->start,
                               reversed ? line->start : line->end);
    }
    if (const auto* arc = dynamic_cast<const Arc*>(&curve)) {
        // The angles describe the shape; the flag says which way the loop runs
        // through it.
        return HatchEdge::arc(arc->center, arc->radius, arc->startAngle, arc->endAngle,
                              !reversed);
    }
    if (const auto* ellipse = dynamic_cast<const EllipseArc*>(&curve)) {
        HatchEdge edge;
        edge.kind = HatchEdge::Kind::Ellipse;
        edge.center = ellipse->center;
        edge.majorAxis = ellipse->majorAxis;
        edge.ratio = ellipse->ratio;
        edge.startAngle = ellipse->startParam;
        edge.endAngle = ellipse->endParam;
        edge.counterClockwise = !reversed;
        return edge;
    }
    return std::nullopt;
}

/// Chain loose curves into closed loops.
///
/// A hatch boundary has to be a closed ring; a set of selected lines and arcs
/// is not one until it is walked end to end. Greedy chaining is enough here
/// because the input is a user's selection rather than arbitrary soup: the
/// curves either meet or they do not, and telling the user which is more useful
/// than guessing.
[[nodiscard]] std::vector<std::vector<CurvePtr>> chainLoops(std::vector<CurvePtr> curves) {
    std::vector<std::vector<CurvePtr>> loops;
    std::vector<bool> used(curves.size(), false);

    for (std::size_t start = 0; start < curves.size(); ++start) {
        if (used[start]) {
            continue;
        }
        used[start] = true;

        std::vector<CurvePtr> loop;
        Vec3 first = curves[start]->startPoint();
        Vec3 end = curves[start]->endPoint();
        loop.push_back(std::move(curves[start]));

        bool grew = true;
        while (grew && !end.equals(first, Tolerance{kChainTolerance})) {
            grew = false;
            for (std::size_t i = 0; i < curves.size(); ++i) {
                if (used[i] || curves[i] == nullptr) {
                    continue;
                }
                if (curves[i]->startPoint().equals(end, Tolerance{kChainTolerance})) {
                    end = curves[i]->endPoint();
                } else if (curves[i]->endPoint().equals(end, Tolerance{kChainTolerance})) {
                    // Runs the other way; reversing keeps the loop continuous.
                    curves[i] = curves[i]->reversed();
                    end = curves[i]->endPoint();
                } else {
                    continue;
                }
                used[i] = true;
                loop.push_back(std::move(curves[i]));
                grew = true;
                break;
            }
        }

        // An open chain is not a boundary. Dropped here rather than filled
        // with an invented closing segment, which would hatch a region the
        // user never drew.
        if (end.equals(first, Tolerance{kChainTolerance}) && !loop.empty()) {
            loops.push_back(std::move(loop));
        }
    }
    return loops;
}

/// Build hatch boundary paths from a selection.
[[nodiscard]] std::vector<HatchBoundaryPath> boundariesFrom(const std::vector<Handle>& handles,
                                                            Database& db, std::size_t& skipped) {
    std::vector<HatchBoundaryPath> paths;
    std::vector<CurvePtr> loose;
    std::vector<Handle> looseSources;
    skipped = 0;

    for (const Handle handle : handles) {
        const Entity* entity = db.getEntity(handle);
        if (entity == nullptr || entity->erased) {
            continue;
        }

        // Closed entities are boundaries in their own right, and keeping them
        // whole preserves their exact form.
        if (const auto* polyline = dynamic_cast<const PolylineEntity*>(entity);
            polyline != nullptr && polyline->closed) {
            HatchBoundaryPath path;
            path.flags.isPolyline = true;
            path.closed = true;
            path.vertices = polyline->vertices;
            path.sourceHandles.push_back(handle);
            paths.push_back(std::move(path));
            continue;
        }
        if (const auto* circle = dynamic_cast<const CircleEntity*>(entity)) {
            HatchBoundaryPath path;
            path.edges.push_back(
                HatchEdge::arc(circle->center, circle->radius, 0.0, kTau, true));
            path.sourceHandles.push_back(handle);
            paths.push_back(std::move(path));
            continue;
        }

        std::vector<CurvePtr> curves = entity->toCurves();
        if (curves.empty()) {
            ++skipped;
            continue;
        }
        for (CurvePtr& curve : curves) {
            loose.push_back(std::move(curve));
            looseSources.push_back(handle);
        }
    }

    for (std::vector<CurvePtr>& loop : chainLoops(std::move(loose))) {
        HatchBoundaryPath path;
        for (const CurvePtr& curve : loop) {
            if (auto edge = edgeFromCurve(*curve, false)) {
                path.edges.push_back(std::move(*edge));
            } else {
                // No exact edge for this curve type — a polyline inside the
                // chain, for instance. Its tessellation is used instead, which
                // is a faithful approximation rather than a dropped boundary.
                const std::vector<Vec3> points = curve->tessellate();
                for (std::size_t i = 0; i + 1 < points.size(); ++i) {
                    path.edges.push_back(HatchEdge::line(points[i], points[i + 1]));
                }
            }
        }
        if (!path.edges.empty()) {
            path.sourceHandles = looseSources;
            paths.push_back(std::move(path));
        }
    }

    // The largest loop encloses the rest, so it is the outer boundary and the
    // others are islands. Getting this wrong inverts which parts are filled.
    if (!paths.empty()) {
        std::size_t outer = 0;
        double largest = -1.0;
        for (std::size_t i = 0; i < paths.size(); ++i) {
            const double area = paths[i].bounds().width() * paths[i].bounds().height();
            if (area > largest) {
                largest = area;
                outer = i;
            }
        }
        paths[outer].flags.isExternal = true;
        paths[outer].flags.isOutermost = true;
    }
    return paths;
}

/// HATCH — fill a region bounded by selected objects.
///
/// Boundaries come from a *selection*, not from a seed point. Seed-point
/// tracing — picking an empty spot and having the region worked out — is what
/// BHATCH does and is a substantially larger algorithm; it needs a planar
/// arrangement of every nearby edge. Selecting the boundary is the honest
/// subset, and it is exactly what the associativity data records anyway.
class HatchCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& context) override {
        if (!context.initialSelection().empty()) {
            // Pickfirst: the user selected the boundary before typing HATCH.
            selection_ = context.initialSelection();
            return askPattern();
        }
        InputRequest request;
        request.kind = InputRequest::Kind::Selection;
        request.prompt = "Select objects that bound the hatch";
        request.minCount = 1;
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }

        switch (stage_) {
            case Stage::Selection: {
                if (input.kind == InputValue::Kind::Selection) {
                    selection_ = input.selection;
                } else if (input.kind == InputValue::Kind::Entity) {
                    selection_ = {input.entity};
                }
                if (selection_.empty()) {
                    return StepResult::cancelled("Nothing selected.");
                }
                return askPattern();
            }

            case Stage::Pattern: {
                if (input.kind == InputValue::Kind::Keyword && matchesKeyword(input.text, 'S')) {
                    patternName_ = kHatchPatternSolid;
                } else if (input.kind == InputValue::Kind::Text && !input.text.empty()) {
                    patternName_ = input.text;
                }
                if (normalizeSymbolName(patternName_) ==
                    normalizeSymbolName(kHatchPatternSolid)) {
                    // A solid fill has no scale or angle to ask about.
                    return create(context);
                }
                stage_ = Stage::Scale;
                InputRequest request;
                request.kind = InputRequest::Kind::Number;
                request.prompt = "Specify pattern scale";
                request.defaultNumber = scale_;
                request.allowEmpty = true;
                request.minimum = 1e-6;
                return StepResult::ask(request);
            }

            case Stage::Scale: {
                if (input.kind == InputValue::Kind::Number && input.number > 0.0) {
                    scale_ = input.number;
                }
                stage_ = Stage::Angle;
                InputRequest request;
                request.kind = InputRequest::Kind::Angle;
                request.prompt = "Specify pattern angle";
                request.defaultNumber = 0.0;
                request.allowEmpty = true;
                return StepResult::ask(request);
            }

            case Stage::Angle:
            default: {
                if (input.kind == InputValue::Kind::Number) {
                    angle_ = input.number;
                }
                return create(context);
            }
        }
    }

private:
    enum class Stage : std::uint8_t { Selection, Pattern, Scale, Angle };

    [[nodiscard]] StepResult askPattern() {
        stage_ = Stage::Pattern;
        InputRequest request;
        request.kind = InputRequest::Kind::Text;
        request.prompt = "Enter pattern name";
        request.defaultText = patternName_;
        request.allowEmpty = true;
        request.keywords.push_back(kSolidKeyword);
        return StepResult::ask(request);
    }

    [[nodiscard]] StepResult create(CommandContext& context) {
        std::size_t skipped = 0;
        std::vector<HatchBoundaryPath> paths =
            boundariesFrom(selection_, context.db(), skipped);
        if (paths.empty()) {
            return StepResult::failed(
                "The selected objects do not form a closed boundary.");
        }
        if (skipped > 0) {
            context.warn(std::to_string(skipped) +
                         " selected object(s) have no boundary geometry and were ignored.");
        }

        auto hatch = std::make_unique<HatchEntity>();
        hatch->paths = std::move(paths);
        hatch->isAssociative = true;

        if (normalizeSymbolName(patternName_) == normalizeSymbolName(kHatchPatternSolid)) {
            hatch->pattern.name = kHatchPatternSolid;
            hatch->pattern.isSolid = true;
        } else if (const auto known = standardHatchPattern(patternName_)) {
            hatch->pattern = *known;
            hatch->pattern.scale = scale_;
            hatch->pattern.angle = angle_;
        } else {
            return StepResult::failed("Unknown hatch pattern '" + patternName_ + "'.");
        }

        context.transaction().addToCurrentSpace(std::move(hatch));
        return StepResult::done();
    }

    Stage stage_ = Stage::Selection;
    std::vector<Handle> selection_;
    std::string patternName_ = "ANSI31";
    double scale_ = 1.0;
    double angle_ = 0.0;
};

}  // namespace

/// LEADER — point at something and say what it is.
///
/// The command a drawing set is made of. Arrow point first, because that is the
/// thing being called out and everything else is placement; then as many bends
/// as the note needs to get clear of the geometry; then the note itself.
///
/// The note is created as a separate MTEXT and linked, which is how DXF stores
/// it. That also means it is editable by double-clicking it, movable on its own
/// grips, and searchable, all without this command knowing anything about text.
class LeaderCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& /*context*/) override {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = "Specify arrow point";
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }

        if (awaitingText_) {
            const std::string note =
                input.kind == InputValue::Kind::Text ? input.text : std::string{};
            return create(note, context);
        }

        if (input.kind == InputValue::Kind::Empty) {
            // Enter ends the path. Two points are the least that draws a
            // leader; one is a click that has not become anything yet.
            if (vertices_.size() < 2) {
                return StepResult::cancelled();
            }
            return askText();
        }

        if (input.kind != InputValue::Kind::Point) {
            return StepResult::cancelled();
        }

        vertices_.push_back(input.point);
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = vertices_.size() == 1 ? "Specify next point"
                                               : "Specify next point, or press Enter for the note";
        request.basePoint = vertices_.back();
        request.allowEmpty = vertices_.size() >= 2;
        return StepResult::ask(request);
    }

    [[nodiscard]] std::vector<std::unique_ptr<Entity>> preview(
        const Vec3& cursor, CommandContext& context) const override {
        if (vertices_.empty() || awaitingText_) {
            return {};
        }
        // The arrow is drawn in the preview too. Where the arrow ends up is
        // most of what the first two picks decide, and a rubber band without
        // one says nothing about which end is the pointed end.
        auto ghost = std::make_unique<LeaderEntity>();
        ghost->vertices = vertices_;
        ghost->vertices.push_back(cursor);
        ghost->styleHandle = currentDimStyle(context);
        std::vector<std::unique_ptr<Entity>> ghosts;
        ghosts.push_back(std::move(ghost));
        return ghosts;
    }

private:
    [[nodiscard]] StepResult askText() {
        awaitingText_ = true;
        InputRequest request;
        request.kind = InputRequest::Kind::Text;
        request.prompt = "Enter the note";
        request.allowEmpty = true;
        return StepResult::ask(request);
    }

    StepResult create(const std::string& note, CommandContext& context) {
        Database& db = context.db();
        const Handle style = currentDimStyle(context);

        auto leader = std::make_unique<LeaderEntity>();
        leader->vertices = vertices_;
        leader->styleHandle = style;

        const DimStyle& resolved = db.dimStyleByHandle(style);

        // The landing is a real vertex, not a flag. A LEADER with the hookline
        // flag set invites the reader to reconstruct the landing itself, and
        // other CAD software does: it fanned every leader out into a straight line to the
        // arrow plus a separate horizontal that ended in mid-air, ignoring the
        // elbow that was picked. Written as plain vertices with no hookline,
        // the file *is* the picture, and every consumer draws the same
        // polyline this one does.
        const Vec3 landing = leader->landingDirection();
        const double reach = resolved.scaled(resolved.arrowSize) + resolved.scaled(resolved.textGap);
        leader->vertices.push_back(leader->vertices.back() + landing * reach);
        leader->hasLanding = false;
        leader->hasHorizontalDirection = true;
        leader->horizontalDirection = landing;

        const LeaderGeometry geometry = leader->geometry(resolved);
        const double height = resolved.scaled(resolved.textHeight);

        Handle annotation;
        if (!note.empty()) {
            auto text = std::make_unique<MTextEntity>(geometry.landingEnd, note, height);
            // Hung from the middle of the end of the landing, on the side the
            // leader arrived from, with a gap so the text does not touch the
            // line it sits on.
            const double gap = resolved.scaled(resolved.textGap);
            text->position.x += geometry.landingToTheRight ? gap : -gap;
            text->attachment = geometry.landingToTheRight ? MTextAttachment::MiddleLeft
                                                          : MTextAttachment::MiddleRight;
            text->styleHandle = resolved.textStyleHandle;
            text->layerHandle = leader->layerHandle;
            annotation = context.transaction().addToCurrentSpace(std::move(text)).handle;
        }

        leader->setAnnotation(annotation);
        leader->textHeight = height;

        context.transaction().addToCurrentSpace(std::move(leader));
        return StepResult::done(annotation.isNull() ? "Leader drawn" : "Leader and note drawn");
    }

    std::vector<Vec3> vertices_;
    bool awaitingText_ = false;
};

/// DIMSTYLE: make or change a dimension style, and dimension with it.
///
/// The settings that decide how a dimension looks on the sheet, asked one
/// by one with the style's own values as defaults, then the style is made
/// current. A new style starts from the current one. `?` lists the styles.
class DimStyleCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& context) override {
        InputRequest request;
        request.kind = InputRequest::Kind::Text;
        request.prompt = "Enter dimension style name or [?]";
        request.defaultText = context.db().settings.currentDimStyleName;
        request.allowEmpty = true;
        request.keywords = {CommandKeyword{"?", "?", "List the styles"}};
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }
        Database& db = context.db();
        const bool number = input.kind == InputValue::Kind::Number;
        switch (stage_) {
            case Stage::Name: {
                const std::string typed = input.kind == InputValue::Kind::Text ||
                                                  input.kind == InputValue::Kind::Keyword
                                              ? input.text
                                              : std::string{};
                if (typed == "?") {
                    for (const Handle handle : db.dimStyles().handles()) {
                        if (const auto* style = db.get<DimStyle>(handle)) {
                            context.log("  " + style->name + "  text " +
                                        std::to_string(style->textHeight) + "  arrows " +
                                        std::to_string(style->arrowSize) + "  scale " +
                                        std::to_string(style->overallScale));
                        }
                    }
                    return start(context);
                }
                name_ = typed.empty() ? db.settings.currentDimStyleName : typed;
                const DimStyle* base = db.findDimStyle(name_);
                if (base == nullptr) {
                    base = &db.ensureDimStyle(db.settings.currentDimStyleName);
                }
                // A working copy of the style, edited as the answers come in
                // and written back in one go at the end.
                draft_ = base->clone();
                stage_ = Stage::ArrowSize;
                return ask("Arrow size", values().arrowSize);
            }
            case Stage::ArrowSize:
                if (number && input.number >= 0.0) values().arrowSize = input.number;
                stage_ = Stage::TextHeight;
                return ask("Text height", values().textHeight);
            case Stage::TextHeight:
                if (number && input.number > 0.0) values().textHeight = input.number;
                stage_ = Stage::TextGap;
                return ask("Gap between text and dimension line", values().textGap);
            case Stage::TextGap:
                if (number && input.number >= 0.0) values().textGap = input.number;
                stage_ = Stage::ExtensionOffset;
                return ask("Extension line offset from the object", values().extensionLineOffset);
            case Stage::ExtensionOffset:
                if (number && input.number >= 0.0) values().extensionLineOffset = input.number;
                stage_ = Stage::ExtensionExtend;
                return ask("Extension line beyond the dimension line", values().extensionLineExtend);
            case Stage::ExtensionExtend:
                if (number && input.number >= 0.0) values().extensionLineExtend = input.number;
                stage_ = Stage::BaselineSpacing;
                return ask("Baseline spacing", values().baselineSpacing);
            case Stage::BaselineSpacing:
                if (number && input.number > 0.0) values().baselineSpacing = input.number;
                stage_ = Stage::Scale;
                return ask("Overall scale", values().overallScale);
            case Stage::Scale:
                if (number && input.number > 0.0) values().overallScale = input.number;
                stage_ = Stage::Precision;
                return ask("Decimal places", values().linearPrecision, true);
            case Stage::Precision:
                if (number && input.number >= 0.0 && input.number <= 8.0) {
                    values().linearPrecision = static_cast<int>(input.number);
                }
                stage_ = Stage::Placement;
                {
                    InputRequest request;
                    request.kind = InputRequest::Kind::Text;
                    request.prompt = "Text placement [Above/Centered]";
                    request.defaultText =
                        values().textVerticalPosition == TextVerticalPosition::Centered ? "Centered" : "Above";
                    request.allowEmpty = true;
                    request.keywords = {CommandKeyword{"Above", "A", "Text above the dimension line"},
                                        CommandKeyword{"Centered", "C", "Text in a gap in the line"}};
                    return StepResult::ask(request);
                }
            case Stage::Placement:
                if (input.kind == InputValue::Kind::Keyword || input.kind == InputValue::Kind::Text) {
                    if (matchesKeyword(input.text, 'A')) {
                        values().textVerticalPosition = TextVerticalPosition::Above;
                    } else if (matchesKeyword(input.text, 'C')) {
                        values().textVerticalPosition = TextVerticalPosition::Centered;
                    }
                }
                return apply(context);
        }
        return StepResult::cancelled();
    }

private:
    enum class Stage {
        Name, ArrowSize, TextHeight, TextGap, ExtensionOffset, ExtensionExtend,
        BaselineSpacing, Scale, Precision, Placement
    };

    [[nodiscard]] static StepResult ask(std::string prompt, double fallback, bool integer = false) {
        InputRequest request;
        request.kind = integer ? InputRequest::Kind::Number : InputRequest::Kind::Distance;
        request.prompt = std::move(prompt);
        request.defaultNumber = fallback;
        request.allowEmpty = true;
        request.integerOnly = integer;
        return StepResult::ask(request);
    }

    StepResult apply(CommandContext& context) {
        Database& db = context.db();
        Transaction& transaction = context.transaction();
        DimStyle* style = db.findDimStyle(name_);
        const bool made = style == nullptr;
        if (made) {
            DbObjectPtr fresh = draft_->clone();
            auto& dim = static_cast<DimStyle&>(*fresh);
            dim.handle = Handle::null();
            dim.name = name_;
            style = &static_cast<DimStyle&>(transaction.addObject(std::move(fresh)));
        } else {
            DimStyle& target = transaction.openForWrite(*style);
            const Handle handle = target.handle;
            const std::string name = target.name;
            target.restoreFrom(*draft_);
            target.handle = handle;
            target.name = name;
        }
        db.settings.currentDimStyleName = style->name;
        return StepResult::done("Dimension style \"" + style->name +
                                (made ? "\" made and current" : "\" changed and current"));
    }

    Stage stage_ = Stage::Name;
    std::string name_;
    DbObjectPtr draft_;
    /// The draft's fields, for reading defaults and writing answers.
    [[nodiscard]] DimStyle& values() { return static_cast<DimStyle&>(*draft_); }
};

const CommandKeyword kSelectDimensionKeyword{"Select", "S", "Pick the dimension to continue from"};

/// DIMCONTINUE and DIMBASELINE: a run of dimensions off the last linear one.
///
/// Continued dimensions sit end to end on the same line, each starting where
/// the previous ended. Baseline dimensions all start from the first one's
/// first point, each a line further out by the style's baseline spacing. The
/// run starts from the newest linear dimension in the space, or from one
/// picked with Select, and ends on Enter.
class ChainedDimensionCommand final : public CommandInstance {
public:
    explicit ChainedDimensionCommand(bool baseline) : baseline_(baseline) {}

    StepResult start(CommandContext& context) override {
        if (const DimensionEntity* last = newestLinear(context)) {
            adopt(*last, context);
            return askPoint();
        }
        awaitingBase_ = true;
        return askBase();
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return finish();
        }
        if (awaitingBase_) {
            Handle picked;
            if (input.kind == InputValue::Kind::Entity) {
                picked = input.entity;
            } else if (input.kind == InputValue::Kind::Selection && !input.selection.empty()) {
                picked = input.selection.front();
            }
            const auto* base = context.db().get<DimensionEntity>(picked);
            if (base == nullptr || base->erased || !isLinear(*base)) {
                return StepResult::failed(
                    "Select a linear or aligned dimension to continue from.");
            }
            adopt(*base, context);
            awaitingBase_ = false;
            return askPoint();
        }
        if (input.kind == InputValue::Kind::Keyword && matchesKeyword(input.text, 'S')) {
            awaitingBase_ = true;
            return askBase();
        }
        if (input.kind != InputValue::Kind::Point) {
            return finish();  // Enter ends the run
        }
        context.transaction().addToCurrentSpace(build(input.point, context));
        advance(input.point);
        return askPoint();
    }

    [[nodiscard]] std::vector<std::unique_ptr<Entity>> preview(
        const Vec3& cursor, CommandContext& context) const override {
        if (awaitingBase_ || styleHandle_.isNull()) {
            return {};
        }
        std::vector<std::unique_ptr<Entity>> items;
        items.push_back(build(cursor, context));
        return items;
    }

private:
    [[nodiscard]] static bool isLinear(const DimensionEntity& dimension) {
        return dimension.type == DimensionType::Rotated || dimension.type == DimensionType::Aligned;
    }

    /// The newest linear dimension in the current space -- the one a drafter
    /// has just placed, nearly always.
    [[nodiscard]] static const DimensionEntity* newestLinear(CommandContext& context) {
        const DimensionEntity* newest = nullptr;
        for (const Entity* entity : context.db().entitiesIn(context.db().currentSpace())) {
            const auto* dimension = dynamic_cast<const DimensionEntity*>(entity);
            if (dimension != nullptr && !dimension->erased && isLinear(*dimension)) {
                newest = dimension;
            }
        }
        return newest;
    }

    void adopt(const DimensionEntity& base, CommandContext& context) {
        firstPoint_ = base.point13;
        lastPoint_ = base.point14;
        definitionPoint_ = base.definitionPoint;
        // An aligned base gives the run its direction; from there the run
        // measures along that line, which is what a chain of dimensions is.
        rotation_ = base.type == DimensionType::Aligned ? (base.point14 - base.point13).angle2()
                                                        : base.rotation;
        along_ = Vec3{std::cos(rotation_), std::sin(rotation_), 0.0};
        across_ = Vec3{-along_.y, along_.x, 0.0};
        // Which side of the measured points the dimension line lies on, so a
        // baseline set grows away from them rather than back through them.
        const double offset = (base.definitionPoint - base.point13).dot(across_);
        side_ = offset < 0.0 ? -1.0 : 1.0;
        styleHandle_ = base.styleHandle;
        layerHandle_ = base.layerHandle;
        spacing_ = context.db().dimStyleByHandle(styleHandle_).scaled(
            context.db().dimStyleByHandle(styleHandle_).baselineSpacing);
        steps_ = 0;
    }

    [[nodiscard]] std::unique_ptr<DimensionEntity> build(const Vec3& point,
                                                         CommandContext& /*context*/) const {
        auto dimension = std::make_unique<DimensionEntity>();
        dimension->type = DimensionType::Rotated;
        dimension->rotation = rotation_;
        dimension->styleHandle = styleHandle_;
        dimension->layerHandle = layerHandle_;
        dimension->point13 = baseline_ ? firstPoint_ : lastPoint_;
        dimension->point14 = point;
        // On the base dimension's line -- or, for a baseline, one spacing
        // further out per dimension already placed.
        const double lift = baseline_ ? side_ * spacing_ * static_cast<double>(steps_ + 1) : 0.0;
        const Vec3 onLine = point + across_ * (definitionPoint_ - point).dot(across_);
        dimension->definitionPoint = onLine + across_ * lift;
        return dimension;
    }

    void advance(const Vec3& point) {
        if (!baseline_) {
            lastPoint_ = point;
        }
        ++steps_;
    }

    [[nodiscard]] StepResult askPoint() const {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = "Specify second extension line origin or [Select]";
        request.keywords.push_back(kSelectDimensionKeyword);
        request.basePoint = baseline_ ? firstPoint_ : lastPoint_;
        return StepResult::ask(request);
    }

    [[nodiscard]] StepResult askBase() const {
        InputRequest request;
        request.kind = InputRequest::Kind::Entity;
        request.prompt = baseline_ ? "Select base dimension" : "Select continued dimension";
        return StepResult::ask(request);
    }

    [[nodiscard]] StepResult finish() const {
        if (steps_ == 0) {
            return StepResult::cancelled();
        }
        return StepResult::done(std::to_string(steps_) + " dimension(s) added");
    }

    bool baseline_ = false;
    bool awaitingBase_ = false;
    Vec3 firstPoint_;
    Vec3 lastPoint_;
    Vec3 definitionPoint_;
    Vec3 along_;
    Vec3 across_;
    double rotation_ = 0.0;
    double side_ = 1.0;
    double spacing_ = 3.75;
    Handle styleHandle_;
    Handle layerHandle_;
    int steps_ = 0;
};

std::vector<CommandDefinition> annotationCommands() {
    std::vector<CommandDefinition> commands;

    {
        CommandDefinition linear;
        linear.id = "cad.dim.linear";
        linear.name = "DIMLINEAR";
        linear.aliases = {"DLI", "DIMLIN"};
        // Titles are the ribbon's button labels, so they are the short form.
        // The panel is already captioned "Dimension"; repeating it on every
        // button costs width and tells the reader nothing.
        linear.title = "Linear";
        linear.description = "Dimension a horizontal or vertical distance";
        linear.category = "Dimension";
        linear.create = [] { return std::make_unique<LinearDimensionCommand>(false); };
        commands.push_back(std::move(linear));
    }
    {
        CommandDefinition aligned;
        aligned.id = "cad.dim.aligned";
        aligned.name = "DIMALIGNED";
        aligned.aliases = {"DAL", "DIMALI"};
        aligned.title = "Aligned";
        aligned.description = "Dimension a distance parallel to the measured points";
        aligned.category = "Dimension";
        aligned.create = [] { return std::make_unique<LinearDimensionCommand>(true); };
        commands.push_back(std::move(aligned));
    }
    {
        CommandDefinition chain;
        chain.id = "cad.dim.continue";
        chain.name = "DIMCONTINUE";
        chain.aliases = {"DCO", "DIMCONT"};
        chain.title = "Continue";
        chain.description = "Dimensions end to end from the last one";
        chain.category = "Dimension";
        chain.create = [] { return std::make_unique<ChainedDimensionCommand>(false); };
        commands.push_back(std::move(chain));
    }
    {
        CommandDefinition baseline;
        baseline.id = "cad.dim.baseline";
        baseline.name = "DIMBASELINE";
        baseline.aliases = {"DBA", "DIMBASE"};
        baseline.title = "Baseline";
        baseline.description = "Dimensions from one origin, each a line further out";
        baseline.category = "Dimension";
        baseline.create = [] { return std::make_unique<ChainedDimensionCommand>(true); };
        commands.push_back(std::move(baseline));
    }
    {
        CommandDefinition style;
        style.id = "cad.dim.style";
        style.name = "DIMSTYLE";
        style.aliases = {"D", "DST", "-DIMSTYLE"};
        style.title = "Dim Style";
        style.description = "Make or change a dimension style and dimension with it";
        style.category = "Dimension";
        style.create = [] { return std::make_unique<DimStyleCommand>(); };
        commands.push_back(std::move(style));
    }
    {
        CommandDefinition radius;
        radius.id = "cad.dim.radius";
        radius.name = "DIMRADIUS";
        radius.aliases = {"DRA", "DIMRAD"};
        radius.title = "Radius";
        radius.description = "Dimension the radius of an arc or circle";
        radius.category = "Dimension";
        radius.create = [] { return std::make_unique<RadialDimensionCommand>(false); };
        commands.push_back(std::move(radius));
    }
    {
        CommandDefinition diameter;
        diameter.id = "cad.dim.diameter";
        diameter.name = "DIMDIAMETER";
        diameter.aliases = {"DDI", "DIMDIA"};
        diameter.title = "Diameter";
        diameter.description = "Dimension the diameter of an arc or circle";
        diameter.category = "Dimension";
        diameter.create = [] { return std::make_unique<RadialDimensionCommand>(true); };
        commands.push_back(std::move(diameter));
    }
    {
        CommandDefinition angular;
        angular.id = "cad.dim.angular";
        angular.name = "DIMANGULAR";
        angular.aliases = {"DAN", "DIMANG"};
        angular.title = "Angular";
        angular.description = "Dimension the angle at a vertex";
        angular.category = "Dimension";
        angular.create = [] { return std::make_unique<AngularDimensionCommand>(); };
        commands.push_back(std::move(angular));
    }
    {
        CommandDefinition ordinate;
        ordinate.id = "cad.dim.ordinate";
        ordinate.name = "DIMORDINATE";
        ordinate.aliases = {"DOR", "DIMORD"};
        ordinate.title = "Ordinate";
        ordinate.description = "Dimension a feature's offset from the drawing datum";
        ordinate.category = "Dimension";
        ordinate.create = [] { return std::make_unique<OrdinateDimensionCommand>(); };
        commands.push_back(std::move(ordinate));
    }
    {
        CommandDefinition leader;
        leader.id = "cad.dim.leader";
        leader.name = "LEADER";
        // MLEADER too: it is the name a drafter trained on any other CAD software since
        // 2008 will type, and refusing it because this writes the older entity
        // would be a distinction that matters to nobody at the command line.
        leader.aliases = {"LE", "MLEADER", "MLD", "QLEADER"};
        leader.title = "Leader";
        leader.description = "Draw an arrow with a note at the end of it";
        leader.category = "Dimension";
        leader.create = [] { return std::make_unique<LeaderCommand>(); };
        commands.push_back(std::move(leader));
    }
    {
        CommandDefinition hatch;
        hatch.id = "cad.draw.hatch";
        hatch.name = "HATCH";
        // BH as well as H: both are stock acad.pgp, and an alias exists to be
        // reached without thinking.
        hatch.aliases = {"H", "BH", "BHATCH"};
        hatch.title = "Hatch";
        hatch.description = "Fill a region bounded by selected objects";
        hatch.category = "Draw";
        hatch.supportsPickfirst = true;
        hatch.create = [] { return std::make_unique<HatchCommand>(); };
        commands.push_back(std::move(hatch));
    }

    return commands;
}

}  // namespace cad
