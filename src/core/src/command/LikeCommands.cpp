// LIKE: draw another one like that one.
//
// Every office draws its things its own way -- a duct run as one wide line
// or two, a thermostat as a letter in a circle, a fixture as a block -- and
// the way is already in the drawing. This takes an example and what is new
// about the copy, the route or the point and the text, and makes a new
// object in the example's style: layer, colour, linetype, width, the second
// line of a double-line run, the tag beside it and how the tag sits. Nothing
// here knows what a duct is; it knows what the example looks like, which is
// what lets it serve every discipline with one command.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cad/command/StandardCommands.hpp"
#include "cad/db/BlockTableRecord.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/Layer.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/geom/Operations.hpp"
#include "cad/geom/Polyline.hpp"
#include "cad/math/Angle.hpp"

namespace cad {
namespace {

/// The look of an entity, to give to a new one.
void copyLook(const Entity& from, Entity& to) {
    to.layerHandle = from.layerHandle;
    to.color = from.color;
    to.linetypeHandle = from.linetypeHandle;
    to.linetypeScale = from.linetypeScale;
    to.lineweight = from.lineweight;
}

[[nodiscard]] double widthOf(const PolylineEntity& polyline) {
    double width = polyline.constantWidth;
    for (const PolylineVertex& vertex : polyline.vertices) {
        width = std::max({width, vertex.startWidth, vertex.endWidth});
    }
    return width;
}

/// The line of a double-line run that pairs with `example`: on the same
/// layer, the same length within a fifth, running parallel a short way off.
/// Returns the distance between them, or nothing.
[[nodiscard]] std::optional<double> doubleLineSpacing(const Database& db, const Entity& example,
                                                      const Curve& curve) {
    const double length = curve.length();
    if (length <= 0.0) {
        return std::nullopt;
    }
    const Vec3 mid = curve.midPoint();
    const Vec3 tangent = curve.tangentAt(0.5).normalized();
    std::optional<double> best;
    for (const Entity* other : db.entitiesIn(const_cast<Database&>(db).modelSpace())) {
        if (other == nullptr || other->erased || other->handle == example.handle ||
            other->layerHandle != example.layerHandle) {
            continue;
        }
        if (dynamic_cast<const PolylineEntity*>(other) == nullptr &&
            dynamic_cast<const LineEntity*>(other) == nullptr) {
            continue;
        }
        for (const CurvePtr& candidate : other->toCurves()) {
            if (candidate == nullptr) {
                continue;
            }
            const double otherLength = candidate->length();
            if (otherLength < length * 0.8 || otherLength > length * 1.25) {
                continue;
            }
            const ClosestPointResult nearest = candidate->closestPoint(mid);
            const Vec3 otherTangent = candidate->tangentAt(nearest.param).normalized();
            if (std::abs(tangent.dot(otherTangent)) < 0.98) {
                continue;
            }
            const double distance = nearest.distance;
            if (distance <= 1e-9 || distance > length) {
                continue;
            }
            if (!best.has_value() || distance < *best) {
                best = distance;
            }
        }
    }
    return best;
}

/// The tag beside a run: the nearest text within a few text heights of
/// the run's middle, on any layer, that is not the run's own annotation
/// elsewhere. Returns how it sits relative to the run: its offset to the
/// left, its shift along, and the text itself.
struct TagStyle {
    const TextEntity* text = nullptr;
    double left = 0.0;
    double along = 0.0;
};

[[nodiscard]] TagStyle tagBeside(const Database& db, const Curve& curve, double halfWidth) {
    TagStyle style;
    const Vec3 mid = curve.midPoint();
    const Vec3 tangent = curve.tangentAt(0.5).normalized();
    const Vec3 leftward{-tangent.y, tangent.x, 0.0};
    double bestDistance = std::numeric_limits<double>::infinity();
    for (const Entity* entity : db.entitiesIn(const_cast<Database&>(db).modelSpace())) {
        const auto* text = dynamic_cast<const TextEntity*>(entity);
        if (text == nullptr || text->erased || dynamic_cast<const AttributeEntity*>(entity) != nullptr) {
            continue;
        }
        const Vec3 centre = text->bounds().center();
        const double distance = curve.distanceTo(centre);
        if (distance > halfWidth + 4.0 * text->height || distance >= bestDistance) {
            continue;
        }
        // Along the run, not off its end.
        const ClosestPointResult nearest = curve.closestPoint(centre);
        if (nearest.param <= 0.0 || nearest.param >= 1.0) {
            continue;
        }
        bestDistance = distance;
        style.text = text;
        const Vec3 d = text->position - mid;
        style.left = d.dot(leftward);
        style.along = d.dot(tangent);
    }
    return style;
}

/// LIKE: an example, then what is new about the copy.
///
/// For a run (a polyline, line or arc): the text for its tag, if it has
/// one, then the route's points until Enter. For a block, a circle with a
/// letter in it, or a piece of text: the text, if there is any to change,
/// then the point.
class LikeCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& context) override {
        // A picked object before the command is the example.
        if (context.initialSelection().size() == 1) {
            return takeExample(context.initialSelection().front(), context);
        }
        InputRequest request;
        request.kind = InputRequest::Kind::Entity;
        request.prompt = "Select the example to draw another like";
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }
        switch (stage_) {
            case Stage::Example:
                if (input.kind != InputValue::Kind::Entity) {
                    return StepResult::cancelled();
                }
                return takeExample(input.entity, context);
            case Stage::Text: {
                if (input.kind == InputValue::Kind::Text || input.kind == InputValue::Kind::Keyword) {
                    newText_ = input.text;
                } else if (input.kind == InputValue::Kind::Number) {
                    newText_ = input.text.empty() ? std::to_string(input.number) : input.text;
                } else if (input.kind == InputValue::Kind::Point) {
                    // No text given: the point came straight away.
                    return takePoint(input.point, context);
                } else if (input.kind != InputValue::Kind::Empty) {
                    return StepResult::cancelled();
                }
                stage_ = Stage::Points;
                return askPoint();
            }
            case Stage::Points:
            default:
                if (input.kind == InputValue::Kind::Point) {
                    return takePoint(input.point, context);
                }
                if (input.kind != InputValue::Kind::Empty) {
                    return StepResult::cancelled();
                }
                if (isRun_) {
                    if (route_.size() < 2) {
                        return StepResult::cancelled("A run needs at least two points.");
                    }
                    return drawRun(context);
                }
                return StepResult::cancelled();
        }
    }

