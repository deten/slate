#include <cmath>
#include <algorithm>
#include <cctype>

#include "cad/command/StandardCommands.hpp"
#include "cad/db/ConstructionLineEntity.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/MlineEntity.hpp"
#include "cad/db/SplineEntity.hpp"
#include "cad/geom/Arc.hpp"
#include "cad/geom/EllipseArc.hpp"
#include "cad/geom/LineSegment.hpp"

namespace cad {
namespace {

const CommandKeyword kUndoKeyword{"Undo", "U", "Remove the last point"};
const CommandKeyword kCloseKeyword{"Close", "C", "Close back to the first point"};
const CommandKeyword kCenterKeyword{"Center", "C", "Give the middle instead of an axis end"};
const CommandKeyword kArcKeyword{"Arc", "A", "Draw arc segments, tangent to the last one"};
const CommandKeyword kLineKeyword{"Line", "L", "Go back to drawing straight segments"};

/// The prompt shared by the multi-point drawing commands.
[[nodiscard]] InputRequest pointRequest(const std::vector<Vec3>& points) {
    InputRequest request;
    request.kind = InputRequest::Kind::Point;
    request.prompt = points.empty() ? "Specify first point" : "Specify next point";
    if (!points.empty()) {
        request.basePoint = points.back();
        request.keywords.push_back(kUndoKeyword);
        request.allowEmpty = true;
    }
    if (points.size() >= 3) {
        request.keywords.insert(request.keywords.begin(), kCloseKeyword);
    }
    return request;
}

/// The direction a polyline segment leaves its far end travelling in.
///
/// What the next arc has to be tangent to, which is what makes a chain of arcs
/// flow rather than kink at every join.
[[nodiscard]] Vec3 segmentEndTangent(const PolylineVertex& from, const Vec3& to) {
    Vec3 chord = to - from.position;
    chord.z = 0.0;
    const double span = chord.length2();
    if (span < 1e-12) {
        return Vec3{1.0, 0.0, 0.0};
    }
    const Vec3 direction{chord.x / span, chord.y / span, 0.0};
    if (from.bulge == 0.0) {
        return direction;
    }
    // Half the included angle, which is the angle between the chord and the
    // tangent at either end -- signed the other way at the far end.
    const double half = 2.0 * std::atan(from.bulge);
    return Vec3{direction.x * std::cos(half) - direction.y * std::sin(half),
                direction.x * std::sin(half) + direction.y * std::cos(half), 0.0};
}

/// The bulge of an arc that leaves `tangent` and ends up across `chord`.
///
/// The tangent and the chord meet at half the included angle, and the bulge is
/// the tangent of a quarter of it -- so the whole arc falls out of one angle
/// with no radius or centre to find. Zero when the chord runs straight along
/// the tangent, which draws the straight segment the geometry asks for rather
/// than a degenerate arc.
[[nodiscard]] double bulgeFromTangent(const Vec3& tangent, const Vec3& chord) {
    const double across = tangent.x * chord.y - tangent.y * chord.x;
    const double along = tangent.x * chord.x + tangent.y * chord.y;
    if (std::abs(across) < 1e-12 && along >= 0.0) {
        return 0.0;
    }
    return std::tan(0.5 * std::atan2(across, along));
}

[[nodiscard]] bool matchesKeyword(const std::string& typed, char initial) {
    return !typed.empty() &&
           std::toupper(static_cast<unsigned char>(typed.front())) == initial;
}

/// LINE — draw a connected chain of line segments.
///
/// Continues prompting after each point, as other CAD software does, so drawing a polygon
/// outline is one command rather than several. Each segment is a separate LINE
/// entity, which is what distinguishes it from PLINE.
/// XLINE and RAY: construction lines through a point.
///
/// XLINE asks for a point and then, for each further point, lays an
/// infinite line through both, until Enter; or, by keyword first, a
/// Horizontal or Vertical line through each point picked, one at a given
/// Angle through each point, the Bisector of an angle given by a vertex and
/// two points, or a line Offset a distance from a line picked, on the side
/// pointed at. RAY asks for a start and then a line from it through each
/// further point.
class ConstructionLineCommand final : public CommandInstance {
public:
    explicit ConstructionLineCommand(bool bothWays) : bothWays_(bothWays) {}

    StepResult start(CommandContext& /*context*/) override {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        if (bothWays_) {
            request.prompt = "Specify a point or [Hor/Ver/Ang/Bisect/Offset]";
            request.keywords = {
                CommandKeyword{"Hor", "H", "Horizontal lines through the points picked"},
                CommandKeyword{"Ver", "V", "Vertical lines through the points picked"},
                CommandKeyword{"Ang", "A", "Lines at an angle through the points picked"},
                CommandKeyword{"Bisect", "B", "The bisector of an angle: vertex, then two points"},
                CommandKeyword{"Offset", "O", "A line a distance from a line picked"},
            };
        } else {
            request.prompt = "Specify start point";
        }
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }
        if (input.kind == InputValue::Kind::Empty) {
            return made_ > 0 ? StepResult::done(summary())
                             : StepResult::cancelled();
        }
        if (input.kind == InputValue::Kind::Keyword && bothWays_ && mode_ == Mode::Through &&
            !hasFirst_) {
            return chooseMode(input.text);
        }
        switch (mode_) {
            case Mode::Through:
                if (input.kind != InputValue::Kind::Point) {
                    return StepResult::cancelled();
                }
                if (!hasFirst_) {
                    first_ = input.point;
                    hasFirst_ = true;
                    return askThrough();
                }
                if (const auto unit = (input.point - first_).tryNormalize(); unit.has_value()) {
                    add(first_, *unit, context);
                }
                return askThrough();
            case Mode::Horizontal:
            case Mode::Vertical:
                if (input.kind != InputValue::Kind::Point) {
                    return StepResult::cancelled();
                }
                add(input.point, mode_ == Mode::Horizontal ? Vec3::unitX() : Vec3::unitY(), context);
                return askPoint("Specify through point");
            case Mode::Angle:
                if (!hasAngle_) {
                    if (input.kind == InputValue::Kind::Number) {
                        angle_ = input.number;
                    } else if (input.kind == InputValue::Kind::Point) {
                        angle_ = (input.point - Vec3{}).angle2();
                    } else {
                        return StepResult::cancelled();
                    }
                    hasAngle_ = true;
                    return askPoint("Specify through point");
                }
                if (input.kind != InputValue::Kind::Point) {
                    return StepResult::cancelled();
                }
                add(input.point, Vec3{std::cos(angle_), std::sin(angle_), 0.0}, context);
                return askPoint("Specify through point");
            case Mode::Bisect:
                if (input.kind != InputValue::Kind::Point) {
                    return StepResult::cancelled();
                }
                if (!hasFirst_) {
                    first_ = input.point;
                    hasFirst_ = true;
                    return askPoint("Specify angle start point");
                }
                if (!hasSecond_) {
                    second_ = input.point;
                    hasSecond_ = true;
                    return askPoint("Specify angle end point");
                }
                {
                    const auto a = (second_ - first_).tryNormalize();
                    const auto b = (input.point - first_).tryNormalize();
                    if (a.has_value() && b.has_value()) {
                        if (const auto bisector = (*a + *b).tryNormalize(); bisector.has_value()) {
                            add(first_, *bisector, context);
                        }
                    }
                }
                return askPoint("Specify angle end point");
            case Mode::Offset:
            default:
                if (!hasOffset_) {
                    if (input.kind != InputValue::Kind::Number) {
                        return StepResult::cancelled();
                    }
                    offset_ = std::abs(input.number);
                    hasOffset_ = true;
                    return askLine();
                }
                if (!hasLine_) {
                    if (input.kind != InputValue::Kind::Entity) {
                        return StepResult::cancelled();
                    }
                    const Entity* entity = context.db().getEntity(input.entity);
                    const std::vector<CurvePtr> curves =
                        entity == nullptr ? std::vector<CurvePtr>{} : entity->toCurves();
                    if (curves.empty() || curves.front() == nullptr) {
                        return StepResult::failed("That is not a line to offset from.");
                    }
                    lineBase_ = curves.front()->startPoint();
                    const auto unit = (curves.front()->endPoint() - lineBase_).tryNormalize();
                    if (!unit.has_value()) {
                        return StepResult::failed("That line has no direction.");
                    }
                    lineDirection_ = *unit;
                    hasLine_ = true;
                    return askPoint("Specify side to offset");
                }
                if (input.kind != InputValue::Kind::Point) {
                    return StepResult::cancelled();
                }
                {
                    const Vec3 left{-lineDirection_.y, lineDirection_.x, 0.0};
                    const double side = (input.point - lineBase_).dot(left) >= 0.0 ? 1.0 : -1.0;
                    add(lineBase_ + left * (offset_ * side), lineDirection_, context);
                }
                hasLine_ = false;
                return askLine();
        }
    }

private:
    enum class Mode : std::uint8_t { Through, Horizontal, Vertical, Angle, Bisect, Offset };

