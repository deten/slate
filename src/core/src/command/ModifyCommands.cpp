#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <unordered_map>

#include "cad/agent/DrawingReading.hpp"
#include "cad/command/StandardCommands.hpp"
#include "cad/db/BlockTableRecord.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/DimensionEntity.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/Layer.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/math/Angle.hpp"
#include "cad/geom/EllipseArc.hpp"
#include "cad/geom/Operations.hpp"

namespace cad {
namespace {

/// Nearest entity in model space to a pick point.
///
/// A thin alias for the shared `nearestEntityAt`, kept so the commands below
/// read as they did before it was shared with ReshapeCommands.cpp.
[[nodiscard]] Entity* nearestEntity(const Vec3& point, CommandContext& context,
                                    const std::vector<Handle>& exclude = {}) {
    return nearestEntityAt(point, context, exclude);
}

/// Collect the world curves of a set of entities, keeping them alive.
struct CurveSet {
    std::vector<CurvePtr> owned;
    std::vector<const Curve*> views;

    void add(const Entity& entity) {
        for (CurvePtr& curve : entity.toCurves()) {
            views.push_back(curve.get());
            owned.push_back(std::move(curve));
        }
    }
};

/// Point every copied block reference at the copies of its attributes.
///
/// A clone of a reference still names the *original's* attributes. Left so,
/// two references would share one set of values, and moving either would
/// drag the other's text along. Attributes not copied are dropped from the
/// copy rather than shared.
void relinkCopiedAttributes(const std::unordered_map<Handle, Handle>& copies,
                            CommandContext& context) {
    for (const auto& [original, copy] : copies) {
        auto* insert = context.db().get<InsertEntity>(copy);
        if (insert == nullptr) {
            continue;
        }
        for (Handle& attribute : insert->attributeHandles) {
            const auto found = copies.find(attribute);
            attribute = found != copies.end() ? found->second : Handle::null();
        }
        std::erase_if(insert->attributeHandles, [](Handle h) { return h.isNull(); });
    }
}

/// Apply a transform, substituting a different entity type when the transform
/// makes the current one unable to represent the result.
///
/// A circle scaled non-uniformly becomes an ellipse. Silently keeping it a
/// circle would distort the drawing, so the entity is replaced.
void transformEntity(Entity& entity, const Mat4& transform, CommandContext& context) {
    const auto* circle = dynamic_cast<const CircleEntity*>(&entity);
    const auto* arc = dynamic_cast<const ArcEntity*>(&entity);

    const bool needsEllipse = (circle != nullptr && circle->requiresEllipseAfter(transform)) ||
                              (arc != nullptr && arc->requiresEllipseAfter(transform));

    if (!needsEllipse) {
        entity.transformBy(transform);
        return;
    }

    const Arc source = circle != nullptr ? circle->geometry() : arc->geometry();
    const CurvePtr widened = EllipseArc::fromArc(source).transformed(transform);
    const auto* ellipse = dynamic_cast<const EllipseArc*>(widened.get());
    if (ellipse == nullptr) {
        entity.transformBy(transform);
        return;
    }

    auto replacement =
        std::make_unique<EllipseEntity>(EllipseEntity::fromEllipseArc(*ellipse));
    copyProperties(entity, *replacement);

    BlockTableRecord* owner = context.db().get<BlockTableRecord>(entity.ownerHandle);
    context.transaction().addEntity(std::move(replacement),
                                    owner != nullptr ? *owner : context.db().modelSpace());
    context.transaction().erase(entity);
}

/// Shared scaffolding for commands that transform an existing selection.
///
/// MOVE, COPY, ROTATE, SCALE and MIRROR all follow the same shape: acquire a
/// selection, gather a few points, apply a transform. Factoring that out keeps
/// each command to just the part that differs, and means pickfirst,
/// cancellation and undo behave identically across all of them.
class TransformCommand : public CommandInstance {
public:
    StepResult start(CommandContext& context) override {
        if (!context.initialSelection().empty()) {
            selection_ = context.initialSelection();
            return askBasePoint();
        }
        InputRequest request;
        request.kind = InputRequest::Kind::Selection;
        request.prompt = "Select objects";
        request.minCount = 1;
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }

        if (selection_.empty()) {
            if (input.kind != InputValue::Kind::Selection || input.selection.empty()) {
                return StepResult::cancelled("Nothing selected");
            }
            selection_ = input.selection;
            return askBasePoint();
        }

        if (!hasBasePoint_) {
            if (input.kind != InputValue::Kind::Point) {
                return StepResult::cancelled();
            }
            basePoint_ = input.point;
            hasBasePoint_ = true;
            return secondPrompt();
        }

        const auto transform = buildTransform(input);
        if (!transform) {
            return StepResult::cancelled();
        }
        const int count = apply(*transform, context);
        return StepResult::done(std::to_string(count) + " object(s) modified");
    }

    /// A ghost of the selection where it would land.
    ///
    /// Every transform command inherits it, so MOVE, COPY, ROTATE, SCALE and
    /// MIRROR all show where the objects are going instead of asking the user
    /// to imagine it. Before this they showed nothing at all: the drafter
    /// picked a base point, moved the pointer, and the drawing sat still until
    /// the click landed.
    ///
    /// Built by asking the command itself what transform the cursor implies, so
    /// the ghost is the *same* computation the commit will do rather than a
    /// second one that can disagree with it.
    [[nodiscard]] std::vector<std::unique_ptr<Entity>> preview(
        const Vec3& cursor, CommandContext& context) const override {
        if (selection_.empty() || !hasBasePoint_) {
            return {};
        }
        const std::optional<Mat4> transform = buildTransform(InputValue::makePoint(cursor));
        if (!transform) {
            return {};
        }

        // Rebuilt on every cursor move, so a selection of thousands would make
        // the pointer stutter for a picture nobody can read anyway. Past this
        // many the drawing already tells you what is selected.
        constexpr std::size_t kMaxGhosts = 400;

        std::vector<std::unique_ptr<Entity>> ghosts;
        for (const Handle handle : selection_) {
            if (ghosts.size() >= kMaxGhosts) {
                break;
            }
            const Entity* source = context.db().getEntity(handle);
            if (source == nullptr || source->erased) {
                continue;
            }
            auto cloned = source->clone();
            std::unique_ptr<Entity> ghost{dynamic_cast<Entity*>(cloned.release())};
            if (ghost == nullptr) {
                continue;
            }
            ghost->transformBy(*transform);
            ghosts.push_back(std::move(ghost));
        }
        return ghosts;
    }

protected:
    /// Prompt shown once a base point is known.
    [[nodiscard]] virtual StepResult secondPrompt() const = 0;

    /// Build the transform from the collected input.
    [[nodiscard]] virtual std::optional<Mat4> buildTransform(const InputValue& input) const = 0;

    /// True when the command duplicates rather than moves.
    [[nodiscard]] virtual bool copies() const { return false; }

    [[nodiscard]] StepResult askBasePoint() const {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = "Specify base point";
        return StepResult::ask(request);
    }

    /// Apply the transform to every selected entity.
    ///
    /// Locked layers are skipped rather than silently modified — a lock the
    /// application ignores is worse than no lock.
    int apply(const Mat4& transform, CommandContext& context) {
        int count = 0;
        // Original -> copy, for a copied block reference to be re-pointed at
        // its copied attributes rather than sharing the originals'.
        std::unordered_map<Handle, Handle> copiesMade;
        for (const Handle handle : selection_) {
            Entity* entity = context.db().getEntity(handle);
            if (entity == nullptr || entity->erased) {
                continue;
            }

            const Layer* layer = context.db().get<Layer>(entity->layerHandle);
            if (layer != nullptr && layer->isLocked) {
                context.warn("Skipped an object on locked layer \"" + layer->name + "\".");
                continue;
            }

            // Some objects are written back from bytes this application does
            // not interpret, so moving them would change the screen and not
            // the file. They refuse rather than pretending: a move that
            // appears to work and is gone after a save is worse than one that
            // never happened.
            if (!entity->canTransform()) {
                context.warn("Skipped a " + entity->objectType() +
                             ", which this drawing can show but not yet move.");
                continue;
            }

            if (copies()) {
                DbObjectPtr cloned = entity->clone();
                auto* copy = dynamic_cast<Entity*>(cloned.get());
                if (copy == nullptr) {
                    continue;
                }
                cloned.release();
                std::unique_ptr<Entity> owned{copy};
                owned->handle = Handle::null();
                transformEntity(*owned, transform, context);

                BlockTableRecord* owner =
                    context.db().get<BlockTableRecord>(entity->ownerHandle);
                const Entity& added = context.transaction().addEntity(
                    std::move(owned), owner != nullptr ? *owner : context.db().modelSpace());
                copiesMade[handle] = added.handle;
            } else {
                context.transaction().modifyEntity(
                    *entity, [&](Entity& e) { transformEntity(e, transform, context); });
            }
            ++count;
        }
        relinkCopiedAttributes(copiesMade, context);
        return count;
    }