private:
    enum class Stage : std::uint8_t { Example, Text, Points };

    StepResult takeExample(Handle handle, CommandContext& context) {
        const Entity* entity = context.db().getEntity(handle);
        if (entity == nullptr || entity->erased) {
            return StepResult::failed("There is no object with handle " + handle.toString() + ".");
        }
        example_ = handle;
        isRun_ = dynamic_cast<const PolylineEntity*>(entity) != nullptr ||
                 dynamic_cast<const LineEntity*>(entity) != nullptr ||
                 dynamic_cast<const ArcEntity*>(entity) != nullptr;
        const bool placeable = isRun_ || dynamic_cast<const InsertEntity*>(entity) != nullptr ||
                               dynamic_cast<const CircleEntity*>(entity) != nullptr ||
                               dynamic_cast<const TextEntity*>(entity) != nullptr ||
                               dynamic_cast<const MTextEntity*>(entity) != nullptr;
        if (!placeable) {
            return StepResult::failed("LIKE copies runs, blocks, circles and text; not a " +
                                      entity->objectType() + ".");
        }
        stage_ = Stage::Text;
        InputRequest request;
        request.kind = InputRequest::Kind::Text;
        request.prompt = isRun_ ? "Enter the tag for the new run, or Enter to keep the example's"
                                : "Enter the text for the new one, or Enter to keep the example's";
        request.allowEmpty = true;
        return StepResult::ask(request);
    }

    [[nodiscard]] StepResult askPoint() const {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        if (isRun_) {
            request.prompt = route_.empty() ? "Specify start of run"
                                            : "Specify next point, or Enter to finish";
            if (!route_.empty()) {
                request.basePoint = route_.back();
            }
            request.allowEmpty = !route_.empty();
        } else {
            request.prompt = "Specify where to place it";
        }
        return StepResult::ask(request);
    }

    StepResult takePoint(const Vec3& point, CommandContext& context) {
        stage_ = Stage::Points;
        if (!isRun_) {
            return place(point, context);
        }
        if (route_.empty() || !route_.back().equals(point, Tolerance{1e-9})) {
            route_.push_back(point);
        }
        return askPoint();
    }

    /// A new run in the example's style along the route.
    StepResult drawRun(CommandContext& context) {
        Database& db = context.db();
        const Entity* example = db.getEntity(example_);
        if (example == nullptr) {
            return StepResult::failed("The example is gone.");
        }
        const std::vector<CurvePtr> exampleCurves = example->toCurves();
        if (exampleCurves.empty() || exampleCurves.front() == nullptr) {
            return StepResult::failed("The example has no geometry to copy.");
        }
        const Curve& exampleCurve = *exampleCurves.front();

        double width = 0.0;
        if (const auto* polyline = dynamic_cast<const PolylineEntity*>(example)) {
            width = widthOf(*polyline);
        }
        const std::optional<double> spacing =
            width > 0.0 ? std::nullopt : doubleLineSpacing(db, *example, exampleCurve);
        int made = 0;

        const Polyline centre = PolylineEntity::fromPoints(route_).geometry();
        if (spacing.has_value()) {
            for (const double side : {*spacing / 2.0, -*spacing / 2.0}) {
                CurvePtr offset = offsetCurve(centre, side);
                const auto* poly = dynamic_cast<const Polyline*>(offset.get());
                if (poly == nullptr) {
                    continue;
                }
                auto entity = std::make_unique<PolylineEntity>(PolylineEntity::fromPolyline(*poly));
                copyLook(*example, *entity);
                context.transaction().addToCurrentSpace(std::move(entity));
                ++made;
            }
        } else {
            auto entity = std::make_unique<PolylineEntity>(PolylineEntity::fromPoints(route_));
            entity->constantWidth = width;
            copyLook(*example, *entity);
            context.transaction().addToCurrentSpace(std::move(entity));
            ++made;
        }

        // The tag as the example has it: same text style, height, layer and
        // placing relative to the run, on the longest leg of the new one.
        const double halfWidth = spacing.has_value() ? *spacing / 2.0 : width / 2.0;
        const TagStyle tag = tagBeside(db, exampleCurve, halfWidth);
        std::string summary = "Drawn like " + example_.toString() + ": " +
                              std::to_string(route_.size() - 1) + " leg" +
                              (route_.size() == 2 ? "" : "s");
        if (tag.text != nullptr) {
            std::size_t longest = 0;
            double longestLength = -1.0;
            for (std::size_t i = 0; i + 1 < route_.size(); ++i) {
                const double length = route_[i].distanceTo(route_[i + 1]);
                if (length > longestLength) {
                    longestLength = length;
                    longest = i;
                }
            }
            const Vec3 a = route_[longest];
            const Vec3 b = route_[longest + 1];
            Vec3 along = (b - a).normalized();
            double angle = std::atan2(along.y, along.x);
            if (angle > kPi / 2.0 + 1e-9 || angle < -kPi / 2.0 + 1e-9) {
                angle += kPi;
                along = along * -1.0;
            }
            const Vec3 leftward{-along.y, along.x, 0.0};
            // The example's tag may sit on the far side when its run ran
            // the other way; the offset's size is kept, its side is what
            // reads upright.
            const Vec3 at = a.midpoint(b) + leftward * std::abs(tag.left) + along * tag.along;
            auto text = std::make_unique<TextEntity>(at, newText_.empty() ? tag.text->contents : newText_,
                                                     tag.text->height);
            text->rotation = angle;
            text->widthFactor = tag.text->widthFactor;
            text->styleHandle = tag.text->styleHandle;
            copyLook(*tag.text, *text);
            context.transaction().addToCurrentSpace(std::move(text));
            summary += ", tagged " + (newText_.empty() ? tag.text->contents : newText_);
        } else if (!newText_.empty()) {
            summary += " (the example has no tag to copy, so none was made)";
        }
        return StepResult::done(summary);
    }

    /// A new block, circle-and-letter or text in the example's style at a point.
    StepResult place(const Vec3& where, CommandContext& context) {
        Database& db = context.db();
        const Entity* example = db.getEntity(example_);
        if (example == nullptr) {
            return StepResult::failed("The example is gone.");
        }
        if (const auto* insert = dynamic_cast<const InsertEntity*>(example)) {
            auto copy = std::make_unique<InsertEntity>(insert->blockHandle, where);
            copy->scale = insert->scale;
            copy->rotation = insert->rotation;
            copyLook(*example, *copy);
            InsertEntity& placed =
                static_cast<InsertEntity&>(context.transaction().addToCurrentSpace(std::move(copy)));
            // Attributes come across with their values; the new text, if
            // any, goes into the first one.
            bool first = true;
            for (const Handle handle : insert->attributeHandles) {
                const auto* attribute = db.get<AttributeEntity>(handle);
                if (attribute == nullptr || attribute->erased) {
                    continue;
                }
                auto value = std::make_unique<AttributeEntity>();
                value->tag = attribute->tag;
                value->flags = attribute->flags;
                value->contents = first && !newText_.empty() ? newText_ : attribute->contents;
                value->height = attribute->height;
                value->rotation = attribute->rotation;
                value->widthFactor = attribute->widthFactor;
                value->styleHandle = attribute->styleHandle;
                value->horizontalAlignment = attribute->horizontalAlignment;
                value->verticalAlignment = attribute->verticalAlignment;
                value->position = where + (attribute->position - insert->position);
                if (attribute->alignmentPoint.has_value()) {
                    value->alignmentPoint = where + (*attribute->alignmentPoint - insert->position);
                }
                copyLook(*attribute, *value);
                const Handle added = context.transaction().addToCurrentSpace(std::move(value)).handle;
                placed.attributeHandles.push_back(added);
                first = false;
            }
            const auto* block = db.get<BlockTableRecord>(insert->blockHandle);
            return StepResult::done("Placed " + (block == nullptr ? std::string{"block"} : block->name) +
                                    " like " + example_.toString());
        }
        if (const auto* circle = dynamic_cast<const CircleEntity*>(example)) {
            auto copy = std::make_unique<CircleEntity>(where, circle->radius);
            copyLook(*example, *copy);
            context.transaction().addToCurrentSpace(std::move(copy));
            // The letter inside, if the example is a symbol.
            const TextEntity* letter = nullptr;
            for (const Entity* entity : db.entitiesIn(db.modelSpace())) {
                const auto* text = dynamic_cast<const TextEntity*>(entity);
                if (text == nullptr || text->erased || text->contents.size() > 2) {
                    continue;
                }
                if (text->bounds().center().distanceTo(circle->center) <= circle->radius) {
                    letter = text;
                    break;
                }
            }
            if (letter != nullptr) {
                auto text = std::make_unique<TextEntity>(where + (letter->position - circle->center),
                                                         newText_.empty() ? letter->contents : newText_,
                                                         letter->height);
                text->rotation = letter->rotation;
                text->widthFactor = letter->widthFactor;
                text->styleHandle = letter->styleHandle;
                text->horizontalAlignment = letter->horizontalAlignment;
                text->verticalAlignment = letter->verticalAlignment;
                if (letter->alignmentPoint.has_value()) {
                    text->alignmentPoint = where + (*letter->alignmentPoint - circle->center);
                }
                copyLook(*letter, *text);
                context.transaction().addToCurrentSpace(std::move(text));
                return StepResult::done("Placed a " + (newText_.empty() ? letter->contents : newText_) +
                                        " in a circle like " + example_.toString());
            }
            return StepResult::done("Placed a circle like " + example_.toString());
        }
        if (const auto* text = dynamic_cast<const TextEntity*>(example)) {
            auto copy = std::make_unique<TextEntity>(where, newText_.empty() ? text->contents : newText_,
                                                     text->height);
            copy->rotation = text->rotation;
            copy->widthFactor = text->widthFactor;
            copy->styleHandle = text->styleHandle;
            copy->horizontalAlignment = text->horizontalAlignment;
            copy->verticalAlignment = text->verticalAlignment;
            if (text->alignmentPoint.has_value()) {
                copy->alignmentPoint = where + (*text->alignmentPoint - text->position);
            }
            copyLook(*example, *copy);
            context.transaction().addToCurrentSpace(std::move(copy));
            return StepResult::done("Placed text like " + example_.toString());
        }
        if (const auto* mtext = dynamic_cast<const MTextEntity*>(example)) {
            auto copy = std::make_unique<MTextEntity>(where, newText_.empty() ? mtext->contents : newText_,
                                                      mtext->height);
            copy->width = mtext->width;
            copy->rotation = mtext->rotation;
            copy->attachment = mtext->attachment;
            copy->styleHandle = mtext->styleHandle;
            copy->lineSpacingFactor = mtext->lineSpacingFactor;
            copyLook(*example, *copy);
            context.transaction().addToCurrentSpace(std::move(copy));
            return StepResult::done("Placed a note like " + example_.toString());
        }
        return StepResult::failed("LIKE cannot copy a " + example->objectType() + ".");
    }

    Stage stage_ = Stage::Example;
    Handle example_;
    bool isRun_ = false;
    std::string newText_;
    std::vector<Vec3> route_;
};

}  // namespace

std::vector<CommandDefinition> likeCommands() {
    std::vector<CommandDefinition> commands;
    CommandDefinition like;
    like.id = "cad.draw.like";
    like.name = "LIKE";
    like.aliases = {"SAME"};
    like.title = "Draw Like";
    like.description = "Draw another one like an example: its layer, width, tag and style";
    like.category = "Draw";
    like.supportsPickfirst = true;
    like.create = [] { return std::make_unique<LikeCommand>(); };
    commands.push_back(std::move(like));
    return commands;
}

}  // namespace cad