    StepResult chooseMode(std::string_view word) {
        const char first = word.empty() ? '\0' : static_cast<char>(std::toupper(
                                                     static_cast<unsigned char>(word.front())));
        switch (first) {
            case 'H':
                mode_ = Mode::Horizontal;
                return askPoint("Specify through point");
            case 'V':
                mode_ = Mode::Vertical;
                return askPoint("Specify through point");
            case 'A': {
                mode_ = Mode::Angle;
                InputRequest request;
                request.kind = InputRequest::Kind::Angle;
                request.prompt = "Enter angle of xline";
                request.defaultNumber = 0.0;
                return StepResult::ask(request);
            }
            case 'B':
                mode_ = Mode::Bisect;
                return askPoint("Specify angle vertex point");
            case 'O': {
                mode_ = Mode::Offset;
                InputRequest request;
                request.kind = InputRequest::Kind::Number;
                request.prompt = "Specify offset distance";
                request.minimum = 0.0;
                return StepResult::ask(request);
            }
            default:
                return StepResult::failed("Unknown option.");
        }
    }

    void add(const Vec3& through, const Vec3& direction, CommandContext& context) {
        context.transaction().addToCurrentSpace(
            std::make_unique<ConstructionLineEntity>(through, direction, bothWays_));
        ++made_;
    }

    [[nodiscard]] StepResult askThrough() const {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = "Specify through point";
        request.basePoint = first_;
        request.allowEmpty = true;
        return StepResult::ask(request);
    }

    [[nodiscard]] static StepResult askPoint(std::string prompt) {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = std::move(prompt);
        request.allowEmpty = true;
        return StepResult::ask(request);
    }

    [[nodiscard]] static StepResult askLine() {
        InputRequest request;
        request.kind = InputRequest::Kind::Entity;
        request.prompt = "Select a line object";
        request.allowEmpty = true;
        return StepResult::ask(request);
    }

    [[nodiscard]] std::string summary() const {
        return std::to_string(made_) + (bothWays_ ? " construction line" : " ray") +
               (made_ == 1 ? "" : "s") + " drawn";
    }

    bool bothWays_;
    Mode mode_ = Mode::Through;
    Vec3 first_;
    Vec3 second_;
    bool hasFirst_ = false;
    bool hasSecond_ = false;
    double angle_ = 0.0;
    bool hasAngle_ = false;
    double offset_ = 0.0;
    bool hasOffset_ = false;
    bool hasLine_ = false;
    Vec3 lineBase_;
    Vec3 lineDirection_;
    int made_ = 0;
};

/// MLINE: a multiline, two parallel lines drawn as one.
///
/// Walls in plan, mostly. The lines sit the scale apart, justified to
/// the picked points at the top, the middle or the bottom, and meet at
/// mitred corners. Drawn from the entity's own vertex data, as the file
/// carries it, so no style record is needed and a file's own multilines
/// and these are one kind of thing.
class MlineCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& /*context*/) override { return askPoint(); }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }
        if (input.kind == InputValue::Kind::Keyword || input.kind == InputValue::Kind::Text) {
            if (awaitingScale_) {
                awaitingScale_ = false;
                return askPoint();
            }
            const char first = input.text.empty() ? '\0' : static_cast<char>(std::toupper(
                                                              static_cast<unsigned char>(input.text.front())));
            if (first == 'J') {
                stage_ = Stage::Justification;
                InputRequest request;
                request.kind = InputRequest::Kind::Keyword;
                request.prompt = "Enter justification [Top/Zero/Bottom]";
                request.keywords = {
                    CommandKeyword{"Top", "T", "The points are the top line"},
                    CommandKeyword{"Zero", "Z", "The points are the centre"},
                    CommandKeyword{"Bottom", "B", "The points are the bottom line"},
                };
                request.defaultText = "Zero";
                request.allowEmpty = true;
                return StepResult::ask(request);
            }
            if (first == 'S') {
                awaitingScale_ = true;
                InputRequest request;
                request.kind = InputRequest::Kind::Number;
                request.prompt = "Enter mline scale";
                request.defaultNumber = scale_;
                request.minimum = 0.0;
                return StepResult::ask(request);
            }
            if (first == 'C' && points_.size() >= 3) {
                closed_ = true;
                return finish(context);
            }
            if (stage_ == Stage::Justification) {
                justification_ = first == 'T' ? 1 : first == 'B' ? -1 : 0;
                stage_ = Stage::Points;
                return askPoint();
            }
            return StepResult::failed("Unknown option.");
        }
        if (input.kind == InputValue::Kind::Number && awaitingScale_) {
            awaitingScale_ = false;
            if (input.number > 0.0) {
                scale_ = input.number;
            }
            return askPoint();
        }
        if (input.kind == InputValue::Kind::Empty) {
            if (stage_ == Stage::Justification) {
                stage_ = Stage::Points;
                return askPoint();
            }
            if (awaitingScale_) {
                awaitingScale_ = false;
                return askPoint();
            }
            return points_.size() >= 2 ? finish(context) : StepResult::cancelled();
        }
        if (input.kind != InputValue::Kind::Point) {
            return StepResult::cancelled();
        }
        if (points_.empty() || !points_.back().equals(input.point, Tolerance{1e-9})) {
            points_.push_back(input.point);
        }
        return askPoint();
    }