    std::vector<Handle> selection_;
    Vec3 basePoint_;
    bool hasBasePoint_ = false;
};

class MoveCommand : public TransformCommand {
protected:
    [[nodiscard]] StepResult secondPrompt() const override {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = "Specify second point";
        request.basePoint = basePoint_;
        return StepResult::ask(request);
    }

    [[nodiscard]] std::optional<Mat4> buildTransform(const InputValue& input) const override {
        if (input.kind != InputValue::Kind::Point) {
            return std::nullopt;
        }
        return Mat4::translation(input.point - basePoint_);
    }
};

class CopyCommand final : public MoveCommand {
protected:
    [[nodiscard]] bool copies() const override { return true; }
};

class RotateCommand final : public TransformCommand {
protected:
    [[nodiscard]] StepResult secondPrompt() const override {
        InputRequest request;
        request.kind = InputRequest::Kind::Angle;
        request.prompt = "Specify rotation angle";
        request.basePoint = basePoint_;
        return StepResult::ask(request);
    }

    [[nodiscard]] std::optional<Mat4> buildTransform(const InputValue& input) const override {
        if (input.kind == InputValue::Kind::Number) {
            return Mat4::rotationZ(input.number, basePoint_);
        }
        if (input.kind == InputValue::Kind::Point) {
            return Mat4::rotationZ((input.point - basePoint_).angle2(), basePoint_);
        }
        return std::nullopt;
    }
};

class ScaleCommand final : public TransformCommand {
protected:
    [[nodiscard]] StepResult secondPrompt() const override {
        InputRequest request;
        request.kind = InputRequest::Kind::Number;
        request.prompt = "Specify scale factor";
        request.defaultNumber = 1.0;
        return StepResult::ask(request);
    }

    [[nodiscard]] std::optional<Mat4> buildTransform(const InputValue& input) const override {
        double factor = 0.0;
        if (input.kind == InputValue::Kind::Number) {
            factor = input.number;
        } else if (input.kind == InputValue::Kind::Point) {
            factor = basePoint_.distanceTo(input.point);
        } else {
            return std::nullopt;
        }
        if (factor == 0.0) {
            return std::nullopt;
        }
        return Mat4::scaling(factor, basePoint_);
    }
};

/// MIRROR — reflect a selection across a line.
///
/// The originals are kept, which is the non-destructive default. other CAD software offers
/// to erase them; that prompt is deferred rather than half-implemented, since a
/// flag that silently never fires is worse than its absence.
class MirrorCommand final : public TransformCommand {
protected:
    [[nodiscard]] bool copies() const override { return true; }

    [[nodiscard]] StepResult secondPrompt() const override {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = "Specify second point of mirror line";
        request.basePoint = basePoint_;
        return StepResult::ask(request);
    }

    [[nodiscard]] std::optional<Mat4> buildTransform(const InputValue& input) const override {
        if (input.kind != InputValue::Kind::Point) {
            return std::nullopt;
        }
        const Vec3 direction = input.point - basePoint_;
        if (direction.isZero()) {
            return std::nullopt;
        }
        return Mat4::mirror2(basePoint_, direction);
    }
};

class EraseCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& context) override {
        if (!context.initialSelection().empty()) {
            return eraseAll(context.initialSelection(), context);
        }
        InputRequest request;
        request.kind = InputRequest::Kind::Selection;
        request.prompt = "Select objects to erase";
        request.minCount = 1;
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind != InputValue::Kind::Selection || input.selection.empty()) {
            return StepResult::cancelled();
        }
        return eraseAll(input.selection, context);
    }

private:
    static StepResult eraseAll(const std::vector<Handle>& handles, CommandContext& context) {
        int count = 0;
        for (const Handle handle : handles) {
            Entity* entity = context.db().getEntity(handle);
            if (entity == nullptr || entity->erased) {
                continue;
            }
            const Layer* layer = context.db().get<Layer>(entity->layerHandle);
            if (layer != nullptr && layer->isLocked) {
                context.warn("Skipped an object on locked layer \"" + layer->name + "\".");
                continue;
            }

            if (!entity->canTransform()) {
                context.warn("Skipped a " + entity->objectType() +
                             ", which this drawing can show but not yet move.");
                continue;
            }
            context.transaction().erase(*entity);
            ++count;
        }
        if (count == 0) {
            return StepResult::cancelled("Nothing was erased");
        }
        return StepResult::done(std::to_string(count) + " object(s) erased");
    }
};

/// A number as a drafter reads it: no exponent, no trailing zeros.
[[nodiscard]] std::string measured(double value) {
    std::string text = std::format("{:.4f}", value);
    const std::size_t lastDigit = text.find_last_not_of('0');
    if (lastDigit != std::string::npos) {
        text.erase(text[lastDigit] == '.' ? lastDigit : lastDigit + 1);
    }
    return text;
}

/// DIST — how far apart two points are, and in what direction.
///
/// Reports the separation along each axis as well as the straight-line
/// distance, because "how much further right" is asked at least as often as
/// "how far", and subtracting two coordinates by hand is exactly the sort of
/// arithmetic a drawing exists to avoid.
class DistanceCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& /*context*/) override {
        return askPoint("Specify first point");
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
            request.prompt = "Specify second point";
            request.basePoint = first_;
            return StepResult::ask(request);
        }

        const Vec3 delta = input.point - first_;
        const double distance = std::hypot(delta.x, delta.y);
        double degrees = radToDeg(std::atan2(delta.y, delta.x));
        if (degrees < 0.0) {
            degrees += 360.0;
        }
        context.log("Distance = " + measured(distance) + ", Angle = " + measured(degrees) +
                    "°");
        context.log("Delta X = " + measured(delta.x) + ", Delta Y = " + measured(delta.y));
        // Nothing was drawn, so nothing should be undoable. A measurement that
        // put an entry in the undo stack would make Ctrl+Z stop doing what the
        // user expects for one press.
        return StepResult::done("Distance = " + measured(distance));
    }

private:
    [[nodiscard]] static StepResult askPoint(std::string prompt) {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = std::move(prompt);
        return StepResult::ask(request);
    }

    Vec3 first_;
    bool hasFirst_ = false;
};

/// AREA — the area and perimeter of an object, or of points picked around one.
///
/// The Object form is the one that matters on a real drawing: room areas come
/// from the polyline somebody already drew round the room, and re-picking its
/// corners by hand would both take longer and be wrong by however much the
/// picks missed by.
class AreaCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& /*context*/) override {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = "Specify first corner, or an option";
        request.keywords = {{"Object", "O", "Measure a closed object instead"}};
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }
        if (input.kind == InputValue::Kind::Keyword) {
            InputRequest request;
            request.kind = InputRequest::Kind::Entity;
            request.prompt = "Select object";
            return StepResult::ask(request);
        }
        if (input.kind == InputValue::Kind::Entity) {
            return measureEntity(input.entity, context);
        }

        if (input.kind == InputValue::Kind::Empty) {
            return measurePoints(context);
        }
        if (input.kind != InputValue::Kind::Point) {
            return StepResult::cancelled();
        }

        corners_.push_back(input.point);
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = corners_.size() < 3 ? "Specify next corner"
                                             : "Specify next corner, or press Enter for total";
        request.basePoint = corners_.back();
        request.allowEmpty = corners_.size() >= 3;
        return StepResult::ask(request);
    }