private:
    enum class Stage : std::uint8_t { Points, Justification };

    [[nodiscard]] StepResult askPoint() const {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        if (points_.empty()) {
            request.prompt = "Specify start point or [Justification/Scale]";
            request.keywords = {
                CommandKeyword{"Justification", "J", "Where the picked points run: top, centre, bottom"},
                CommandKeyword{"Scale", "S", "How far apart the lines are"},
            };
        } else {
            request.prompt = points_.size() >= 3 ? "Specify next point or [Close], or Enter to finish"
                                                 : "Specify next point, or Enter to finish";
            if (points_.size() >= 3) {
                request.keywords = {CommandKeyword{"Close", "C", "Back to the start"}};
            }
            request.basePoint = points_.back();
            request.allowEmpty = true;
        }
        return StepResult::ask(request);
    }

    StepResult finish(CommandContext& context) {
        auto mline = std::make_unique<MlineEntity>();
        mline->scale = scale_;
        mline->justification = justification_ == 1 ? 0 : justification_ == 0 ? 1 : 2;
        mline->closed = closed_;
        mline->elementCount = 2;
        mline->startPoint = points_.front();
        // Element offsets from the picked line: at the top the lines lie
        // at 0 and -scale, at the centre at +half and -half, at the bottom
        // at +scale and 0.
        const double top = justification_ == 1 ? 0.0 : justification_ == 0 ? scale_ / 2.0 : scale_;
        const double bottom = top - scale_;
        const std::size_t n = points_.size();
        for (std::size_t i = 0; i < n; ++i) {
            // The direction into the next segment; the last vertex of an
            // open multiline keeps the last segment's direction.
            const Vec3 next = (i + 1 < n) ? points_[i + 1] : (closed_ ? points_[0] : points_[i]);
            const Vec3 previous = (i > 0) ? points_[i - 1] : (closed_ ? points_[n - 1] : points_[i]);
            Vec3 outgoing = (i + 1 < n || closed_) ? (next - points_[i]).normalized()
                                                   : (points_[i] - previous).normalized();
            Vec3 incoming = (i > 0 || closed_) ? (points_[i] - previous).normalized() : outgoing;
            if (outgoing.length() < 1e-12) {
                outgoing = incoming;
            }
            if (incoming.length() < 1e-12) {
                incoming = outgoing;
            }
            // The miter: the bisector of the two normals, stretched so the
            // element lines meet where the offset lines cross.
            const Vec3 normalIn{-incoming.y, incoming.x, 0.0};
            const Vec3 normalOut{-outgoing.y, outgoing.x, 0.0};
            Vec3 miter = (normalIn + normalOut);
            double stretch = 1.0;
            if (miter.length() < 1e-9) {
                miter = normalOut;
            } else {
                miter = miter.normalized();
                stretch = 1.0 / std::max(miter.dot(normalOut), 0.1);
            }
            MlineVertex vertex;
            vertex.position = points_[i];
            vertex.direction = outgoing;
            vertex.miterDirection = miter;
            vertex.elementOffsets = {{top * stretch}, {bottom * stretch}};
            vertex.fillOffsets = {{}, {}};
            mline->vertices.push_back(std::move(vertex));
        }
        context.transaction().addToCurrentSpace(std::move(mline));
        return StepResult::done("Multiline drawn, " + std::to_string(closed_ ? n : n - 1) +
                                " segment" + ((closed_ ? n : n - 1) == 1 ? "" : "s"));
    }

    Stage stage_ = Stage::Points;
    std::vector<Vec3> points_;
    double scale_ = 1.0;
    int justification_ = 0;
    bool closed_ = false;
    bool awaitingScale_ = false;
};

class LineCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& /*context*/) override {
        return StepResult::ask(pointRequest(points_));
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Empty || input.kind == InputValue::Kind::Cancel) {
            // Enter and Escape both end the chain, keeping the segments already
            // committed — matching other CAD software, where Escape abandons the
            // rubber-band segment in progress, not the ones already drawn.
            return created_.empty()
                       ? StepResult::cancelled()
                       : StepResult::done(std::to_string(created_.size()) + " line(s) drawn");
        }

        if (input.kind == InputValue::Kind::Keyword) {
            return handleKeyword(input.text, context);
        }
        if (input.kind != InputValue::Kind::Point) {
            return StepResult::ask(pointRequest(points_));
        }

        points_.push_back(input.point);
        if (points_.size() >= 2) {
            const Vec3& from = points_[points_.size() - 2];
            const Vec3& to = points_.back();
            // A zero-length segment is a mis-click, not geometry worth keeping.
            if (!from.equals(to)) {
                Entity& line = context.transaction().addToCurrentSpace(
                    std::make_unique<LineEntity>(from, to));
                created_.push_back(line.handle);
            }
        }
        return StepResult::ask(pointRequest(points_));
    }

    [[nodiscard]] std::vector<std::unique_ptr<Entity>> preview(
        const Vec3& cursor, CommandContext& /*context*/) const override {
        if (points_.empty()) {
            return {};
        }
        std::vector<std::unique_ptr<Entity>> items;
        items.push_back(std::make_unique<LineEntity>(points_.back(), cursor));
        return items;
    }

private:
    StepResult handleKeyword(const std::string& keyword, CommandContext& context) {
        if (matchesKeyword(keyword, 'C')) {
            if (points_.size() < 3) {
                context.warn("At least three points are needed to close.");
                return StepResult::ask(pointRequest(points_));
            }
            const Vec3& last = points_.back();
            const Vec3& first = points_.front();
            if (!last.equals(first)) {
                Entity& line = context.transaction().addToCurrentSpace(
                    std::make_unique<LineEntity>(last, first));
                created_.push_back(line.handle);
            }
            return StepResult::done(std::to_string(created_.size()) + " line(s) drawn");
        }

        if (matchesKeyword(keyword, 'U') && !created_.empty()) {
            if (DbObject* last = context.db().getObject(created_.back())) {
                context.transaction().erase(*last);
            }
            created_.pop_back();
            if (!points_.empty()) {
                points_.pop_back();
            }
        }
        return StepResult::ask(pointRequest(points_));
    }

    std::vector<Vec3> points_;
    std::vector<Handle> created_;
};

/// PLINE — draw a polyline as a single entity.
///
/// Distinct from LINE because the result is one object: it can be offset as a
/// unit, carries width, and reports an enclosed area.
const CommandKeyword kArcCenterKeyword{"CEnter", "CE", "Arc by its centre, then its end"};
const CommandKeyword kArcRadiusKeyword{"Radius", "R", "Arc of a given radius to the next point"};
const CommandKeyword kArcSecondKeyword{"Second", "S", "Arc through a second point to the next"};
const CommandKeyword kArcDirectionKeyword{"Direction", "D", "Arc leaving in a given direction"};
const CommandKeyword kArcAngleKeyword{"Angle", "AN", "Arc of a given included angle"};
const CommandKeyword kWidthKeyword{"Width", "W", "Width of the segments that follow"};
const CommandKeyword kLengthKeyword{"Length", "LE", "A straight segment of a given length, straight on"};

/// PLINE: a chain of straight and arc segments as one object.
///
/// Arc segments are tangent to what came before unless told otherwise: by
/// centre, radius, a second point, a start direction or an included angle,
/// each of which asks for its own datum before the segment's end. Width
/// applies to the segments that follow it. Length continues straight on.
class PolylineCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& /*context*/) override { return StepResult::ask(prompt()); }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return finish(false, context);
        }
        // A datum the arc or width option asked for, before the segment.
        if (pending_ != Pending::None) {
            return supplyDatum(input, context);
        }
        if (input.kind == InputValue::Kind::Empty) {
            return finish(false, context);
        }

        if (input.kind == InputValue::Kind::Keyword) {
            const std::string typed = upper(input.text);
            if (typed.rfind("CL", 0) == 0 || (typed == "C" && !arcMode_)) {
                return finish(true, context);
            }
            if (typed == "A" || typed == "ARC") {
                arcMode_ = true;
            } else if (typed == "L" || typed == "LINE") {
                arcMode_ = false;
            } else if (typed.rfind("LE", 0) == 0 && !vertices_.empty()) {
                return ask(Pending::Length, "Specify length of line");
            } else if (typed == "U" || typed == "UNDO") {
                undo(context);
            } else if (typed == "W" || typed == "WIDTH" || typed == "H" || typed == "HALFWIDTH") {
                halfWidth_ = typed.front() == 'H';
                return ask(Pending::WidthStart,
                           halfWidth_ ? "Specify starting half-width" : "Specify starting width");
            } else if (arcMode_ && (typed == "CE" || typed == "CENTER" || typed == "C")) {
                return ask(Pending::Center, "Specify center point of arc");
            } else if (arcMode_ && (typed == "R" || typed == "RADIUS")) {
                return ask(Pending::Radius, "Specify radius of arc");
            } else if (arcMode_ && (typed == "S" || typed == "SECOND")) {
                return ask(Pending::Second, "Specify second point on arc");
            } else if (arcMode_ && (typed == "D" || typed == "DIRECTION")) {
                return ask(Pending::Direction, "Specify the tangent direction for the start of arc");
            } else if (arcMode_ && (typed == "AN" || typed == "ANGLE")) {
                return ask(Pending::Angle, "Specify included angle");
            }
            return StepResult::ask(prompt());
        }
        if (input.kind != InputValue::Kind::Point) {
            return StepResult::ask(prompt());
        }

        append(input.point, pendingBulge(input.point));
        sync(context);
        return StepResult::ask(prompt());
    }

    [[nodiscard]] std::vector<std::unique_ptr<Entity>> preview(
        const Vec3& cursor, CommandContext& /*context*/) const override {
        if (vertices_.empty()) {
            return {};
        }
        std::vector<std::unique_ptr<Entity>> items;
        const bool arcEnding = pending_ == Pending::None ? arcMode_ : endsAnArc();
        if (!arcEnding) {
            if (pending_ == Pending::None || pending_ == Pending::Length) {
                items.push_back(std::make_unique<LineEntity>(vertices_.back().position, cursor));
            }
            return items;
        }
        // The arc itself, not the chord across it: in arc mode the shape of
        // what is about to be drawn is the whole question.
        auto pending = std::make_unique<PolylineEntity>();
        PolylineVertex from = vertices_.back();
        from.bulge = pendingBulge(cursor);
        pending->vertices.push_back(from);
        pending->vertices.emplace_back(cursor);
        items.push_back(std::move(pending));
        return items;
    }

private:
    /// What the next input is: a datum for an option, or the segment's end.
    enum class Pending {
        None,
        Center, CenterEnd,
        Radius, RadiusEnd,
        Second, SecondEnd,
        Direction, DirectionEnd,
        Angle, AngleEnd,
        WidthStart, WidthEnd,
        Length,
    };

    [[nodiscard]] static std::string upper(std::string text) {
        for (char& c : text) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return text;
    }

    [[nodiscard]] bool endsAnArc() const {
        return pending_ == Pending::CenterEnd || pending_ == Pending::RadiusEnd ||
               pending_ == Pending::SecondEnd || pending_ == Pending::DirectionEnd ||
               pending_ == Pending::AngleEnd;
    }

    /// The prompt, and the options that make sense at this point in the chain.
    [[nodiscard]] InputRequest prompt() const {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = vertices_.empty() ? "Specify first point"
                         : arcMode_        ? "Specify endpoint of arc"
                                           : "Specify next point";
        if (!vertices_.empty()) {
            request.basePoint = vertices_.back().position;
            if (arcMode_) {
                request.keywords = {kArcAngleKeyword, kArcCenterKeyword, kArcDirectionKeyword,
                                    kLineKeyword, kArcRadiusKeyword, kArcSecondKeyword,
                                    kUndoKeyword, kWidthKeyword};
            } else {
                request.keywords = {kArcKeyword, kLengthKeyword, kUndoKeyword, kWidthKeyword};
            }
            request.allowEmpty = true;
        } else {
            request.keywords = {kWidthKeyword};
        }
        if (vertices_.size() >= 3) {
            request.keywords.insert(request.keywords.begin(), kCloseKeyword);
        }
        return request;
    }

    StepResult ask(Pending what, std::string promptText) {
        pending_ = what;
        InputRequest request;
        switch (what) {
            case Pending::Radius:
            case Pending::Length:
            case Pending::WidthStart:
            case Pending::WidthEnd:
                request.kind = InputRequest::Kind::Distance;
                request.basePoint = vertices_.empty() ? std::optional<Vec3>{} : vertices_.back().position;
                if (what == Pending::WidthStart) {
                    request.defaultNumber = halfWidth_ ? width_ / 2.0 : width_;
                } else if (what == Pending::WidthEnd) {
                    request.defaultNumber = halfWidth_ ? startWidth_ / 2.0 : startWidth_;
                }
                break;
            case Pending::Angle:
                request.kind = InputRequest::Kind::Angle;
                break;
            default:
                request.kind = InputRequest::Kind::Point;
                request.basePoint = vertices_.empty() ? std::optional<Vec3>{} : vertices_.back().position;
                break;
        }
        request.prompt = std::move(promptText);
        return StepResult::ask(request);
    }

    StepResult supplyDatum(const InputValue& input, CommandContext& context) {
        const bool number = input.kind == InputValue::Kind::Number;
        const bool point = input.kind == InputValue::Kind::Point;
        switch (pending_) {
            case Pending::Center:
                if (!point) return cancelOption();
                center_ = input.point;
                return ask(Pending::CenterEnd, "Specify endpoint of arc");
            case Pending::Radius:
                if (!number || input.number <= 0.0) return cancelOption();
                radius_ = input.number;
                return ask(Pending::RadiusEnd, "Specify endpoint of arc");
            case Pending::Second:
                if (!point) return cancelOption();
                second_ = input.point;
                return ask(Pending::SecondEnd, "Specify endpoint of arc");
            case Pending::Direction:
                if (!point) return cancelOption();
                direction_ = input.point - vertices_.back().position;
                direction_.z = 0.0;
                if (direction_.lengthSquared() < 1e-18) return cancelOption();
                return ask(Pending::DirectionEnd, "Specify endpoint of arc");
            case Pending::Angle:
                if (!number || std::abs(input.number) < 1e-9) return cancelOption();
                angle_ = input.number;
                return ask(Pending::AngleEnd, "Specify endpoint of arc");
            case Pending::WidthStart:
                if (input.kind == InputValue::Kind::Empty) {
                    startWidth_ = width_;
                } else if (!number || input.number < 0.0) {
                    return cancelOption();
                } else {
                    startWidth_ = halfWidth_ ? 2.0 * input.number : input.number;
                }
                return ask(Pending::WidthEnd, halfWidth_ ? "Specify ending half-width" : "Specify ending width");
            case Pending::WidthEnd: {
                double endWidth = startWidth_;
                if (number && input.number >= 0.0) {
                    endWidth = halfWidth_ ? 2.0 * input.number : input.number;
                } else if (input.kind != InputValue::Kind::Empty) {
                    return cancelOption();
                }
                width_ = endWidth;
                nextStartWidth_ = startWidth_;
                pending_ = Pending::None;
                return StepResult::ask(prompt());
            }
            case Pending::Length: {
                if (!number || input.number <= 0.0) return cancelOption();
                const Vec3 tangent = vertices_.size() >= 2
                                         ? segmentEndTangent(vertices_[vertices_.size() - 2],
                                                             vertices_.back().position)
                                         : Vec3{1.0, 0.0, 0.0};
                pending_ = Pending::None;
                append(vertices_.back().position + tangent * input.number, 0.0);
                sync(context);
                return StepResult::ask(prompt());
            }
            case Pending::CenterEnd:
            case Pending::RadiusEnd:
            case Pending::SecondEnd:
            case Pending::DirectionEnd:
            case Pending::AngleEnd: {
                if (!point) return cancelOption();
                const double bulge = pendingBulge(input.point);
                pending_ = Pending::None;
                append(input.point, bulge);
                sync(context);
                return StepResult::ask(prompt());
            }
            case Pending::None:
                break;
        }
        pending_ = Pending::None;
        return StepResult::ask(prompt());
    }

    StepResult cancelOption() {
        pending_ = Pending::None;
        return StepResult::ask(prompt());
    }

    /// The bulge a segment ending at `to` would have, given the option in
    /// force: tangent to what came before unless a datum says otherwise.
    [[nodiscard]] double pendingBulge(const Vec3& to) const {
        if (vertices_.empty()) {
            return 0.0;
        }
        const Vec3 from = vertices_.back().position;
        Vec3 chord = to - from;
        chord.z = 0.0;
        if (chord.lengthSquared() < 1e-18) {
            return 0.0;
        }
        switch (pending_) {
            case Pending::CenterEnd: {
                // The sweep from start to end about the centre, counter-clockwise
                // as other CAD software takes it; the end is used for its direction.
                const double start = std::atan2(from.y - center_.y, from.x - center_.x);
                const double end = std::atan2(to.y - center_.y, to.x - center_.x);
                double sweep = end - start;
                while (sweep <= 0.0) sweep += kTau;
                while (sweep > kTau) sweep -= kTau;
                return std::tan(sweep / 4.0);
            }
            case Pending::RadiusEnd: {
                // The minor arc of that radius, counter-clockwise. A radius
                // shorter than half the chord cannot reach: the chord's half
                // is used, which is the semicircle.
                const double half = chord.length2() / 2.0;
                const double radius = std::max(radius_, half);
                const double sweep = 2.0 * std::asin(std::min(1.0, half / radius));
                return std::tan(sweep / 4.0);
            }
            case Pending::SecondEnd: {
                const std::optional<Arc> arc = Arc::fromThreePoints(from, second_, to);
                if (!arc) {
                    return 0.0;
                }
                // Through the second point, in the direction the three points turn.
                const double turn = (second_.x - from.x) * (to.y - from.y) - (second_.y - from.y) * (to.x - from.x);
                return turn >= 0.0 ? arc->bulge() : -arc->bulge();
            }
            case Pending::DirectionEnd:
                return bulgeFromTangent(direction_.normalized(), chord);
            case Pending::AngleEnd:
                return std::tan(angle_ / 4.0);
            default:
                break;
        }
        if (!arcMode_) {
            return 0.0;
        }
        // The first segment has nothing to be tangent to. East, which is what
        // other CAD software assumes when there is no previous direction either.
        const Vec3 tangent = vertices_.size() >= 2
                                 ? segmentEndTangent(vertices_[vertices_.size() - 2], from)
                                 : Vec3{1.0, 0.0, 0.0};
        return bulgeFromTangent(tangent, chord);
    }

    void append(const Vec3& point, double bulge) {
        if (!vertices_.empty()) {
            vertices_.back().bulge = bulge;
            // Width belongs to the segment that starts at the vertex behind.
            vertices_.back().startWidth = nextStartWidth_;
            vertices_.back().endWidth = width_;
            nextStartWidth_ = width_;
        }
        vertices_.emplace_back(point);
    }

    void undo(CommandContext& context) {
        if (vertices_.empty()) {
            return;
        }
        vertices_.pop_back();
        // The bulge belongs to the segment that started at the vertex now on
        // the end, and that segment has just been taken away.
        if (!vertices_.empty()) {
            vertices_.back().bulge = 0.0;
        }
        sync(context);
    }

    /// Create or update the entity as points accumulate, so the drawing always
    /// reflects what the user has committed. A cancel rolls the whole thing
    /// back through the transaction, not through bookkeeping here.
    void sync(CommandContext& context) {
        if (vertices_.size() < 2) {
            return;
        }
        if (handle_.isNull()) {
            auto entity = std::make_unique<PolylineEntity>();
            entity->vertices = vertices_;
            handle_ = context.transaction().addToCurrentSpace(std::move(entity)).handle;
            return;
        }
        if (auto* entity = context.db().get<PolylineEntity>(handle_)) {
            context.transaction().modifyEntity(
                *entity, [this](PolylineEntity& e) { e.vertices = vertices_; });
        }
    }

    StepResult finish(bool close, CommandContext& context) {
        if (handle_.isNull() || vertices_.size() < 2) {
            return StepResult::cancelled();
        }
        if (close) {
            if (auto* entity = context.db().get<PolylineEntity>(handle_)) {
                context.transaction().modifyEntity(*entity,
                                                   [](PolylineEntity& e) { e.closed = true; });
            }
        }
        return StepResult::done("Polyline with " + std::to_string(vertices_.size()) +
                                " vertices");
    }

    std::vector<PolylineVertex> vertices_;
    bool arcMode_ = false;
    Pending pending_ = Pending::None;
    Vec3 center_;
    Vec3 second_;
    Vec3 direction_;
    double radius_ = 0.0;
    double angle_ = 0.0;
    double width_ = 0.0;
    double startWidth_ = 0.0;
    double nextStartWidth_ = 0.0;
    bool halfWidth_ = false;
    Handle handle_;
};