private:
    /// The polygon the picked corners enclose, closed back to the first.
    StepResult measurePoints(CommandContext& context) {
        if (corners_.size() < 3) {
            return StepResult::cancelled("Three corners are needed for an area");
        }
        double twiceArea = 0.0;
        double perimeter = 0.0;
        for (std::size_t i = 0; i < corners_.size(); ++i) {
            const Vec3& a = corners_[i];
            const Vec3& b = corners_[(i + 1) % corners_.size()];
            twiceArea += a.x * b.y - b.x * a.y;
            perimeter += a.distanceTo(b);
        }
        return report(std::abs(twiceArea) * 0.5, perimeter, context);
    }

    StepResult measureEntity(Handle handle, CommandContext& context) {
        const Entity* entity = context.db().getEntity(handle);
        if (entity == nullptr || entity->erased) {
            return StepResult::failed("That object is no longer there.");
        }

        double area = 0.0;
        double perimeter = 0.0;
        bool closed = false;
        for (const CurvePtr& curve : entity->toCurves()) {
            if (curve == nullptr) {
                continue;
            }
            perimeter += curve->length();
            // Signed, so a shape traced clockwise does not cancel one traced
            // the other way; the total is taken as a magnitude at the end.
            area += curve->signedArea();
            closed = closed || curve->isClosed();
        }
        if (!closed) {
            // An open shape still has a length worth reporting, and saying so
            // is better than reporting the area of the polygon it would be if
            // its ends were joined without being asked.
            context.log("Length = " + measured(perimeter));
            return StepResult::done("Length = " + measured(perimeter));
        }
        return report(std::abs(area), perimeter, context);
    }

    static StepResult report(double area, double perimeter, CommandContext& context) {
        context.log("Area = " + measured(area) + ", Perimeter = " + measured(perimeter));
        return StepResult::done("Area = " + measured(area));
    }

    std::vector<Vec3> corners_;
};

/// ID — the coordinates of a point.
class IdentifyCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& /*context*/) override {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = "Specify point";
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind != InputValue::Kind::Point) {
            return StepResult::cancelled();
        }
        const std::string text = "X = " + measured(input.point.x) +
                                 ", Y = " + measured(input.point.y) +
                                 ", Z = " + measured(input.point.z);
        context.log(text);
        return StepResult::done(text);
    }
};

/// MATCHPROP — take one object's appearance and give it to others.
///
/// Only *presentation* is copied: the layer, colour, linetype and its scale,
/// lineweight and transparency. Not geometry, and not visibility -- an object
/// deliberately hidden should not reappear because something on its layer was
/// used as a source.
class MatchPropertiesCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& /*context*/) override {
        InputRequest request;
        request.kind = InputRequest::Kind::Entity;
        request.prompt = "Select source object";
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }

        if (source_.isNull()) {
            if (input.kind != InputValue::Kind::Entity) {
                return StepResult::cancelled();
            }
            const Entity* entity = context.db().getEntity(input.entity);
            if (entity == nullptr || entity->erased) {
                return StepResult::failed("That object is no longer there.");
            }
            source_ = input.entity;

            InputRequest request;
            request.kind = InputRequest::Kind::Selection;
            request.prompt = "Select objects to receive the properties";
            request.minCount = 1;
            return StepResult::ask(request);
        }

        if (input.kind != InputValue::Kind::Selection || input.selection.empty()) {
            return StepResult::cancelled();
        }
        return applyTo(input.selection, context);
    }

private:
    StepResult applyTo(const std::vector<Handle>& handles, CommandContext& context) {
        const Entity* source = context.db().getEntity(source_);
        if (source == nullptr || source->erased) {
            return StepResult::failed("The source object is no longer there.");
        }
        // Read out before the loop: the source may itself be in the selection,
        // and reading through the pointer while modifying it would then hand
        // half-applied values to everything after it.
        const Handle layerHandle = source->layerHandle;
        const Color color = source->color;
        const Handle linetypeHandle = source->linetypeHandle;
        const double linetypeScale = source->linetypeScale;
        const std::int16_t lineweight = source->lineweight;
        const Transparency transparency = source->transparency;

        int count = 0;
        for (const Handle handle : handles) {
            Entity* target = context.db().getEntity(handle);
            if (target == nullptr || target->erased || handle == source_) {
                continue;
            }
            const Layer* layer = context.db().get<Layer>(target->layerHandle);
            if (layer != nullptr && layer->isLocked) {
                context.warn("Skipped an object on a locked layer.");
                continue;
            }

            if (!target->canTransform()) {
                context.warn("Skipped a " + target->objectType() +
                             ", which this drawing can show but not yet move.");
                continue;
            }
            context.transaction().modifyEntity(*target, [&](Entity& entity) {
                entity.layerHandle = layerHandle;
                entity.color = color;
                entity.linetypeHandle = linetypeHandle;
                entity.linetypeScale = linetypeScale;
                entity.lineweight = lineweight;
                entity.transparency = transparency;
            });
            ++count;
        }
        if (count == 0) {
            return StepResult::cancelled("Nothing was changed");
        }
        return StepResult::done(std::to_string(count) + " object(s) matched");
    }

    Handle source_;
};

/// ARRAY — repeat a selection in a grid or around a centre.
///
/// Copies, rather than an associative array object. other CAD software's modern ARRAY
/// makes one thing that remembers its own rule and can be edited later; plain
/// copies are what it produces once that is exploded, they round-trip through
/// any format, and every command already knows how to edit them.
class ArrayCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& context) override {
        if (!context.initialSelection().empty()) {
            selection_ = context.initialSelection();
            return askKind();
        }
        InputRequest request;
        request.kind = InputRequest::Kind::Selection;
        request.prompt = "Select objects to array";
        request.minCount = 1;
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }

        if (selection_.empty()) {
            if (input.kind != InputValue::Kind::Selection || input.selection.empty()) {
                return StepResult::cancelled();
            }
            selection_ = input.selection;
            return askKind();
        }

        if (!polar_.has_value()) {
            if (input.kind != InputValue::Kind::Keyword) {
                return askKind();
            }
            polar_ = matchesArrayKeyword(input.text, 'P');
            return *polar_ ? askPoint("Specify center of array")
                           : askNumber("Enter number of rows");
        }

        return *polar_ ? supplyPolar(input, context) : supplyRectangular(input, context);
    }

private:
    [[nodiscard]] static bool matchesArrayKeyword(const std::string& typed, char initial) {
        return !typed.empty() &&
               std::toupper(static_cast<unsigned char>(typed.front())) == initial;
    }

    [[nodiscard]] static StepResult askPoint(std::string prompt) {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = std::move(prompt);
        return StepResult::ask(request);
    }

    [[nodiscard]] static StepResult askNumber(std::string prompt) {
        InputRequest request;
        request.kind = InputRequest::Kind::Number;
        request.prompt = std::move(prompt);
        return StepResult::ask(request);
    }

    [[nodiscard]] static StepResult askDistance(std::string prompt) {
        InputRequest request;
        request.kind = InputRequest::Kind::Distance;
        request.prompt = std::move(prompt);
        return StepResult::ask(request);
    }

    [[nodiscard]] static StepResult askKind() {
        InputRequest request;
        request.kind = InputRequest::Kind::Keyword;
        request.prompt = "Rectangular or Polar";
        request.keywords = {{"Rectangular", "R", "Rows and columns"},
                            {"Polar", "P", "Around a centre point"}};
        return StepResult::ask(request);
    }

    /// Whole copies, and at least one: a count of zero would leave the original
    /// alone and report success, which reads as the command having failed.
    [[nodiscard]] static int asCount(const InputValue& input) {
        if (input.kind != InputValue::Kind::Number) {
            return 0;
        }
        return static_cast<int>(std::lround(input.number));
    }

    StepResult supplyRectangular(const InputValue& input, CommandContext& context) {
        if (rows_ == 0) {
            rows_ = asCount(input);
            if (rows_ < 1) {
                rows_ = 0;
                return askNumber("Enter number of rows");
            }
            return askNumber("Enter number of columns");
        }
        if (columns_ == 0) {
            columns_ = asCount(input);
            if (columns_ < 1) {
                columns_ = 0;
                return askNumber("Enter number of columns");
            }
            if (rows_ == 1 && columns_ == 1) {
                return StepResult::cancelled("A one-by-one array is the original");
            }
            return askDistance("Enter distance between rows");
        }
        if (!hasRowSpacing_) {
            if (input.kind != InputValue::Kind::Number) {
                return askDistance("Enter distance between rows");
            }
            rowSpacing_ = input.number;
            hasRowSpacing_ = true;
            return askDistance("Enter distance between columns");
        }
        if (input.kind != InputValue::Kind::Number) {
            return askDistance("Enter distance between columns");
        }
        const double columnSpacing = input.number;

        int made = 0;
        for (int row = 0; row < rows_; ++row) {
            for (int column = 0; column < columns_; ++column) {
                // The original stays where it is and is not copied over.
                if (row == 0 && column == 0) {
                    continue;
                }
                made += duplicate(Mat4::translation(
                                      Vec3{static_cast<double>(column) * columnSpacing,
                                           static_cast<double>(row) * rowSpacing_, 0.0}),
                                  context);
            }
        }
        return finish(made);
    }

    StepResult supplyPolar(const InputValue& input, CommandContext& context) {
        if (!hasCenter_) {
            if (input.kind != InputValue::Kind::Point) {
                return askPoint("Specify center of array");
            }
            center_ = input.point;
            hasCenter_ = true;
            return askNumber("Enter number of items");
        }
        const int items = asCount(input);
        if (items < 2) {
            return askNumber("Enter number of items");
        }

        // The count includes the original, so the copies are the rest of the
        // circle: eight items around a centre is the original and seven more.
        int made = 0;
        for (int i = 1; i < items; ++i) {
            const double angle = kTau * static_cast<double>(i) / static_cast<double>(items);
            made += duplicate(Mat4::rotationZ(angle, center_), context);
        }
        return finish(made);
    }

    /// One transformed copy of everything selected.
    int duplicate(const Mat4& transform, CommandContext& context) {
        int made = 0;
        std::unordered_map<Handle, Handle> copiesMade;
        for (const Handle handle : selection_) {
            const Entity* source = context.db().getEntity(handle);
            if (source == nullptr || source->erased) {
                continue;
            }
            auto cloned = source->clone();
            std::unique_ptr<Entity> copy{dynamic_cast<Entity*>(cloned.release())};
            if (copy == nullptr) {
                continue;
            }
            copy->handle = Handle::null();
            copy->transformBy(transform);
            copiesMade[handle] = context.transaction().addToCurrentSpace(std::move(copy)).handle;
            ++made;
        }
        relinkCopiedAttributes(copiesMade, context);
        return made;
    }

    [[nodiscard]] static StepResult finish(int made) {
        if (made == 0) {
            return StepResult::cancelled("Nothing was arrayed");
        }
        return StepResult::done(std::to_string(made) + " copy(s) created");
    }

    std::vector<Handle> selection_;
    std::optional<bool> polar_;
    int rows_ = 0;
    int columns_ = 0;
    double rowSpacing_ = 0.0;
    bool hasRowSpacing_ = false;
    Vec3 center_;
    bool hasCenter_ = false;
};

/// OFFSET — create a parallel copy at a given distance, repeating.
class OffsetCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& /*context*/) override {
        InputRequest request;
        request.kind = InputRequest::Kind::Distance;
        request.prompt = "Specify offset distance";
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel || input.kind == InputValue::Kind::Empty) {
            // Escape keeps the offsets already created.
            return hasDistance_ && created_ > 0
                       ? StepResult::done(std::to_string(created_) + " offset(s) created")
                       : StepResult::cancelled();
        }

        if (!hasDistance_) {
            if (input.kind != InputValue::Kind::Number) {
                return StepResult::cancelled();
            }
            if (!(input.number > 0.0)) {
                return StepResult::failed("Offset distance must be greater than zero.");
            }
            distance_ = input.number;
            hasDistance_ = true;
            return askEntity();
        }

        if (sourceHandle_.isNull()) {
            if (input.kind != InputValue::Kind::Entity) {
                return StepResult::cancelled();
            }
            sourceHandle_ = input.entity;
            InputRequest request;
            request.kind = InputRequest::Kind::Point;
            request.prompt = "Specify point on side to offset";
            return StepResult::ask(request);
        }

        if (input.kind != InputValue::Kind::Point) {
            return StepResult::cancelled();
        }

        Entity* source = context.db().getEntity(sourceHandle_);
        if (source == nullptr) {
            return StepResult::failed("The selected object no longer exists.");
        }

        const std::vector<CurvePtr> curves = source->toCurves();
        if (curves.empty()) {
            return StepResult::failed("That object has no geometry that can be offset.");
        }
        const Curve& curve = *curves.front();

        // The sign is chosen so the result lands on the side the user clicked.
        const ClosestPointResult projection = curve.closestPoint(input.point);
        const Vec3 tangent = curve.tangentAt(projection.param);
        const Vec3 toSide = input.point - projection.point;
        const double signedDistance = tangent.cross2(toSide) >= 0.0 ? distance_ : -distance_;

        const CurvePtr offset = offsetCurve(curve, signedDistance);
        if (offset == nullptr) {
            return StepResult::failed("The offset distance is too large for this geometry.");
        }

        std::unique_ptr<Entity> entity = entityFromCurve(*offset);
        if (entity == nullptr) {
            return StepResult::failed("The offset result cannot be represented.");
        }
        copyProperties(*source, *entity);
        context.transaction().addToCurrentSpace(std::move(entity));
        ++created_;

        // Offset repeats, so a run of parallel lines is one command.
        sourceHandle_ = Handle::null();
        return askEntity();
    }

private:
    [[nodiscard]] static StepResult askEntity() {
        InputRequest request;
        request.kind = InputRequest::Kind::Entity;
        request.prompt = "Select object to offset";
        request.allowEmpty = true;
        return StepResult::ask(request);
    }

    double distance_ = 0.0;
    bool hasDistance_ = false;
    Handle sourceHandle_;
    int created_ = 0;
};

/// TRIM — cut a curve back to its intersections with cutting edges.
class TrimCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& context) override {
        if (!context.initialSelection().empty()) {
            edges_ = context.initialSelection();
            return askTarget();
        }
        InputRequest request;
        request.kind = InputRequest::Kind::Selection;
        request.prompt = "Select cutting edges";
        request.minCount = 1;
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel || input.kind == InputValue::Kind::Empty) {
            return trimmed_ > 0 ? StepResult::done(std::to_string(trimmed_) + " object(s) trimmed")
                                : StepResult::cancelled();
        }

        if (edges_.empty()) {
            if (input.kind != InputValue::Kind::Selection) {
                return StepResult::cancelled();
            }
            edges_ = input.selection;
            return askTarget();
        }

        if (input.kind != InputValue::Kind::Point) {
            return StepResult::done();
        }

        CurveSet cutting;
        for (const Handle handle : edges_) {
            if (const Entity* entity = context.db().getEntity(handle)) {
                cutting.add(*entity);
            }
        }

        Entity* target = nearestEntity(input.point, context, edges_);
        if (target == nullptr) {
            context.warn("No object found at that point.");
            return askTarget();
        }

        const std::vector<CurvePtr> curves = target->toCurves();
        if (curves.empty()) {
            context.warn("That object cannot be trimmed.");
            return askTarget();
        }

        const TrimResult result = trimCurve(*curves.front(), cutting.views, input.point);
        if (!result.trimmed) {
            context.warn("That object does not cross any cutting edge.");
            return askTarget();
        }

        // The trimmed curve may become several pieces, so the original is
        // erased and replaced rather than mutated.
        for (const CurvePtr& piece : result.remaining) {
            std::unique_ptr<Entity> entity = entityFromCurve(*piece);
            if (entity == nullptr) {
                continue;
            }
            copyProperties(*target, *entity);
            context.transaction().addToCurrentSpace(std::move(entity));
        }
        context.transaction().erase(*target);
        ++trimmed_;

        return askTarget();
    }

private:
    [[nodiscard]] static StepResult askTarget() {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = "Select object to trim";
        request.allowEmpty = true;
        return StepResult::ask(request);
    }

    std::vector<Handle> edges_;
    int trimmed_ = 0;
};

/// EXTEND — lengthen a curve until it meets a boundary.
class ExtendCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& context) override {
        if (!context.initialSelection().empty()) {
            boundaries_ = context.initialSelection();
            return askTarget();
        }
        InputRequest request;
        request.kind = InputRequest::Kind::Selection;
        request.prompt = "Select boundary edges";
        request.minCount = 1;
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel || input.kind == InputValue::Kind::Empty) {
            return extended_ > 0
                       ? StepResult::done(std::to_string(extended_) + " object(s) extended")
                       : StepResult::cancelled();
        }

        if (boundaries_.empty()) {
            if (input.kind != InputValue::Kind::Selection) {
                return StepResult::cancelled();
            }
            boundaries_ = input.selection;
            return askTarget();
        }

        if (input.kind != InputValue::Kind::Point) {
            return StepResult::done();
        }

        CurveSet bounds;
        for (const Handle handle : boundaries_) {
            if (const Entity* entity = context.db().getEntity(handle)) {
                bounds.add(*entity);
            }
        }

        Entity* target = nearestEntity(input.point, context, boundaries_);
        if (target == nullptr) {
            context.warn("No object found at that point.");
            return askTarget();
        }

        const std::vector<CurvePtr> curves = target->toCurves();
        if (curves.empty()) {
            context.warn("That object cannot be extended.");
            return askTarget();
        }

        const CurvePtr extendedCurve = extendCurve(*curves.front(), bounds.views, input.point);
        if (extendedCurve == nullptr) {
            context.warn("There is no boundary beyond that end to extend to.");
            return askTarget();
        }

        std::unique_ptr<Entity> replacement = entityFromCurve(*extendedCurve);
        if (replacement != nullptr) {
            copyProperties(*target, *replacement);
            context.transaction().addToCurrentSpace(std::move(replacement));
            context.transaction().erase(*target);
            ++extended_;
        }
        return askTarget();
    }