/// SPLINE: a smooth curve through the points picked.
///
/// Built as a chain of cubic Bezier spans, one per pair of fit points, with
/// the tangent at each interior point taken from its neighbours (Catmull-Rom)
/// and the ends left to run straight on. That passes exactly through every
/// point, is smooth across each one, and needs no equation solving. Stored
/// as the NURBS it is -- degree 3, each interior knot tripled -- by control
/// vertices, so every reader draws exactly this curve.
class SplineCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& /*context*/) override { return StepResult::ask(prompt()); }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return finish(context);
        }
        if (input.kind == InputValue::Kind::Empty) {
            return finish(context);
        }
        if (input.kind == InputValue::Kind::Keyword) {
            if (matchesKeyword(input.text, 'U') && !points_.empty()) {
                points_.pop_back();
                sync(context);
            } else if (matchesKeyword(input.text, 'C') && points_.size() >= 3) {
                closed_ = true;
                return finish(context);
            }
            return StepResult::ask(prompt());
        }
        if (input.kind != InputValue::Kind::Point) {
            return StepResult::ask(prompt());
        }
        if (!points_.empty() && input.point.equals(points_.back())) {
            return StepResult::ask(prompt());
        }
        points_.push_back(input.point);
        sync(context);
        return StepResult::ask(prompt());
    }

    [[nodiscard]] std::vector<std::unique_ptr<Entity>> preview(
        const Vec3& cursor, CommandContext& /*context*/) const override {
        if (points_.empty()) {
            return {};
        }
        std::vector<Vec3> through = points_;
        through.push_back(cursor);
        std::vector<std::unique_ptr<Entity>> items;
        if (auto spline = build(through, false)) {
            items.push_back(std::move(spline));
        }
        return items;
    }

    /// The spline through `fit`, or nothing with fewer than two points.
    [[nodiscard]] static std::unique_ptr<SplineEntity> build(std::vector<Vec3> fit, bool closed) {
        if (closed && fit.size() >= 3) {
            fit.push_back(fit.front());
        }
        const std::size_t n = fit.size();
        if (n < 2) {
            return nullptr;
        }
        // Tangent at each fit point: the chord between its neighbours, halved.
        std::vector<Vec3> tangents(n);
        for (std::size_t i = 0; i < n; ++i) {
            const Vec3& before = i == 0 ? (closed ? fit[n - 2] : fit[0]) : fit[i - 1];
            const Vec3& after = i + 1 == n ? (closed ? fit[1] : fit[n - 1]) : fit[i + 1];
            tangents[i] = (after - before) * 0.5;
        }
        std::vector<Vec3> control;
        std::vector<double> knots{0.0, 0.0, 0.0, 0.0};
        control.push_back(fit[0]);
        for (std::size_t i = 0; i + 1 < n; ++i) {
            control.push_back(fit[i] + tangents[i] * (1.0 / 3.0));
            control.push_back(fit[i + 1] - tangents[i + 1] * (1.0 / 3.0));
            control.push_back(fit[i + 1]);
            const double knot = static_cast<double>(i + 1);
            knots.push_back(knot);
            knots.push_back(knot);
            knots.push_back(knot);
        }
        knots.push_back(static_cast<double>(n - 1));
        auto spline = std::make_unique<SplineEntity>();
        spline->curve = NurbsCurve{std::move(control), std::move(knots), 3};
        // By control vertices, with no fit points: given fit points, other CAD software
        // re-fits the curve by its own interpolation and draws a different
        // shape from these control points. Without them it draws exactly this.
        spline->fitPoints.clear();
        spline->flags = 8 | (closed ? 1 : 0);
        return spline;
    }