private:
    [[nodiscard]] static StepResult askTarget() {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = "Select object to extend";
        request.allowEmpty = true;
        return StepResult::ask(request);
    }

    std::vector<Handle> boundaries_;
    int extended_ = 0;
};

/// FILLET — round the corner between two lines.
class FilletCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& /*context*/) override {
        InputRequest request;
        request.kind = InputRequest::Kind::Distance;
        request.prompt = "Specify fillet radius";
        request.defaultNumber = 0.0;
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }

        if (!hasRadius_) {
            if (input.kind == InputValue::Kind::Number) {
                if (input.number < 0.0) {
                    return StepResult::failed("Fillet radius cannot be negative.");
                }
                radius_ = input.number;
            }
            hasRadius_ = true;
            return askObject("Select first object");
        }

        if (input.kind != InputValue::Kind::Point) {
            return StepResult::cancelled();
        }

        Entity* picked = nearestEntity(input.point, context);
        if (picked == nullptr) {
            context.warn("No object found at that point.");
            return askObject("Select first object");
        }

        if (firstHandle_.isNull()) {
            firstHandle_ = picked->handle;
            firstPick_ = input.point;
            return askObject("Select second object");
        }

        if (picked->handle == firstHandle_) {
            context.warn("Select a different object for the second edge.");
            return askObject("Select second object");
        }

        Entity* first = context.db().getEntity(firstHandle_);
        if (first == nullptr) {
            return StepResult::cancelled();
        }

        const std::vector<CurvePtr> curvesA = first->toCurves();
        const std::vector<CurvePtr> curvesB = picked->toCurves();
        if (curvesA.empty() || curvesB.empty()) {
            return StepResult::failed("Those objects cannot be filleted.");
        }

        const auto result =
            filletCurves(*curvesA.front(), *curvesB.front(), radius_, firstPick_, input.point);
        if (!result) {
            return StepResult::failed(
                "Cannot fillet those objects with that radius. Currently only line-to-line "
                "fillets are supported.");
        }

        if (radius_ > 0.0) {
            auto arcEntity = std::make_unique<ArcEntity>(ArcEntity::fromArc(result->arc));
            copyProperties(*first, *arcEntity);
            context.transaction().addToCurrentSpace(std::move(arcEntity));
        }

        replaceWith(*first, result->trimmedA, context);
        replaceWith(*picked, result->trimmedB, context);
        return StepResult::done();
    }

private:
    [[nodiscard]] static StepResult askObject(std::string prompt) {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = std::move(prompt);
        return StepResult::ask(request);
    }

    static void replaceWith(Entity& original, const LineSegment& trimmed,
                            CommandContext& context) {
        std::unique_ptr<Entity> replacement = entityFromCurve(trimmed);
        if (replacement == nullptr) {
            return;
        }
        copyProperties(original, *replacement);
        context.transaction().addToCurrentSpace(std::move(replacement));
        context.transaction().erase(original);
    }

    double radius_ = 0.0;
    bool hasRadius_ = false;
    Handle firstHandle_;
    Vec3 firstPick_;
};

/// UNDO and REDO manage their own history rather than creating one.
class UndoCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& context) override {
        if (!context.transactions().canUndo()) {
            return StepResult::cancelled("Nothing to undo");
        }
        const std::string name = context.transactions().undoName();
        context.transactions().undo();
        return StepResult::done("Undo " + name);
    }

    StepResult supply(const InputValue& /*input*/, CommandContext& /*context*/) override {
        return StepResult::done();
    }
};

class RedoCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& context) override {
        if (!context.transactions().canRedo()) {
            return StepResult::cancelled("Nothing to redo");
        }
        const std::string name = context.transactions().redoName();
        context.transactions().redo();
        return StepResult::done("Redo " + name);
    }

    StepResult supply(const InputValue& /*input*/, CommandContext& /*context*/) override {
        return StepResult::done();
    }
};

/// Populate the shared fields of a modify command definition.
/// Points along a curve at given distances from its start, by walking its
/// tessellation. Parameterisation differs between curve kinds; arc length
/// along a fine polyline does not.
[[nodiscard]] std::vector<Vec3> pointsAlong(const Curve& curve, const std::vector<double>& distances) {
    TessellationOptions options;
    options.chordTolerance = std::max(curve.length() * 1e-5, 1e-9);
    const std::vector<Vec3> polyline = curve.tessellate(options);
    std::vector<Vec3> result;
    if (polyline.size() < 2) {
        return result;
    }
    std::size_t segment = 0;
    double walked = 0.0;
    for (const double target : distances) {
        while (segment + 1 < polyline.size() &&
               walked + polyline[segment].distanceTo(polyline[segment + 1]) < target) {
            walked += polyline[segment].distanceTo(polyline[segment + 1]);
            ++segment;
        }
        if (segment + 1 >= polyline.size()) {
            result.push_back(polyline.back());
            continue;
        }
        const double span = polyline[segment].distanceTo(polyline[segment + 1]);
        const double t = span > 0.0 ? std::clamp((target - walked) / span, 0.0, 1.0) : 0.0;
        result.push_back(polyline[segment].lerp(polyline[segment + 1], t));
    }
    return result;
}

/// MEASURE and DIVIDE: points along an object, every so far or so many.
class MarkAlongCommand final : public CommandInstance {
public:
    explicit MarkAlongCommand(bool divide) : divide_(divide) {}

    StepResult start(CommandContext& context) override {
        for (const Handle handle : context.initialSelection()) {
            if (adopt(handle, context)) {
                return askHowMany();
            }
        }
        InputRequest request;
        request.kind = InputRequest::Kind::Entity;
        request.prompt = divide_ ? "Select object to divide" : "Select object to measure";
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }
        if (curve_ == nullptr) {
            Handle picked;
            if (input.kind == InputValue::Kind::Entity) {
                picked = input.entity;
            } else if (input.kind == InputValue::Kind::Selection && !input.selection.empty()) {
                picked = input.selection.front();
            }
            if (!adopt(picked, context)) {
                return StepResult::failed("Select a line, arc, circle, polyline, spline or ellipse.");
            }
            return askHowMany();
        }
        if (input.kind != InputValue::Kind::Number || input.number <= 0.0) {
            return StepResult::failed(divide_ ? "Enter the number of segments, 2 or more."
                                              : "Enter a positive length.");
        }
        const double length = curve_->length();
        std::vector<double> distances;
        if (divide_) {
            const int segments = static_cast<int>(std::lround(input.number));
            if (segments < 2 || segments > 32767) {
                return StepResult::failed("Enter the number of segments, 2 or more.");
            }
            for (int i = 1; i < segments; ++i) {
                distances.push_back(length * static_cast<double>(i) / segments);
            }
        } else {
            for (double at = input.number; at < length - 1e-9; at += input.number) {
                distances.push_back(at);
            }
            if (distances.size() > 32767) {
                return StepResult::failed("That interval would place too many points.");
            }
        }
        int placed = 0;
        for (const Vec3& position : pointsAlong(*curve_, distances)) {
            auto point = std::make_unique<PointEntity>(position);
            point->layerHandle = layerHandle_;
            context.transaction().addToCurrentSpace(std::move(point));
            ++placed;
        }
        if (placed == 0) {
            return StepResult::done(divide_ ? "Nothing to place" : "The object is shorter than that interval");
        }
        return StepResult::done(std::to_string(placed) + " point(s) placed");
    }

private:
    bool adopt(Handle handle, CommandContext& context) {
        const Entity* entity = context.db().getEntity(handle);
        if (entity == nullptr || entity->erased) {
            return false;
        }
        std::vector<CurvePtr> curves = entity->toCurves();
        if (curves.empty() || curves.front()->length() <= 0.0) {
            return false;
        }
        curve_ = std::move(curves.front());
        layerHandle_ = entity->layerHandle;
        return true;
    }

    [[nodiscard]] StepResult askHowMany() const {
        InputRequest request;
        request.kind = divide_ ? InputRequest::Kind::Number : InputRequest::Kind::Distance;
        request.prompt = divide_ ? "Enter the number of segments" : "Specify length of segment";
        request.integerOnly = divide_;
        return StepResult::ask(request);
    }

    bool divide_ = false;
    CurvePtr curve_;
    Handle layerHandle_;
};

/// LIST: what an object is, in words, in the history.
/// DESCRIBE: the drawing read as rooms, systems, shell and title block, in
/// the history.
///
/// The same reading the assistant is given, shown to the person, so what it
/// understood can be checked before anything is asked of it: a room it did
/// not find gets no duct.
class DescribeCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& context) override {
        const DrawingReading reading = readDrawing(context.db());
        const std::string text = reading.describe();
        if (text.empty()) {
            return StepResult::done("Nothing to describe: no rooms, systems or title block found");
        }
        std::size_t begin = 0;
        while (begin < text.size()) {
            std::size_t end = text.find('\n', begin);
            if (end == std::string::npos) {
                end = text.size();
            }
            context.log(text.substr(begin, end - begin));
            begin = end + 1;
        }
        std::string summary = std::to_string(reading.rooms.size()) + " room" +
                              (reading.rooms.size() == 1 ? "" : "s") + ", " +
                              std::to_string(reading.systems.size()) + " system layer" +
                              (reading.systems.size() == 1 ? "" : "s");
        if (!reading.titleBlocks.empty()) {
            summary += ", title block read";
        }
        return StepResult::done(summary);
    }

    StepResult supply(const InputValue& /*input*/, CommandContext& /*context*/) override {
        return StepResult::done();
    }
};

class ListCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& context) override {
        if (!context.initialSelection().empty()) {
            return report(context.initialSelection(), context);
        }
        InputRequest request;
        request.kind = InputRequest::Kind::Selection;
        request.prompt = "Select objects";
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind != InputValue::Kind::Selection || input.selection.empty()) {
            return StepResult::cancelled();
        }
        return report(input.selection, context);
    }

private:
    [[nodiscard]] static std::string point(const Vec3& p) {
        return measured(p.x) + ", " + measured(p.y) + (p.z != 0.0 ? ", " + measured(p.z) : "");
    }

    static StepResult report(const std::vector<Handle>& handles, CommandContext& context) {
        Database& db = context.db();
        int listed = 0;
        for (const Handle handle : handles) {
            const Entity* entity = db.getEntity(handle);
            if (entity == nullptr || entity->erased) {
                continue;
            }
            const auto* layer = db.get<Layer>(entity->layerHandle);
            context.log(entity->objectType() + "  Layer: \"" + (layer != nullptr ? layer->name : "?") +
                        "\"  Handle: " + entity->handle.toString());
            if (const auto* line = dynamic_cast<const LineEntity*>(entity)) {
                context.log("  from " + point(line->start) + " to " + point(line->end));
                context.log("  Length = " + measured(line->start.distanceTo(line->end)) +
                            ", Angle = " + measured(radToDeg((line->end - line->start).angle2())));
            } else if (const auto* circle = dynamic_cast<const CircleEntity*>(entity)) {
                context.log("  center " + point(circle->center) + ", radius " + measured(circle->radius));
                context.log("  Circumference = " + measured(kTau * circle->radius) +
                            ", Area = " + measured(kPi * circle->radius * circle->radius));
            } else if (const auto* arc = dynamic_cast<const ArcEntity*>(entity)) {
                context.log("  center " + point(arc->center) + ", radius " + measured(arc->radius));
                context.log("  from " + measured(radToDeg(arc->startAngle)) + " to " +
                            measured(radToDeg(arc->endAngle)) + " degrees");
            } else if (const auto* poly = dynamic_cast<const PolylineEntity*>(entity)) {
                context.log("  " + std::to_string(poly->vertices.size()) + " vertices, " +
                            (poly->closed ? "closed" : "open"));
                for (std::size_t i = 0; i < poly->vertices.size() && i < 32; ++i) {
                    context.log("    " + point(poly->vertices[i].position) +
                                (poly->vertices[i].bulge != 0.0
                                     ? "  bulge " + measured(poly->vertices[i].bulge)
                                     : std::string{}));
                }
            } else if (const auto* text = dynamic_cast<const TextEntity*>(entity)) {
                context.log("  \"" + text->contents + "\" at " + point(text->position) +
                            ", height " + measured(text->height));
            } else if (const auto* mtext = dynamic_cast<const MTextEntity*>(entity)) {
                context.log("  \"" + mtext->plainText() + "\" at " + point(mtext->position) +
                            ", height " + measured(mtext->height));
            } else if (const auto* insert = dynamic_cast<const InsertEntity*>(entity)) {
                const auto* block = db.get<BlockTableRecord>(insert->blockHandle);
                context.log("  block \"" + (block != nullptr ? block->name : "?") + "\" at " +
                            point(insert->position) + ", scale " + measured(insert->scale.x) +
                            ", rotation " + measured(radToDeg(insert->rotation)));
            } else if (const auto* dimension = dynamic_cast<const DimensionEntity*>(entity)) {
                context.log("  measures " + measured(dimension->measure()));
            }
            // Length and area, for anything that is a curve.
            double length = 0.0;
            double area = 0.0;
            bool anyCurve = false;
            for (const CurvePtr& curve : entity->toCurves()) {
                anyCurve = true;
                length += curve->length();
                if (curve->hasArea()) {
                    area += std::abs(curve->signedArea());
                }
            }
            if (anyCurve && dynamic_cast<const LineEntity*>(entity) == nullptr &&
                dynamic_cast<const CircleEntity*>(entity) == nullptr) {
                context.log("  Length = " + measured(length) +
                            (area > 0.0 ? ", Area = " + measured(area) : std::string{}));
            }
            const Box3 bounds = entity->bounds();
            if (!bounds.isEmpty()) {
                context.log("  extents " + point(bounds.min) + " to " + point(bounds.max));
            }
            ++listed;
        }
        return StepResult::done(std::to_string(listed) + " object(s) listed");
    }
};

/// Whether a typed keyword is the one starting with `initial`.
[[nodiscard]] bool matchesKeyword(const std::string& typed, char initial) {
    return !typed.empty() &&
           std::toupper(static_cast<unsigned char>(typed.front())) == initial;
}

/// DRAWORDER — which objects draw over which.
///
/// The order objects are kept in is the order they are drawn in, here and in
/// the file, so this is a reordering of the space's list: to the front, to
/// the back, or just above or below one other object. A hatch sent to the
/// back stops hiding the walls it fills between.
class DrawOrderCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& context) override {
        if (!context.initialSelection().empty()) {
            selection_ = context.initialSelection();
            return askMode();
        }
        InputRequest request;
        request.kind = InputRequest::Kind::Selection;
        request.prompt = "Select objects to reorder";
        request.minCount = 1;
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }
        switch (stage_) {
            case Stage::Select:
                if (input.kind == InputValue::Kind::Selection) {
                    selection_ = input.selection;
                } else if (input.kind == InputValue::Kind::Entity) {
                    selection_ = {input.entity};
                }
                if (selection_.empty()) {
                    return StepResult::cancelled("Nothing selected.");
                }
                return askMode();
            case Stage::Mode: {
                if (input.kind == InputValue::Kind::Keyword || input.kind == InputValue::Kind::Text) {
                    if (matchesKeyword(input.text, 'A')) {
                        mode_ = Mode::Above;
                    } else if (matchesKeyword(input.text, 'U')) {
                        mode_ = Mode::Under;
                    } else if (matchesKeyword(input.text, 'F')) {
                        mode_ = Mode::Front;
                    } else {
                        mode_ = Mode::Back;
                    }
                }
                if (mode_ == Mode::Above || mode_ == Mode::Under) {
                    stage_ = Stage::Reference;
                    InputRequest request;
                    request.kind = InputRequest::Kind::Entity;
                    request.prompt = mode_ == Mode::Above ? "Select the object to draw above"
                                                          : "Select the object to draw under";
                    return StepResult::ask(request);
                }
                return apply(Handle{}, context);
            }
            case Stage::Reference:
            default:
                if (input.kind != InputValue::Kind::Entity) {
                    return StepResult::cancelled();
                }
                return apply(input.entity, context);
        }
    }