private:
    [[nodiscard]] InputRequest prompt() const {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = points_.empty() ? "Specify first point" : "Specify next point";
        if (!points_.empty()) {
            request.basePoint = points_.back();
            request.keywords.push_back(kUndoKeyword);
            request.allowEmpty = true;
        }
        if (points_.size() >= 3) {
            request.keywords.insert(request.keywords.begin(), kCloseKeyword);
        }
        return request;
    }

    void sync(CommandContext& context) {
        if (points_.size() < 2) {
            return;
        }
        auto spline = build(points_, false);
        if (spline == nullptr) {
            return;
        }
        if (handle_.isNull()) {
            handle_ = context.transaction().addToCurrentSpace(std::move(spline)).handle;
            return;
        }
        if (auto* existing = context.db().get<SplineEntity>(handle_)) {
            context.transaction().modifyEntity(*existing, [&](SplineEntity& e) {
                e.curve = spline->curve;
                e.fitPoints = spline->fitPoints;
                e.flags = spline->flags;
            });
        }
    }

    StepResult finish(CommandContext& context) {
        if (points_.size() < 2) {
            return StepResult::cancelled();
        }
        if (closed_) {
            if (auto* existing = context.db().get<SplineEntity>(handle_)) {
                auto spline = build(points_, true);
                context.transaction().modifyEntity(*existing, [&](SplineEntity& e) {
                    e.curve = spline->curve;
                    e.fitPoints = spline->fitPoints;
                    e.flags = spline->flags;
                });
            }
        }
        return StepResult::done("Spline through " + std::to_string(points_.size()) + " points");
    }

    std::vector<Vec3> points_;
    bool closed_ = false;
    Handle handle_;
};

/// ELLIPSE — two ends of one axis, then half the other.
///
/// other CAD software's default form, and the one worth having first: an ellipse is almost
/// always drawn to fit between two known points. `Center` switches to the other
/// form, where the middle is known instead.
///
/// The second axis is given as a distance rather than a point, so it is half an
/// axis and not a whole one. That asymmetry is other CAD software's, and it is what lets
/// the third pick be anywhere off the axis line rather than exactly on the
/// perpendicular.
class EllipseCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& /*context*/) override {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = "Specify axis endpoint, or an option";
        request.keywords = {kCenterKeyword};
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel || input.kind == InputValue::Kind::Empty) {
            return StepResult::cancelled();
        }
        if (input.kind == InputValue::Kind::Keyword) {
            if (matchesKeyword(input.text, 'C')) {
                fromCenter_ = true;
                return askPoint("Specify center point");
            }
            return start(context);
        }

        if (!hasFirst_) {
            if (input.kind != InputValue::Kind::Point) {
                return askPoint(fromCenter_ ? "Specify center point"
                                            : "Specify axis endpoint");
            }
            first_ = input.point;
            hasFirst_ = true;
            return askPoint(fromCenter_ ? "Specify endpoint of axis"
                                        : "Specify other endpoint of axis");
        }

        if (!hasSecond_) {
            if (input.kind != InputValue::Kind::Point) {
                return askPoint("Specify endpoint of axis");
            }
            second_ = input.point;
            hasSecond_ = true;

            InputRequest request;
            request.kind = InputRequest::Kind::Distance;
            request.prompt = "Specify distance to other axis";
            request.basePoint = centerPoint();
            return StepResult::ask(request);
        }

        const double half = input.kind == InputValue::Kind::Number
                                ? input.number
                                : (input.kind == InputValue::Kind::Point
                                       ? perpendicularDistance(input.point)
                                       : 0.0);
        std::unique_ptr<Entity> shape = build(half);
        if (shape == nullptr) {
            return StepResult::failed("An ellipse needs two axes with a length.");
        }
        context.transaction().addToCurrentSpace(std::move(shape));
        return StepResult::done();
    }

    [[nodiscard]] std::vector<std::unique_ptr<Entity>> preview(
        const Vec3& cursor, CommandContext& /*context*/) const override {
        std::vector<std::unique_ptr<Entity>> items;
        if (!hasFirst_) {
            return items;
        }
        if (!hasSecond_) {
            // The axis itself, so its length and direction are visible before
            // the shape exists. A preview of nothing at all here would leave
            // the most consequential pick of the three unguided.
            items.push_back(std::make_unique<LineEntity>(first_, cursor));
            return items;
        }
        if (std::unique_ptr<Entity> shape = build(perpendicularDistance(cursor))) {
            items.push_back(std::move(shape));
        }
        return items;
    }

private:
    [[nodiscard]] static StepResult askPoint(std::string prompt) {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = std::move(prompt);
        return StepResult::ask(request);
    }

    /// The middle, whichever way the axis was given.
    [[nodiscard]] Vec3 centerPoint() const {
        return fromCenter_ ? first_ : first_.midpoint(second_);
    }

    /// From the centre to the far end of the major axis.
    [[nodiscard]] Vec3 majorAxis() const { return second_ - centerPoint(); }

    /// How far a point lies off the axis, which is the half-length of the
    /// other one. Measured perpendicular rather than as a plain distance, so
    /// sliding along the axis does not change the shape.
    [[nodiscard]] double perpendicularDistance(const Vec3& point) const {
        const Vec3 axis = majorAxis();
        const double length = axis.length2();
        if (length < 1e-12) {
            return 0.0;
        }
        const Vec3 offset = point - centerPoint();
        return std::abs(axis.x * offset.y - axis.y * offset.x) / length;
    }

    [[nodiscard]] std::unique_ptr<Entity> build(double halfOther) const {
        const Vec3 axis = majorAxis();
        const double major = axis.length2();
        if (major < 1e-9 || halfOther < 1e-9) {
            return nullptr;
        }
        auto ellipse = std::make_unique<EllipseEntity>();
        ellipse->center = centerPoint();
        // DXF stores the ratio of minor to major, and requires the stored axis
        // to be the *major* one -- a ratio above 1 is not representable. When
        // the second axis is the longer, the two swap and the stored axis turns
        // a quarter turn.
        if (halfOther > major) {
            ellipse->majorAxis = Vec3{-axis.y, axis.x, 0.0} * (halfOther / major);
            ellipse->ratio = major / halfOther;
        } else {
            ellipse->majorAxis = axis;
            ellipse->ratio = halfOther / major;
        }
        return ellipse;
    }

    bool fromCenter_ = false;
    bool hasFirst_ = false;
    bool hasSecond_ = false;
    Vec3 first_;
    Vec3 second_;
};

/// CIRCLE — centre and radius, plus the alternative constructions other CAD software
/// offers.
class CircleCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& /*context*/) override {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = "Specify center point";
        request.keywords = {{"2Point", "2P", "Two points on the diameter"},
                            {"3Point", "3P", "Three points on the circumference"}};
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel || input.kind == InputValue::Kind::Empty) {
            return StepResult::cancelled();
        }

        if (input.kind == InputValue::Kind::Keyword) {
            if (!input.text.empty() && input.text.front() == '2') {
                mode_ = Mode::TwoPoint;
                return askPoint("Specify first end of diameter");
            }
            if (!input.text.empty() && input.text.front() == '3') {
                mode_ = Mode::ThreePoint;
                return askPoint("Specify first point on circle");
            }
            return askPoint("Specify center point");
        }

        switch (mode_) {
            case Mode::CenterRadius: return supplyCenterRadius(input, context);
            case Mode::TwoPoint:     return supplyTwoPoint(input, context);
            case Mode::ThreePoint:   return supplyThreePoint(input, context);
        }
        return StepResult::cancelled();
    }

    [[nodiscard]] std::vector<std::unique_ptr<Entity>> preview(
        const Vec3& cursor, CommandContext& /*context*/) const override {
        std::vector<std::unique_ptr<Entity>> items;

        if (mode_ == Mode::CenterRadius && hasCenter_) {
            const double radius = center_.distanceTo(cursor);
            if (radius > 0.0) {
                items.push_back(std::make_unique<CircleEntity>(center_, radius));
            }
        } else if (mode_ == Mode::TwoPoint && hasFirst_) {
            const double radius = first_.distanceTo(cursor) / 2.0;
            if (radius > 0.0) {
                items.push_back(
                    std::make_unique<CircleEntity>(first_.midpoint(cursor), radius));
            }
        } else if (mode_ == Mode::ThreePoint && hasFirst_ && hasSecond_) {
            if (const auto arc = Arc::fromThreePoints(first_, second_, cursor)) {
                items.push_back(std::make_unique<CircleEntity>(arc->center, arc->radius));
            }
        }
        return items;
    }