private:
    enum class Stage : std::uint8_t { Select, Mode, Reference };
    enum class Mode : std::uint8_t { Front, Back, Above, Under };

    [[nodiscard]] StepResult askMode() {
        stage_ = Stage::Mode;
        InputRequest request;
        request.kind = InputRequest::Kind::Keyword;
        request.prompt = "Enter object ordering option [Above/Under/Front/Back]";
        request.defaultText = "Back";
        request.allowEmpty = true;
        request.keywords = {CommandKeyword{"Above", "A", "Just over one other object"},
                            CommandKeyword{"Under", "U", "Just beneath one other object"},
                            CommandKeyword{"Front", "F", "Over everything"},
                            CommandKeyword{"Back", "B", "Under everything"}};
        return StepResult::ask(request);
    }

    [[nodiscard]] StepResult apply(Handle reference, CommandContext& context) {
        Database& db = context.db();
        // The space the selection lives in: every selected object's owner has
        // to be the same one, or "in front" means nothing.
        Handle spaceHandle;
        std::vector<Handle> moving;
        for (const Handle handle : selection_) {
            const Entity* entity = db.getEntity(handle);
            if (entity == nullptr || entity->erased || handle == reference) {
                continue;
            }
            if (spaceHandle.isNull()) {
                spaceHandle = entity->ownerHandle;
            } else if (entity->ownerHandle != spaceHandle) {
                return StepResult::failed("The objects are not all in the same space.");
            }
            moving.push_back(handle);
        }
        auto* space = db.get<BlockTableRecord>(spaceHandle);
        if (space == nullptr || moving.empty()) {
            return StepResult::failed("Nothing to reorder.");
        }
        if (!reference.isNull()) {
            const Entity* other = db.getEntity(reference);
            if (other == nullptr || other->ownerHandle != spaceHandle) {
                return StepResult::failed("The reference object is not in the same space.");
            }
        }

        // Keep the moved objects in the order they already had among
        // themselves; only their place against the rest changes.
        std::vector<Handle> kept;
        std::vector<Handle> picked;
        for (const Handle handle : space->entityHandles) {
            if (std::find(moving.begin(), moving.end(), handle) != moving.end()) {
                picked.push_back(handle);
            } else {
                kept.push_back(handle);
            }
        }
        std::vector<Handle> order;
        switch (mode_) {
            case Mode::Front:
                order = kept;
                order.insert(order.end(), picked.begin(), picked.end());
                break;
            case Mode::Back:
                order = picked;
                order.insert(order.end(), kept.begin(), kept.end());
                break;
            case Mode::Above:
            case Mode::Under:
                for (const Handle handle : kept) {
                    if (handle == reference && mode_ == Mode::Under) {
                        order.insert(order.end(), picked.begin(), picked.end());
                    }
                    order.push_back(handle);
                    if (handle == reference && mode_ == Mode::Above) {
                        order.insert(order.end(), picked.begin(), picked.end());
                    }
                }
                break;
        }
        BlockTableRecord& writable = context.transaction().openForWrite(*space);
        writable.entityHandles = std::move(order);
        db.notifyModified(writable.handle);
        const char* where = mode_ == Mode::Front   ? "to the front"
                            : mode_ == Mode::Back  ? "to the back"
                            : mode_ == Mode::Above ? "above the reference"
                                                   : "under the reference";
        return StepResult::done(std::to_string(picked.size()) + " object(s) moved " + where);
    }

    Stage stage_ = Stage::Select;
    Mode mode_ = Mode::Back;
    std::vector<Handle> selection_;
};

/// ALIGN — move, turn and optionally scale objects so that picked points land
/// on picked points.
///
/// One pair of points is a move. Two pairs add the turn that takes the first
/// source direction onto the destination direction, and, if asked, the scale
/// that makes the distances match: how a detail traced at one size is dropped
/// onto a plan at another.
class AlignCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& context) override {
        if (!context.initialSelection().empty()) {
            selection_ = context.initialSelection();
            return askPoint("Specify first source point");
        }
        InputRequest request;
        request.kind = InputRequest::Kind::Selection;
        request.prompt = "Select objects to align";
        request.minCount = 1;
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }
        switch (stage_) {
            case Stage::Select:
                if (input.kind == InputValue::Kind::Selection) {
                    selection_ = input.selection;
                } else if (input.kind == InputValue::Kind::Entity) {
                    selection_ = {input.entity};
                }
                if (selection_.empty()) {
                    return StepResult::cancelled("Nothing selected.");
                }
                return askPoint("Specify first source point");
            case Stage::Source1:
                if (input.kind != InputValue::Kind::Point) {
                    return StepResult::cancelled();
                }
                source1_ = input.point;
                return askPoint("Specify first destination point");
            case Stage::Destination1:
                if (input.kind != InputValue::Kind::Point) {
                    return StepResult::cancelled();
                }
                destination1_ = input.point;
                return askPoint("Specify second source point or press Enter", true);
            case Stage::Source2:
                if (input.kind != InputValue::Kind::Point) {
                    return apply(context);
                }
                source2_ = input.point;
                hasSecond_ = true;
                return askPoint("Specify second destination point");
            case Stage::Destination2: {
                if (input.kind != InputValue::Kind::Point) {
                    return StepResult::cancelled();
                }
                destination2_ = input.point;
                stage_ = Stage::Scale;
                InputRequest request;
                request.kind = InputRequest::Kind::Keyword;
                request.prompt = "Scale objects based on alignment points? [Yes/No]";
                request.defaultText = "No";
                request.allowEmpty = true;
                request.keywords = {CommandKeyword{"Yes", "Y", "Make the distances match"},
                                    CommandKeyword{"No", "N", "Keep the size"}};
                return StepResult::ask(request);
            }
            case Stage::Scale:
            default:
                scale_ = (input.kind == InputValue::Kind::Keyword ||
                          input.kind == InputValue::Kind::Text) &&
                         matchesKeyword(input.text, 'Y');
                return apply(context);
        }
    }

private:
    enum class Stage : std::uint8_t { Select, Source1, Destination1, Source2, Destination2, Scale };

    [[nodiscard]] StepResult askPoint(const char* prompt, bool allowEmpty = false) {
        stage_ = static_cast<Stage>(static_cast<int>(stage_) + 1);
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = prompt;
        request.allowEmpty = allowEmpty;
        return StepResult::ask(request);
    }

    [[nodiscard]] StepResult apply(CommandContext& context) {
        Mat4 transform = Mat4::translation(destination1_ - source1_);
        if (hasSecond_) {
            const Vec3 from = source2_ - source1_;
            const Vec3 to = destination2_ - destination1_;
            if (from.lengthSquared() < 1e-18 || to.lengthSquared() < 1e-18) {
                return StepResult::failed("The two points of a pair must differ.");
            }
            const double angle = to.angle2() - from.angle2();
            transform = Mat4::rotationZ(angle, destination1_) * transform;
            if (scale_) {
                transform = Mat4::scaling(to.length() / from.length(), destination1_) * transform;
            }
        }
        int count = 0;
        for (const Handle handle : selection_) {
            Entity* entity = context.db().getEntity(handle);
            if (entity == nullptr || entity->erased) {
                continue;
            }
            const Layer* layer = context.db().get<Layer>(entity->layerHandle);
            if (layer != nullptr && layer->isLocked) {
                context.warn("Skipped an object on locked layer \"" + layer->name + "\".");
                continue;
            }
            if (!entity->canTransform()) {
                context.warn("Skipped a " + entity->objectType() + ", which cannot be moved.");
                continue;
            }
            context.transaction().modifyEntity(
                *entity, [&](Entity& e) { transformEntity(e, transform, context); });
            ++count;
        }
        return StepResult::done(std::to_string(count) + " object(s) aligned");
    }

    Stage stage_ = Stage::Select;
    std::vector<Handle> selection_;
    Vec3 source1_;
    Vec3 destination1_;
    Vec3 source2_;
    Vec3 destination2_;
    bool hasSecond_ = false;
    bool scale_ = false;
};

[[nodiscard]] CommandDefinition makeDefinition(std::string id, std::string name,
                                               std::vector<std::string> aliases,
                                               std::string title, std::string description,
                                               std::function<CommandInstancePtr()> create,
                                               bool pickfirst = true) {
    CommandDefinition definition;
    definition.id = std::move(id);
    definition.name = std::move(name);
    definition.aliases = std::move(aliases);
    definition.title = std::move(title);
    definition.description = std::move(description);
    definition.category = "Modify";
    definition.supportsPickfirst = pickfirst;
    definition.create = std::move(create);
    return definition;
}

}  // namespace

std::unique_ptr<Entity> cloneEntity(const Entity& entity) {
    DbObjectPtr clone = entity.clone();
    auto* typed = dynamic_cast<Entity*>(clone.get());
    if (typed == nullptr) {
        return nullptr;
    }
    clone.release();
    return std::unique_ptr<Entity>{typed};
}

Entity* nearestEntityAt(const Vec3& point, CommandContext& context,
                        const std::vector<Handle>& exclude) {
    Entity* best = nullptr;
    double bestDistance = std::numeric_limits<double>::infinity();

    for (Entity* entity : context.db().entitiesIn(context.db().modelSpace())) {
        if (std::find(exclude.begin(), exclude.end(), entity->handle) != exclude.end()) {
            continue;
        }
        const double distance = entity->distanceToPoint(point);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = entity;
        }
    }
    return best;
}