private:
    enum class Mode : std::uint8_t { CenterRadius, TwoPoint, ThreePoint };

    [[nodiscard]] static StepResult askPoint(std::string prompt) {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = std::move(prompt);
        return StepResult::ask(request);
    }

    StepResult supplyCenterRadius(const InputValue& input, CommandContext& context) {
        if (!hasCenter_) {
            if (input.kind != InputValue::Kind::Point) {
                return askPoint("Specify center point");
            }
            center_ = input.point;
            hasCenter_ = true;

            InputRequest request;
            request.kind = InputRequest::Kind::Distance;
            request.prompt = "Specify radius";
            request.basePoint = center_;
            return StepResult::ask(request);
        }

        const double radius = input.kind == InputValue::Kind::Number
                                  ? input.number
                                  : (input.kind == InputValue::Kind::Point
                                         ? center_.distanceTo(input.point)
                                         : 0.0);
        if (!(radius > 0.0)) {
            return StepResult::failed("Radius must be greater than zero.");
        }
        context.transaction().addToCurrentSpace(std::make_unique<CircleEntity>(center_, radius));
        return StepResult::done();
    }

    StepResult supplyTwoPoint(const InputValue& input, CommandContext& context) {
        if (input.kind != InputValue::Kind::Point) {
            return StepResult::cancelled();
        }
        if (!hasFirst_) {
            first_ = input.point;
            hasFirst_ = true;
            InputRequest request;
            request.kind = InputRequest::Kind::Point;
            request.prompt = "Specify second end of diameter";
            request.basePoint = first_;
            return StepResult::ask(request);
        }
        const double radius = first_.distanceTo(input.point) / 2.0;
        if (!(radius > 0.0)) {
            return StepResult::failed("The two points are coincident.");
        }
        context.transaction().addToCurrentSpace(
            std::make_unique<CircleEntity>(first_.midpoint(input.point), radius));
        return StepResult::done();
    }

    StepResult supplyThreePoint(const InputValue& input, CommandContext& context) {
        if (input.kind != InputValue::Kind::Point) {
            return StepResult::cancelled();
        }
        if (!hasFirst_) {
            first_ = input.point;
            hasFirst_ = true;
            return askPoint("Specify second point on circle");
        }
        if (!hasSecond_) {
            second_ = input.point;
            hasSecond_ = true;
            return askPoint("Specify third point on circle");
        }
        const auto arc = Arc::fromThreePoints(first_, second_, input.point);
        if (!arc) {
            return StepResult::failed(
                "The three points are collinear; no circle passes through them.");
        }
        context.transaction().addToCurrentSpace(
            std::make_unique<CircleEntity>(arc->center, arc->radius));
        return StepResult::done();
    }

    Mode mode_ = Mode::CenterRadius;
    Vec3 center_;
    Vec3 first_;
    Vec3 second_;
    bool hasCenter_ = false;
    bool hasFirst_ = false;
    bool hasSecond_ = false;
};

/// ARC — three points on the arc. The most predictable of other CAD software's many arc
/// constructions, and the one needing no direction convention explained.
class ArcCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& /*context*/) override {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = "Specify start point of arc";
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind != InputValue::Kind::Point) {
            return StepResult::cancelled();
        }

        if (!hasFirst_) {
            first_ = input.point;
            hasFirst_ = true;
            InputRequest request;
            request.kind = InputRequest::Kind::Point;
            request.prompt = "Specify second point on arc";
            request.basePoint = first_;
            return StepResult::ask(request);
        }
        if (!hasSecond_) {
            second_ = input.point;
            hasSecond_ = true;
            InputRequest request;
            request.kind = InputRequest::Kind::Point;
            request.prompt = "Specify end point of arc";
            return StepResult::ask(request);
        }

        const auto arc = Arc::fromThreePoints(first_, second_, input.point);
        if (!arc) {
            return StepResult::failed(
                "The three points are collinear; no arc passes through them.");
        }
        context.transaction().addToCurrentSpace(
            std::make_unique<ArcEntity>(ArcEntity::fromArc(*arc)));
        return StepResult::done();
    }

    [[nodiscard]] std::vector<std::unique_ptr<Entity>> preview(
        const Vec3& cursor, CommandContext& /*context*/) const override {
        std::vector<std::unique_ptr<Entity>> items;
        if (hasFirst_ && hasSecond_) {
            if (const auto arc = Arc::fromThreePoints(first_, second_, cursor)) {
                items.push_back(std::make_unique<ArcEntity>(ArcEntity::fromArc(*arc)));
            }
        } else if (hasFirst_) {
            // Before the third point there is no arc yet, so the chord stands
            // in for it.
            items.push_back(std::make_unique<LineEntity>(first_, cursor));
        }
        return items;
    }

private:
    Vec3 first_;
    Vec3 second_;
    bool hasFirst_ = false;
    bool hasSecond_ = false;
};

/// RECTANG — a closed rectangular polyline from two opposite corners.
class RectangleCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& /*context*/) override {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = "Specify first corner";
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind != InputValue::Kind::Point) {
            return StepResult::cancelled();
        }
        if (!hasFirst_) {
            first_ = input.point;
            hasFirst_ = true;
            InputRequest request;
            request.kind = InputRequest::Kind::Point;
            request.prompt = "Specify opposite corner";
            request.basePoint = first_;
            return StepResult::ask(request);
        }
        if (first_.equals2(input.point)) {
            return StepResult::failed("The corners are coincident; the rectangle would be empty.");
        }
        context.transaction().addToCurrentSpace(std::make_unique<PolylineEntity>(
            PolylineEntity::fromPolyline(Polyline::rectangle(first_, input.point))));
        return StepResult::done();
    }

    [[nodiscard]] std::vector<std::unique_ptr<Entity>> preview(
        const Vec3& cursor, CommandContext& /*context*/) const override {
        std::vector<std::unique_ptr<Entity>> items;
        if (hasFirst_ && !first_.equals2(cursor)) {
            items.push_back(std::make_unique<PolylineEntity>(
                PolylineEntity::fromPolyline(Polyline::rectangle(first_, cursor))));
        }
        return items;
    }

private:
    Vec3 first_;
    bool hasFirst_ = false;
};

/// POINT — place a node, repeating until cancelled.
class PointCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& /*context*/) override { return ask(); }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind != InputValue::Kind::Point) {
            // Escape or Enter keeps the points already placed.
            return count_ > 0
                       ? StepResult::done(std::to_string(count_) + " point(s) placed")
                       : StepResult::cancelled();
        }
        context.transaction().addToCurrentSpace(std::make_unique<PointEntity>(input.point));
        ++count_;
        return ask();
    }

private:
    [[nodiscard]] StepResult ask() const {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = "Specify a point";
        request.allowEmpty = count_ > 0;
        return StepResult::ask(request);
    }

    int count_ = 0;
};

/// POLYGON — an inscribed or circumscribed regular polygon.
class PolygonCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& /*context*/) override {
        InputRequest request;
        request.kind = InputRequest::Kind::Number;
        request.prompt = "Enter number of sides";
        request.defaultNumber = 6.0;
        request.minimum = 3.0;
        request.maximum = 1024.0;
        request.integerOnly = true;
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }

        if (!hasCenter_) {
            if (input.kind == InputValue::Kind::Number) {
                sides_ = static_cast<int>(std::lround(input.number));
                if (sides_ < 3) {
                    return StepResult::failed("A polygon needs at least three sides.");
                }
            }
            if (input.kind == InputValue::Kind::Point) {
                center_ = input.point;
                hasCenter_ = true;
                InputRequest request;
                request.kind = InputRequest::Kind::Keyword;
                request.prompt = "Inscribed in circle or circumscribed about circle?";
                request.keywords = {{"Inscribed", "I", {}}, {"Circumscribed", "C", {}}};
                request.defaultText = "Inscribed";
                return StepResult::ask(request);
            }
            InputRequest request;
            request.kind = InputRequest::Kind::Point;
            request.prompt = "Specify center of polygon";
            return StepResult::ask(request);
        }

        if (input.kind == InputValue::Kind::Keyword || input.kind == InputValue::Kind::Empty) {
            if (input.kind == InputValue::Kind::Keyword) {
                inscribed_ = matchesKeyword(input.text, 'I');
            }
            InputRequest request;
            request.kind = InputRequest::Kind::Distance;
            request.prompt = "Specify radius of circle";
            request.basePoint = center_;
            return StepResult::ask(request);
        }

        const double radius = input.kind == InputValue::Kind::Number
                                  ? input.number
                                  : (input.kind == InputValue::Kind::Point
                                         ? center_.distanceTo(input.point)
                                         : 0.0);
        if (!(radius > 0.0)) {
            return StepResult::failed("Radius must be greater than zero.");
        }

        context.transaction().addToCurrentSpace(
            std::make_unique<PolylineEntity>(PolylineEntity::fromPolyline(
                regularPolygon(center_, radius, sides_, inscribed_))));
        return StepResult::done();
    }

private:
    int sides_ = 6;
    Vec3 center_;
    bool hasCenter_ = false;
    bool inscribed_ = true;
};

}  // namespace

Polyline regularPolygon(const Vec3& center, double radius, int sides, bool inscribed) {
    const double effectiveRadius =
        inscribed ? radius : radius / std::cos(kPi / static_cast<double>(sides));

    std::vector<Vec3> points;
    points.reserve(static_cast<std::size_t>(sides));
    for (int i = 0; i < sides; ++i) {
        const double angle =
            (static_cast<double>(i) * kTau / static_cast<double>(sides)) + kPi / 2.0;
        points.push_back(center + Vec3::fromAngle(angle, effectiveRadius));
    }
    return Polyline::fromPoints(points, true);
}

std::vector<CommandDefinition> drawCommands() {
    std::vector<CommandDefinition> commands;

    {
        CommandDefinition spline;
        spline.id = "cad.draw.spline";
        spline.name = "SPLINE";
        spline.aliases = {"SPL"};
        spline.title = "Spline";
        spline.description = "A smooth curve through the points picked";
        spline.category = "Draw";
        spline.create = [] { return std::make_unique<SplineCommand>(); };
        commands.push_back(std::move(spline));
    }
    {
        CommandDefinition line;
        line.id = "cad.draw.line";
        line.name = "LINE";
        line.aliases = {"L"};
        line.title = "Line";
        line.description = "Draw a series of straight line segments";
        line.category = "Draw";
        line.create = [] { return std::make_unique<LineCommand>(); };
        commands.push_back(std::move(line));
    }
    {
        CommandDefinition xline;
        xline.id = "cad.draw.xline";
        xline.name = "XLINE";
        xline.aliases = {"XL"};
        xline.title = "Construction Line";
        xline.description = "Draw infinite construction lines through points";
        xline.category = "Draw";
        xline.supportsPickfirst = false;
        xline.create = [] { return std::make_unique<ConstructionLineCommand>(true); };
        commands.push_back(std::move(xline));
    }
    {
        CommandDefinition ray;
        ray.id = "cad.draw.ray";
        ray.name = "RAY";
        ray.title = "Ray";
        ray.description = "Draw construction lines from a point in one direction";
        ray.category = "Draw";
        ray.supportsPickfirst = false;
        ray.create = [] { return std::make_unique<ConstructionLineCommand>(false); };
        commands.push_back(std::move(ray));
    }
    {
        CommandDefinition mline;
        mline.id = "cad.draw.mline";
        mline.name = "MLINE";
        mline.aliases = {"ML"};
        mline.title = "Multiline";
        mline.description = "Draw two parallel lines as one, for walls";
        mline.category = "Draw";
        mline.supportsPickfirst = false;
        mline.create = [] { return std::make_unique<MlineCommand>(); };
        commands.push_back(std::move(mline));
    }
    {
        CommandDefinition pline;
        pline.id = "cad.draw.polyline";
        pline.name = "PLINE";
        pline.aliases = {"PL", "POLYLINE"};
        pline.title = "Polyline";
        pline.description = "Draw a connected sequence of segments as a single object";
        pline.category = "Draw";
        pline.create = [] { return std::make_unique<PolylineCommand>(); };
        commands.push_back(std::move(pline));
    }
    {
        CommandDefinition circle;
        circle.id = "cad.draw.circle";
        circle.name = "CIRCLE";
        circle.aliases = {"C"};
        circle.title = "Circle";
        circle.description = "Draw a circle by center and radius, two points or three points";
        circle.category = "Draw";
        circle.create = [] { return std::make_unique<CircleCommand>(); };
        commands.push_back(std::move(circle));
    }
    {
        CommandDefinition ellipse;
        ellipse.id = "cad.draw.ellipse";
        ellipse.name = "ELLIPSE";
        ellipse.aliases = {"EL"};
        ellipse.title = "Ellipse";
        ellipse.description = "Draw an ellipse by two axis endpoints, or from its center";
        ellipse.category = "Draw";
        ellipse.create = [] { return std::make_unique<EllipseCommand>(); };
        commands.push_back(std::move(ellipse));
    }
    {
        CommandDefinition arc;
        arc.id = "cad.draw.arc";
        arc.name = "ARC";
        arc.aliases = {"A"};
        arc.title = "Arc";
        arc.description = "Draw an arc through three points";
        arc.category = "Draw";
        arc.create = [] { return std::make_unique<ArcCommand>(); };
        commands.push_back(std::move(arc));
    }
    {
        CommandDefinition rectangle;
        rectangle.id = "cad.draw.rectangle";
        rectangle.name = "RECTANG";
        rectangle.aliases = {"REC", "RECTANGLE"};
        rectangle.title = "Rectangle";
        rectangle.description = "Draw a rectangular polyline";
        rectangle.category = "Draw";
        rectangle.create = [] { return std::make_unique<RectangleCommand>(); };
        commands.push_back(std::move(rectangle));
    }
    {
        CommandDefinition point;
        point.id = "cad.draw.point";
        point.name = "POINT";
        point.aliases = {"PO"};
        point.title = "Point";
        point.description = "Place point objects";
        point.category = "Draw";
        point.create = [] { return std::make_unique<PointCommand>(); };
        commands.push_back(std::move(point));
    }
    {
        CommandDefinition polygon;
        polygon.id = "cad.draw.polygon";
        polygon.name = "POLYGON";
        polygon.aliases = {"POL"};
        polygon.title = "Polygon";
        polygon.description = "Draw a regular polygon";
        polygon.category = "Draw";
        polygon.create = [] { return std::make_unique<PolygonCommand>(); };
        commands.push_back(std::move(polygon));
    }

    return commands;
}

}  // namespace cad