std::unique_ptr<Entity> entityFromCurve(const Curve& curve) {
    if (const auto* line = dynamic_cast<const LineSegment*>(&curve)) {
        return std::make_unique<LineEntity>(line->start, line->end);
    }
    if (const auto* arc = dynamic_cast<const Arc*>(&curve)) {
        if (arc->isFullCircle()) {
            return std::make_unique<CircleEntity>(arc->center, arc->radius);
        }
        return std::make_unique<ArcEntity>(ArcEntity::fromArc(*arc));
    }
    if (const auto* ellipse = dynamic_cast<const EllipseArc*>(&curve)) {
        return std::make_unique<EllipseEntity>(EllipseEntity::fromEllipseArc(*ellipse));
    }
    if (const auto* poly = dynamic_cast<const Polyline*>(&curve)) {
        return std::make_unique<PolylineEntity>(PolylineEntity::fromPolyline(*poly));
    }
    return nullptr;
}

void copyProperties(const Entity& source, Entity& target) {
    target.layerHandle = source.layerHandle;
    target.color = source.color;
    target.linetypeHandle = source.linetypeHandle;
    target.linetypeScale = source.linetypeScale;
    target.lineweight = source.lineweight;
    target.transparency = source.transparency;
    target.visible = source.visible;
    target.normal = source.normal;
}

std::vector<CommandDefinition> modifyCommands() {
    std::vector<CommandDefinition> commands;

    commands.push_back(makeDefinition("cad.modify.move", "MOVE", {"M"}, "Move",
                                      "Move objects by a displacement",
                                      [] { return std::make_unique<MoveCommand>(); }));
    commands.push_back(makeDefinition("cad.modify.align", "ALIGN", {"AL"}, "Align",
                                      "Move, turn and scale objects so picked points land on "
                                      "picked points",
                                      [] { return std::make_unique<AlignCommand>(); }));
    commands.push_back(makeDefinition("cad.modify.draworder", "DRAWORDER", {"DR"}, "Draw Order",
                                      "Bring objects to the front, send them back, or put "
                                      "them just above or under another",
                                      [] { return std::make_unique<DrawOrderCommand>(); }));
    commands.push_back(makeDefinition("cad.modify.copy", "COPY", {"CO", "CP"}, "Copy",
                                      "Duplicate objects at a displacement",
                                      [] { return std::make_unique<CopyCommand>(); }));
    commands.push_back(makeDefinition("cad.modify.rotate", "ROTATE", {"RO"}, "Rotate",
                                      "Rotate objects about a base point",
                                      [] { return std::make_unique<RotateCommand>(); }));
    commands.push_back(makeDefinition("cad.modify.scale", "SCALE", {"SC"}, "Scale",
                                      "Scale objects about a base point",
                                      [] { return std::make_unique<ScaleCommand>(); }));
    commands.push_back(makeDefinition("cad.modify.mirror", "MIRROR", {"MI"}, "Mirror",
                                      "Reflect objects across a line",
                                      [] { return std::make_unique<MirrorCommand>(); }));
    commands.push_back(makeDefinition("cad.modify.erase", "ERASE", {"E", "DELETE"}, "Erase",
                                      "Delete objects",
                                      [] { return std::make_unique<EraseCommand>(); }));
    // Measurement changes nothing, so none of these takes a prior selection.
    // Filed under Tools rather than Modify -- the category decides which tab an
    // uncurated command surfaces on, and a command that measures does not
    // belong among the ones that alter the drawing.
    const auto inquiry = [&](std::string id, std::string name, std::vector<std::string> aliases,
                             std::string title, std::string description,
                             std::function<CommandInstancePtr()> create) {
        CommandDefinition definition =
            makeDefinition(std::move(id), std::move(name), std::move(aliases), std::move(title),
                           std::move(description), std::move(create), false);
        definition.category = "Tools";
        definition.transactionMode = CommandTransactionMode::ReadOnly;
        commands.push_back(std::move(definition));
    };
    inquiry("cad.inquiry.list", "LIST", {"LI", "LS"}, "List",
            "Say what the selected objects are, in the history",
            [] { return std::make_unique<ListCommand>(); });
    inquiry("cad.inquiry.describe", "DESCRIBE", {"DESC"}, "Describe",
            "Say what the drawing shows: rooms, systems, shell and title block",
            [] { return std::make_unique<DescribeCommand>(); });
    {
        // Measure and Divide place points, so they change the drawing and
        // are not inquiries -- but they belong with them on the ribbon.
        CommandDefinition measure = makeDefinition(
            "cad.tools.measure", "MEASURE", {"ME"}, "Measure",
            "Place points along an object every given distance",
            [] { return std::make_unique<MarkAlongCommand>(false); }, true);
        measure.category = "Tools";
        commands.push_back(std::move(measure));
        CommandDefinition divide = makeDefinition(
            "cad.tools.divide", "DIVIDE", {"DIV"}, "Divide",
            "Place points that split an object into equal parts",
            [] { return std::make_unique<MarkAlongCommand>(true); }, true);
        divide.category = "Tools";
        commands.push_back(std::move(divide));
    }
    inquiry("cad.inquiry.dist", "DIST", {"DI"}, "Distance", "Measure between two points",
            [] { return std::make_unique<DistanceCommand>(); });
    inquiry("cad.inquiry.area", "AREA", {"AA"}, "Area",
            "Measure the area and perimeter of a shape",
            [] { return std::make_unique<AreaCommand>(); });
    inquiry("cad.inquiry.id", "ID", {}, "Identify Point", "Report the coordinates of a point",
            [] { return std::make_unique<IdentifyCommand>(); });
    commands.push_back(makeDefinition(
        "cad.modify.matchprop", "MATCHPROP", {"MA", "PAINTER"}, "Match Properties",
        "Copy layer, colour and linetype from one object to others",
        [] { return std::make_unique<MatchPropertiesCommand>(); }, false));
    commands.push_back(makeDefinition("cad.modify.array", "ARRAY", {"AR"}, "Array",
                                      "Repeat objects in rows and columns, or around a centre",
                                      [] { return std::make_unique<ArrayCommand>(); }));
    commands.push_back(makeDefinition("cad.modify.offset", "OFFSET", {"O"}, "Offset",
                                      "Create a parallel copy at a distance",
                                      [] { return std::make_unique<OffsetCommand>(); }, false));
    commands.push_back(makeDefinition("cad.modify.trim", "TRIM", {"TR"}, "Trim",
                                      "Cut objects back to a cutting edge",
                                      [] { return std::make_unique<TrimCommand>(); }, false));
    commands.push_back(makeDefinition("cad.modify.extend", "EXTEND", {"EX"}, "Extend",
                                      "Lengthen objects to meet a boundary",
                                      [] { return std::make_unique<ExtendCommand>(); }, false));
    commands.push_back(makeDefinition("cad.modify.fillet", "FILLET", {"F"}, "Fillet",
                                      "Round the corner between two objects",
                                      [] { return std::make_unique<FilletCommand>(); }, false));

    {
        CommandDefinition undo;
        undo.id = "cad.edit.undo";
        undo.name = "UNDO";
        undo.aliases = {"U"};
        undo.title = "Undo";
        undo.description = "Reverse the last operation";
        undo.category = "Edit";
        undo.transactionMode = CommandTransactionMode::Manual;
        undo.repeatable = false;
        undo.create = [] { return std::make_unique<UndoCommand>(); };
        commands.push_back(std::move(undo));
    }
    {
        CommandDefinition redo;
        redo.id = "cad.edit.redo";
        redo.name = "REDO";
        redo.title = "Redo";
        redo.description = "Re-apply the last undone operation";
        redo.category = "Edit";
        redo.transactionMode = CommandTransactionMode::Manual;
        redo.repeatable = false;
        redo.create = [] { return std::make_unique<RedoCommand>(); };
        commands.push_back(std::move(redo));
    }

    return commands;
}

std::unique_ptr<CommandRegistry> createStandardRegistry() {
    auto registry = std::make_unique<CommandRegistry>();
    registry->registerAll(drawCommands()).release();
    registry->registerAll(modifyCommands()).release();
    registry->registerAll(reshapeCommands()).release();
    registry->registerAll(textCommands()).release();
    registry->registerAll(fieldCommands()).release();
    registry->registerAll(blockCommands()).release();
    registry->registerAll(layerCommands()).release();
    registry->registerAll(annotationCommands()).release();
    registry->registerAll(layoutCommands()).release();
    registry->registerAll(attributeCommands()).release();
    registry->registerAll(systemCommands()).release();
    registry->registerAll(likeCommands()).release();
    registry->registerAll(clipCommands()).release();
    registry->registerAll(purgeCommands()).release();
    return registry;
}

}  // namespace cad
