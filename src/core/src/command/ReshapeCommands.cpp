/// CHAMFER, BREAK, JOIN, EXPLODE and STRETCH.
///
/// The Tier 3 commands that change an object's shape, or change how many
/// objects there are. Kept apart from ModifyCommands.cpp — which already runs
/// to eight hundred lines of transforms, trim and fillet — rather than making
/// one file the home of everything that edits.
///
/// Two of these are only commands: `chamferLines` and `breakCurve` were already
/// implemented and tested in the geometry layer with nothing exposing them.

#include <algorithm>
#include <cctype>
#include <limits>

#include "cad/command/StandardCommands.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/Layer.hpp"
#include "cad/geom/Operations.hpp"
#include "cad/math/Angle.hpp"

namespace cad {
namespace {

constexpr double kJoinTolerance = 1e-7;

[[nodiscard]] StepResult askPoint(std::string prompt, bool allowEmpty = false) {
    InputRequest request;
    request.kind = InputRequest::Kind::Point;
    request.prompt = std::move(prompt);
    request.allowEmpty = allowEmpty;
    return StepResult::ask(request);
}

[[nodiscard]] StepResult askSelection(std::string prompt) {
    InputRequest request;
    request.kind = InputRequest::Kind::Selection;
    request.prompt = std::move(prompt);
    request.minCount = 1;
    return StepResult::ask(request);
}

/// True when the entity may not be edited because its layer is locked.
[[nodiscard]] bool isLocked(const Entity& entity, CommandContext& context) {
    const Layer* layer = context.db().get<Layer>(entity.layerHandle);
    return layer != nullptr && layer->isLocked;
}

/// Replace an entity with the curves an edit produced, keeping its appearance.
///
/// Returns how many replacements were added. Erasing the original only after
/// the pieces are added keeps the operation a single undo step that never
/// passes through a state with the geometry missing.
std::size_t replaceWithCurves(Entity& original, const std::vector<CurvePtr>& pieces,
                              CommandContext& context) {
    std::size_t added = 0;
    for (const CurvePtr& piece : pieces) {
        if (piece == nullptr) {
            continue;
        }
        std::unique_ptr<Entity> replacement = entityFromCurve(*piece);
        if (replacement == nullptr) {
            continue;
        }
        copyProperties(original, *replacement);

        BlockTableRecord* owner = context.db().get<BlockTableRecord>(original.ownerHandle);
        context.transaction().addEntity(std::move(replacement),
                                        owner != nullptr ? *owner : context.db().modelSpace());
        ++added;
    }
    if (added > 0) {
        context.transaction().erase(original);
    }
    return added;
}

// ------------------------------------------------------------------- chamfer

/// CHAMFER — cut the corner between two lines with a straight bevel.
///
/// Two distances rather than one, as other CAD software has: an equal chamfer is the
/// common case but an unequal one is what a drafter reaches for on a sheet
/// metal detail, and offering only the symmetric version is the kind of
/// simplification the product is meant to avoid.
class ChamferCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& /*context*/) override {
        InputRequest request;
        request.kind = InputRequest::Kind::Distance;
        request.prompt = "Specify first chamfer distance";
        request.defaultNumber = 0.0;
        request.minimum = 0.0;
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }

        if (!hasFirstDistance_) {
            if (input.kind == InputValue::Kind::Number) {
                if (input.number < 0.0) {
                    return StepResult::failed("Chamfer distance cannot be negative.");
                }
                distanceA_ = input.number;
            }
            hasFirstDistance_ = true;

            InputRequest request;
            request.kind = InputRequest::Kind::Distance;
            request.prompt = "Specify second chamfer distance";
            // Defaulting the second to the first makes the symmetric case a
            // single keystroke while leaving the asymmetric one available.
            request.defaultNumber = distanceA_;
            request.minimum = 0.0;
            return StepResult::ask(request);
        }

        if (!hasSecondDistance_) {
            distanceB_ = input.kind == InputValue::Kind::Number ? input.number : distanceA_;
            if (distanceB_ < 0.0) {
                return StepResult::failed("Chamfer distance cannot be negative.");
            }
            hasSecondDistance_ = true;
            return askPoint("Select first line");
        }

        if (input.kind != InputValue::Kind::Point) {
            return StepResult::cancelled();
        }

        Entity* picked = nearestEntityAt(input.point, context);
        if (picked == nullptr) {
            context.warn("No object found at that point.");
            return askPoint(firstHandle_.isNull() ? "Select first line" : "Select second line");
        }

        if (firstHandle_.isNull()) {
            firstHandle_ = picked->handle;
            firstPick_ = input.point;
            return askPoint("Select second line");
        }
        if (picked->handle == firstHandle_) {
            context.warn("Select a different object for the second edge.");
            return askPoint("Select second line");
        }

        Entity* first = context.db().getEntity(firstHandle_);
        if (first == nullptr) {
            return StepResult::cancelled();
        }
        if (isLocked(*first, context) || isLocked(*picked, context)) {
            return StepResult::failed("Cannot chamfer an object on a locked layer.");
        }

        const std::vector<CurvePtr> curvesA = first->toCurves();
        const std::vector<CurvePtr> curvesB = picked->toCurves();
        if (curvesA.empty() || curvesB.empty()) {
            return StepResult::failed("Those objects cannot be chamfered.");
        }
        const auto* lineA = dynamic_cast<const LineSegment*>(curvesA.front().get());
        const auto* lineB = dynamic_cast<const LineSegment*>(curvesB.front().get());
        if (lineA == nullptr || lineB == nullptr) {
            return StepResult::failed("CHAMFER currently works between two lines.");
        }

        const auto result =
            chamferLines(*lineA, *lineB, distanceA_, distanceB_, firstPick_, input.point);
        if (!result) {
            return StepResult::failed(
                "Cannot chamfer those lines with those distances. They may be parallel, or "
                "too short for the distances given.");
        }

        // A zero-by-zero chamfer is a corner join, exactly as it is for FILLET,
        // and adding a degenerate bevel line would leave litter behind.
        if (distanceA_ > 0.0 || distanceB_ > 0.0) {
            auto bevel = std::make_unique<LineEntity>(result->line.start, result->line.end);
            copyProperties(*first, *bevel);
            context.transaction().addToCurrentSpace(std::move(bevel));
        }

        replaceWithCurves(*first, single(result->trimmedA), context);
        replaceWithCurves(*picked, single(result->trimmedB), context);
        return StepResult::done();
    }

private:
    [[nodiscard]] static std::vector<CurvePtr> single(const LineSegment& line) {
        std::vector<CurvePtr> out;
        out.push_back(line.clone());
        return out;
    }

    double distanceA_ = 0.0;
    double distanceB_ = 0.0;
    bool hasFirstDistance_ = false;
    bool hasSecondDistance_ = false;
    Handle firstHandle_;
    Vec3 firstPick_;
};

// --------------------------------------------------------------------- break

/// BREAK — split a curve, discarding the span between two points.
///
/// Pressing Enter at the second point breaks *at* the first instead, keeping
/// both halves. That is other CAD software's BREAK AT POINT, and it is the same geometry
/// call with the second point omitted rather than a separate command.
class BreakCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& /*context*/) override {
        return askPoint("Select object to break");
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }

        if (handle_.isNull()) {
            if (input.kind != InputValue::Kind::Point) {
                return StepResult::cancelled();
            }
            Entity* picked = nearestEntityAt(input.point, context);
            if (picked == nullptr) {
                context.warn("No object found at that point.");
                return askPoint("Select object to break");
            }
            if (isLocked(*picked, context)) {
                return StepResult::failed("Cannot break an object on a locked layer.");
            }
            handle_ = picked->handle;
            // The pick doubles as the default first break point, which is how
            // BREAK is normally used: one click, then one more.
            firstPoint_ = input.point;
            return askPoint("Specify first break point", true);
        }

        if (!hasFirstPoint_) {
            if (input.kind == InputValue::Kind::Point) {
                firstPoint_ = input.point;
            }
            hasFirstPoint_ = true;
            return askPoint("Specify second break point, or Enter to break at the first", true);
        }

        std::optional<Vec3> secondPoint;
        if (input.kind == InputValue::Kind::Point) {
            secondPoint = input.point;
        }

        Entity* entity = context.db().getEntity(handle_);
        if (entity == nullptr || entity->erased) {
            return StepResult::cancelled();
        }
        const std::vector<CurvePtr> curves = entity->toCurves();
        if (curves.empty()) {
            return StepResult::failed("That object cannot be broken.");
        }

        const std::vector<CurvePtr> pieces = breakCurve(*curves.front(), firstPoint_, secondPoint);
        if (pieces.empty()) {
            return StepResult::failed(
                "Nothing to break there — the points may be off the object, or at its ends.");
        }
        if (replaceWithCurves(*entity, pieces, context) == 0) {
            return StepResult::failed("That object cannot be broken.");
        }
        return StepResult::done(pieces.size() == 1 ? "Object broken"
                                                   : "Object broken into 2 pieces");
    }

private:
    Handle handle_;
    Vec3 firstPoint_;
    bool hasFirstPoint_ = false;
};

// ---------------------------------------------------------------------- join

/// One chain of curves running end to end, and whether it closes on itself.
struct Chain {
    std::vector<CurvePtr> curves;
    bool closed = false;
};

/// Walk loose curves into chains, reversing them where needed.
///
/// Greedy: each chain grows from whichever curve is left until nothing else
/// touches either end. That is enough for JOIN, where the user has already said
/// which objects belong together by selecting them.
[[nodiscard]] std::vector<Chain> chainCurves(std::vector<CurvePtr> curves) {
    const Tolerance tolerance{kJoinTolerance};
    std::vector<Chain> chains;

    while (!curves.empty()) {
        Chain chain;
        chain.curves.push_back(std::move(curves.back()));
        curves.pop_back();

        bool grew = true;
        while (grew) {
            grew = false;
            const Vec3 head = chain.curves.front()->startPoint();
            const Vec3 tail = chain.curves.back()->endPoint();

            if (head.equals(tail, tolerance) && chain.curves.size() > 1) {
                chain.closed = true;
                break;
            }

            for (std::size_t i = 0; i < curves.size(); ++i) {
                const Vec3 start = curves[i]->startPoint();
                const Vec3 end = curves[i]->endPoint();

                if (end.equals(head, tolerance)) {
                    chain.curves.insert(chain.curves.begin(), std::move(curves[i]));
                } else if (start.equals(head, tolerance)) {
                    chain.curves.insert(chain.curves.begin(), curves[i]->reversed());
                } else if (start.equals(tail, tolerance)) {
                    chain.curves.push_back(std::move(curves[i]));
                } else if (end.equals(tail, tolerance)) {
                    chain.curves.push_back(curves[i]->reversed());
                } else {
                    continue;
                }
                curves.erase(curves.begin() + static_cast<std::ptrdiff_t>(i));
                grew = true;
                break;
            }
        }
        chains.push_back(std::move(chain));
    }
    return chains;
}

/// Build a polyline from an ordered chain, carrying arcs across as bulges.
///
/// Returns nothing when the chain holds a curve a polyline cannot express —
/// an ellipse or spline — because a polyline that quietly straightened one
/// would be worse than refusing.
[[nodiscard]] std::optional<Polyline> polylineFromChain(const Chain& chain) {
    std::vector<PolylineVertex> vertices;
    vertices.reserve(chain.curves.size() + 1);

    for (const CurvePtr& curve : chain.curves) {
        double bulge = 0.0;
        if (const auto* arc = dynamic_cast<const Arc*>(curve.get())) {
            bulge = arc->bulge();
        } else if (dynamic_cast<const LineSegment*>(curve.get()) == nullptr) {
            return std::nullopt;
        }
        vertices.emplace_back(curve->startPoint(), bulge);
    }

    if (!chain.closed) {
        vertices.emplace_back(chain.curves.back()->endPoint());
    }
    if (vertices.size() < 2) {
        return std::nullopt;
    }
    return Polyline{std::move(vertices), chain.closed};
}

/// JOIN — merge touching lines, arcs and polylines into single polylines.
class JoinCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& context) override {
        if (!context.initialSelection().empty()) {
            return join(context.initialSelection(), context);
        }
        return askSelection("Select objects to join");
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind != InputValue::Kind::Selection || input.selection.empty()) {
            return StepResult::cancelled();
        }
        return join(input.selection, context);
    }

private:
    static StepResult join(const std::vector<Handle>& handles, CommandContext& context) {
        std::vector<Entity*> sources;
        std::vector<CurvePtr> curves;

        for (const Handle handle : handles) {
            Entity* entity = context.db().getEntity(handle);
            if (entity == nullptr || entity->erased) {
                continue;
            }
            if (isLocked(*entity, context)) {
                context.warn("Skipped an object on a locked layer.");
                continue;
            }
            std::vector<CurvePtr> entityCurves = entity->toCurves();
            if (entityCurves.empty()) {
                continue;
            }
            sources.push_back(entity);
            for (CurvePtr& curve : entityCurves) {
                curves.push_back(std::move(curve));
            }
        }

        if (sources.size() < 2) {
            return StepResult::failed("Select at least two objects that meet end to end.");
        }

        const std::vector<Chain> chains = chainCurves(std::move(curves));
        int joined = 0;
        for (const Chain& chain : chains) {
            if (chain.curves.size() < 2) {
                continue;
            }
            const std::optional<Polyline> polyline = polylineFromChain(chain);
            if (!polyline) {
                context.warn("Skipped a chain containing geometry a polyline cannot hold.");
                continue;
            }
            auto entity = std::make_unique<PolylineEntity>(
                PolylineEntity::fromPolyline(*polyline));
            copyProperties(*sources.front(), *entity);
            context.transaction().addToCurrentSpace(std::move(entity));
            ++joined;
        }

        if (joined == 0) {
            return StepResult::failed("Those objects do not meet end to end.");
        }
        for (Entity* entity : sources) {
            context.transaction().erase(*entity);
        }
        return StepResult::done(joined == 1 ? "Joined into 1 polyline"
                                            : "Joined into " + std::to_string(joined) +
                                                  " polylines");
    }
};

// ------------------------------------------------------------------- explode

/// EXPLODE — replace compound objects with their parts.
///
/// One level deep, as other CAD software does: a block containing blocks explodes into
/// those blocks rather than all the way to lines. Exploding recursively would
/// discard the nesting the drawing was built with, and it cannot be undone by
/// re-blocking.
class ExplodeCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& context) override {
        if (!context.initialSelection().empty()) {
            return explode(context.initialSelection(), context);
        }
        return askSelection("Select objects to explode");
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind != InputValue::Kind::Selection || input.selection.empty()) {
            return StepResult::cancelled();
        }
        return explode(input.selection, context);
    }

private:
    static StepResult explode(const std::vector<Handle>& handles, CommandContext& context) {
        int exploded = 0;
        int parts = 0;
        int refused = 0;

        for (const Handle handle : handles) {
            Entity* entity = context.db().getEntity(handle);
            if (entity == nullptr || entity->erased) {
                continue;
            }
            if (isLocked(*entity, context)) {
                context.warn("Skipped an object on a locked layer.");
                continue;
            }

            int added = 0;
            if (auto* insert = dynamic_cast<InsertEntity*>(entity)) {
                added = explodeInsert(*insert, context);
            } else if (dynamic_cast<PolylineEntity*>(entity) != nullptr) {
                added = explodePolyline(*entity, context);
            } else {
                ++refused;
                continue;
            }

            if (added > 0) {
                context.transaction().erase(*entity);
                ++exploded;
                parts += added;
            }
        }

        if (refused > 0 && exploded == 0) {
            return StepResult::failed("Those objects cannot be exploded.");
        }
        if (exploded == 0) {
            return StepResult::cancelled("Nothing was exploded");
        }
        if (refused > 0) {
            context.warn(std::to_string(refused) + " object(s) could not be exploded.");
        }
        return StepResult::done(std::to_string(exploded) + " object(s) exploded into " +
                                std::to_string(parts) + " part(s)");
    }

    /// Copy a block's contents into the insert's space, under its transform.
    ///
    /// Every cell of an array insert is expanded, because each is a real
    /// placement rather than a display repeat.
    static int explodeInsert(InsertEntity& insert, CommandContext& context) {
        const auto* definition = context.db().get<BlockTableRecord>(insert.blockHandle);
        if (definition == nullptr) {
            context.warn("Skipped a block reference whose definition is missing.");
            return 0;
        }

        BlockTableRecord* owner = context.db().get<BlockTableRecord>(insert.ownerHandle);
        BlockTableRecord& target = owner != nullptr ? *owner : context.db().modelSpace();

        int added = 0;
        for (const Mat4& placement : insert.cellTransforms()) {
            for (const Entity* source : context.db().entitiesIn(*definition)) {
                if (source->erased) {
                    continue;
                }
                std::unique_ptr<Entity> owned = cloneEntity(*source);
                if (owned == nullptr) {
                    continue;
                }
                owned->transformBy(placement);
                // A block's contents may be on layer 0, meaning "the layer the
                // reference sits on". Once exploded there is no reference left
                // to inherit from, so it is resolved here rather than losing it.
                if (owned->layerHandle.isNull()) {
                    owned->layerHandle = insert.layerHandle;
                }
                context.transaction().addEntity(std::move(owned), target);
                ++added;
            }
        }
        return added;
    }

    /// Split a polyline into the lines and arcs it is made of.
    static int explodePolyline(Entity& entity, CommandContext& context) {
        const std::vector<CurvePtr> curves = entity.toCurves();
        if (curves.empty()) {
            return 0;
        }
        const auto* polyline = dynamic_cast<const Polyline*>(curves.front().get());
        if (polyline == nullptr) {
            return 0;
        }

        std::vector<CurvePtr> pieces;
        for (const CurvePtr& segment : polyline->segments()) {
            pieces.push_back(segment->clone());
        }
        // Counted without erasing: the caller erases once, so that a failure
        // partway cannot leave the polyline gone and its parts missing.
        std::size_t added = 0;
        for (const CurvePtr& piece : pieces) {
            std::unique_ptr<Entity> replacement = entityFromCurve(*piece);
            if (replacement == nullptr) {
                continue;
            }
            copyProperties(entity, *replacement);
            BlockTableRecord* owner = context.db().get<BlockTableRecord>(entity.ownerHandle);
            context.transaction().addEntity(std::move(replacement),
                                            owner != nullptr ? *owner
                                                             : context.db().modelSpace());
            ++added;
        }
        return static_cast<int>(added);
    }
};

// ------------------------------------------------------------------- stretch

/// STRETCH — move the grips that fall inside a crossing window.
///
/// The defining behaviour is partial: an object with one end inside the window
/// and one outside is *reshaped*, while one entirely inside is moved. That is
/// what makes STRETCH different from MOVE, and it is why the window matters
/// rather than the selection alone.
///
/// Implemented over `Entity::moveGrip`, which each entity type already
/// implements for interactive grip editing — so a line stretches by its
/// endpoint and a polyline by the vertex, without this command knowing either.
class StretchCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& context) override {
        if (!context.initialSelection().empty()) {
            selection_ = context.initialSelection();
            return askWindowCorner();
        }
        return askSelection("Select objects to stretch");
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
            return askWindowCorner();
        }

        if (input.kind != InputValue::Kind::Point) {
            return StepResult::cancelled();
        }

        if (!hasFirstCorner_) {
            firstCorner_ = input.point;
            hasFirstCorner_ = true;
            InputRequest request;
            request.kind = InputRequest::Kind::Point;
            request.prompt = "Specify opposite corner of the crossing window";
            request.basePoint = firstCorner_;
            return StepResult::ask(request);
        }

        if (!hasWindow_) {
            window_ = Box3::fromCorners(firstCorner_, input.point);
            hasWindow_ = true;
            InputRequest request;
            request.kind = InputRequest::Kind::Point;
            request.prompt = "Specify base point";
            return StepResult::ask(request);
        }

        if (!hasBasePoint_) {
            basePoint_ = input.point;
            hasBasePoint_ = true;
            InputRequest request;
            request.kind = InputRequest::Kind::Point;
            request.prompt = "Specify second point of displacement";
            request.basePoint = basePoint_;
            return StepResult::ask(request);
        }

        return apply(input.point - basePoint_, context);
    }

private:
    [[nodiscard]] static StepResult askWindowCorner() {
        return askPoint("Specify first corner of the crossing window");
    }

    StepResult apply(const Vec3& displacement, CommandContext& context) {
        if (displacement.isZero()) {
            return StepResult::cancelled("Nothing moved");
        }

        const Mat4 translation = Mat4::translation(displacement);
        int stretched = 0;
        int moved = 0;

        for (const Handle handle : selection_) {
            Entity* entity = context.db().getEntity(handle);
            if (entity == nullptr || entity->erased) {
                continue;
            }
            if (isLocked(*entity, context)) {
                context.warn("Skipped an object on a locked layer.");
                continue;
            }

            const std::vector<GripPoint> grips = entity->gripPoints();
            const std::size_t inside = countInside(grips);

            context.transaction().openForWrite(*entity);
            if (grips.empty() || inside == grips.size()) {
                // Wholly enclosed, or nothing to grab: move it bodily. This is
                // the case that makes STRETCH usable as a MOVE that also drags
                // whatever is attached.
                entity->transformBy(translation);
                ++moved;
            } else if (inside > 0) {
                // Moved back to front so that an entity whose grips are indexed
                // positionally is not disturbed by its own earlier edits.
                for (std::size_t i = grips.size(); i-- > 0;) {
                    if (window_.containsPoint(grips[i].position)) {
                        entity->moveGrip(grips[i], grips[i].position + displacement);
                    }
                }
                ++stretched;
            } else {
                continue;
            }
            context.db().notifyModified(handle);
        }

        if (stretched == 0 && moved == 0) {
            return StepResult::cancelled("Nothing was inside the window");
        }
        return StepResult::done(std::to_string(stretched) + " stretched, " +
                                std::to_string(moved) + " moved");
    }

    [[nodiscard]] std::size_t countInside(const std::vector<GripPoint>& grips) const {
        return static_cast<std::size_t>(
            std::count_if(grips.begin(), grips.end(), [this](const GripPoint& grip) {
                return window_.containsPoint(grip.position);
            }));
    }

    std::vector<Handle> selection_;
    Vec3 firstCorner_;
    Box3 window_;
    Vec3 basePoint_;
    bool hasFirstCorner_ = false;
    bool hasWindow_ = false;
    bool hasBasePoint_ = false;
};

[[nodiscard]] CommandDefinition makeDefinition(std::string id, std::string name,
                                               std::vector<std::string> aliases,
                                               std::string title, std::string description,
                                               std::function<CommandInstancePtr()> create,
                                               bool pickfirst) {
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

[[nodiscard]] bool matchesKeyword(const std::string& typed, char initial) {
    return !typed.empty() &&
           std::toupper(static_cast<unsigned char>(typed.front())) == initial;
}

const CommandKeyword kPeditCloseKeyword{"Close", "C", "Join the last vertex to the first"};
const CommandKeyword kPeditOpenKeyword{"Open", "O", "Remove the closing segment"};
const CommandKeyword kPeditJoinKeyword{"Join", "J", "Add objects that meet an end"};
const CommandKeyword kPeditWidthKeyword{"Width", "W", "One width for every segment"};
const CommandKeyword kPeditReverseKeyword{"Reverse", "R", "Run the other way"};
const CommandKeyword kPeditDecurveKeyword{"Decurve", "D", "Straighten every arc segment"};
const CommandKeyword kPeditExitKeyword{"eXit", "X", "Done"};

/// PEDIT: edit one polyline. A line or arc picked instead is turned into one
/// first, as other CAD software does with PEDITACCEPT on.
/// LENGTHEN — make a line or an arc longer or shorter at the end picked.
///
/// By an amount, to a percentage of what it is, or to a total length. The end
/// nearer the pick is the one that moves; the other stays where it is, which
/// is what makes this different from STRETCH.
class LengthenCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& /*context*/) override {
        InputRequest request;
        request.kind = InputRequest::Kind::Keyword;
        request.prompt = "Enter an option [DElta/Percent/Total]";
        request.defaultText = "DElta";
        request.allowEmpty = true;
        request.keywords = {CommandKeyword{"DElta", "DE", "Add or take off an amount"},
                            CommandKeyword{"Percent", "P", "A percentage of the current length"},
                            CommandKeyword{"Total", "T", "A length to end up with"}};
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }
        switch (stage_) {
            case Stage::Mode: {
                if (input.kind == InputValue::Kind::Keyword || input.kind == InputValue::Kind::Text) {
                    if (matchesKeyword(input.text, 'P')) {
                        mode_ = Mode::Percent;
                    } else if (matchesKeyword(input.text, 'T')) {
                        mode_ = Mode::Total;
                    }
                }
                stage_ = Stage::Value;
                InputRequest request;
                request.kind = InputRequest::Kind::Number;
                request.prompt = mode_ == Mode::Delta     ? "Enter delta length"
                                 : mode_ == Mode::Percent ? "Enter percentage length"
                                                          : "Specify total length";
                request.defaultNumber = mode_ == Mode::Percent ? 100.0 : 0.0;
                return StepResult::ask(request);
            }
            case Stage::Value:
                if (input.kind != InputValue::Kind::Number) {
                    return StepResult::cancelled();
                }
                value_ = input.number;
                return askObject();
            case Stage::Object:
            default: {
                if (input.kind != InputValue::Kind::Entity) {
                    return changed_ == 0 ? StepResult::cancelled()
                                         : StepResult::done(std::to_string(changed_) +
                                                            " object(s) lengthened");
                }
                Entity* entity = context.db().getEntity(input.entity);
                if (entity == nullptr || entity->erased) {
                    return askObject();
                }
                if (isLocked(*entity, context)) {
                    context.warn("That object is on a locked layer.");
                    return askObject();
                }
                std::string problem;
                if (!lengthen(*entity, input.point, context, problem)) {
                    context.warn(problem);
                }
                return askObject();
            }
        }
    }

private:
    enum class Stage : std::uint8_t { Mode, Value, Object };
    enum class Mode : std::uint8_t { Delta, Percent, Total };

    [[nodiscard]] StepResult askObject() {
        stage_ = Stage::Object;
        InputRequest request;
        request.kind = InputRequest::Kind::Entity;
        request.prompt = "Select an object to change or press Enter";
        request.allowEmpty = true;
        return StepResult::ask(request);
    }

    [[nodiscard]] bool lengthen(Entity& entity, const Vec3& pick, CommandContext& context,
                                std::string& problem) {
        const std::vector<CurvePtr> curves = entity.toCurves();
        if (curves.size() != 1 || curves.front() == nullptr) {
            problem = "LENGTHEN works on a line or an arc.";
            return false;
        }
        const Curve& curve = *curves.front();
        const double length = curve.length();
        double delta = value_;
        if (mode_ == Mode::Percent) {
            delta = length * (value_ / 100.0 - 1.0);
        } else if (mode_ == Mode::Total) {
            delta = value_ - length;
        }
        // The end nearer the pick is the one that moves.
        const bool atEnd = pick.distanceTo(curve.endPoint()) <= pick.distanceTo(curve.startPoint());

        std::vector<CurvePtr> pieces;
        if (const auto* line = dynamic_cast<const LineSegment*>(&curve)) {
            if (length + delta <= 1e-9) {
                problem = "That would leave the line with no length.";
                return false;
            }
            const Vec3 direction = (line->end - line->start).normalized();
            pieces.push_back(std::make_unique<LineSegment>(
                atEnd ? line->start : line->start - direction * delta,
                atEnd ? line->end + direction * delta : line->end));
        } else if (const auto* arc = dynamic_cast<const Arc*>(&curve)) {
            if (arc->isFullCircle() || arc->radius <= 0.0) {
                problem = "A circle has no end to lengthen.";
                return false;
            }
            const double turn = delta / arc->radius;
            const double sweep = arc->sweep() + turn;
            if (sweep <= 1e-9 || sweep >= kTau) {
                problem = "That would leave the arc with no length, or close it.";
                return false;
            }
            const double start = atEnd ? arc->startAngle : arc->startAngle - turn;
            pieces.push_back(std::make_unique<Arc>(arc->center, arc->radius, start, start + sweep,
                                                   arc->normal));
        } else {
            problem = "LENGTHEN works on a line or an arc.";
            return false;
        }
        if (replaceWithCurves(entity, pieces, context) == 0) {
            problem = "The object could not be changed.";
            return false;
        }
        ++changed_;
        return true;
    }

    Stage stage_ = Stage::Mode;
    Mode mode_ = Mode::Delta;
    double value_ = 0.0;
    std::size_t changed_ = 0;
};

class PeditCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& context) override {
        for (const Handle handle : context.initialSelection()) {
            if (adopt(handle, context)) {
                return askOption(context);
            }
        }
        InputRequest request;
        request.kind = InputRequest::Kind::Entity;
        request.prompt = "Select polyline";
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return finish();
        }
        if (handle_.isNull()) {
            Handle picked;
            if (input.kind == InputValue::Kind::Entity) {
                picked = input.entity;
            } else if (input.kind == InputValue::Kind::Selection && !input.selection.empty()) {
                picked = input.selection.front();
            }
            if (!adopt(picked, context)) {
                return StepResult::failed("Select a polyline, a line or an arc.");
            }
            return askOption(context);
        }
        if (joining_) {
            joining_ = false;
            if (input.kind == InputValue::Kind::Selection && !input.selection.empty()) {
                join(input.selection, context);
            }
            return askOption(context);
        }
        if (settingWidth_) {
            settingWidth_ = false;
            if (input.kind == InputValue::Kind::Number && input.number >= 0.0) {
                edit(context, [&](PolylineEntity& poly) {
                    poly.constantWidth = input.number;
                    for (PolylineVertex& vertex : poly.vertices) {
                        vertex.startWidth = vertex.endWidth = 0.0;
                    }
                });
                ++changes_;
            }
            return askOption(context);
        }

        if (input.kind == InputValue::Kind::Empty ||
            (input.kind == InputValue::Kind::Keyword && matchesKeyword(input.text, 'X'))) {
            return finish();
        }
        if (input.kind != InputValue::Kind::Keyword && input.kind != InputValue::Kind::Text) {
            return askOption(context);
        }
        const std::string typed = input.text;
        if (matchesKeyword(typed, 'C')) {
            const auto* poly = context.db().get<PolylineEntity>(handle_);
            if (poly != nullptr && poly->vertices.size() < 3) {
                context.warn("A polyline needs three vertices to close.");
            } else if (poly != nullptr && !poly->closed) {
                edit(context, [](PolylineEntity& p) { p.closed = true; });
                ++changes_;
            }
        } else if (matchesKeyword(typed, 'O')) {
            edit(context, [](PolylineEntity& p) { p.closed = false; });
            ++changes_;
        } else if (matchesKeyword(typed, 'J')) {
            joining_ = true;
            InputRequest request;
            request.kind = InputRequest::Kind::Selection;
            request.prompt = "Select objects to join";
            return StepResult::ask(request);
        } else if (matchesKeyword(typed, 'W')) {
            settingWidth_ = true;
            InputRequest request;
            request.kind = InputRequest::Kind::Distance;
            request.prompt = "Specify new width for all segments";
            const auto* poly = context.db().get<PolylineEntity>(handle_);
            request.defaultNumber = poly != nullptr ? poly->constantWidth : 0.0;
            return StepResult::ask(request);
        } else if (matchesKeyword(typed, 'R')) {
            edit(context, [](PolylineEntity& p) {
                const CurvePtr reversed = p.geometry().reversed();
                if (const auto* back = dynamic_cast<const Polyline*>(reversed.get())) {
                    p.vertices = back->vertices;
                }
            });
            ++changes_;
        } else if (matchesKeyword(typed, 'D')) {
            edit(context, [](PolylineEntity& p) {
                for (PolylineVertex& vertex : p.vertices) {
                    vertex.bulge = 0.0;
                }
            });
            ++changes_;
        } else {
            context.warn("Close, Open, Join, Width, Reverse, Decurve or eXit.");
        }
        return askOption(context);
    }

private:
    /// Take the entity as the polyline to edit; a line or arc becomes one.
    bool adopt(Handle handle, CommandContext& context) {
        Entity* entity = context.db().getEntity(handle);
        if (entity == nullptr || entity->erased) {
            return false;
        }
        if (dynamic_cast<PolylineEntity*>(entity) != nullptr) {
            handle_ = handle;
            return true;
        }
        if (dynamic_cast<LineEntity*>(entity) == nullptr && dynamic_cast<ArcEntity*>(entity) == nullptr) {
            return false;
        }
        std::vector<CurvePtr> curves = entity->toCurves();
        if (curves.empty()) {
            return false;
        }
        Chain chain;
        chain.curves.push_back(std::move(curves.front()));
        const std::optional<Polyline> polyline = polylineFromChain(chain);
        if (!polyline) {
            return false;
        }
        auto replacement = std::make_unique<PolylineEntity>(PolylineEntity::fromPolyline(*polyline));
        copyProperties(*entity, *replacement);
        BlockTableRecord* owner = context.db().get<BlockTableRecord>(entity->ownerHandle);
        handle_ = context.transaction()
                      .addEntity(std::move(replacement),
                                 owner != nullptr ? *owner : context.db().currentSpace())
                      .handle;
        context.transaction().erase(*entity);
        context.log("Object selected is not a polyline; turned into one.");
        ++changes_;
        return true;
    }

    template <typename F>
    void edit(CommandContext& context, F&& mutate) {
        if (auto* poly = context.db().get<PolylineEntity>(handle_)) {
            context.transaction().modifyEntity(*poly, std::forward<F>(mutate));
        }
    }

    /// An entity as the curves a polyline can hold: a polyline's own segments
    /// one by one, since as a single curve it could not be chained with
    /// anything, and everything else as it comes.
    [[nodiscard]] static std::vector<CurvePtr> joinableCurves(const Entity& entity) {
        const auto* poly = dynamic_cast<const PolylineEntity*>(&entity);
        if (poly == nullptr) {
            return entity.toCurves();
        }
        std::vector<CurvePtr> curves;
        const std::size_t count = poly->vertices.size();
        const std::size_t segments = poly->closed ? count : (count > 0 ? count - 1 : 0);
        for (std::size_t i = 0; i < segments; ++i) {
            const PolylineVertex& from = poly->vertices[i];
            const PolylineVertex& to = poly->vertices[(i + 1) % count];
            if (from.bulge != 0.0) {
                if (auto arc = Arc::fromBulge(from.position, to.position, from.bulge)) {
                    curves.push_back(std::make_unique<Arc>(*arc));
                    continue;
                }
            }
            curves.push_back(std::make_unique<LineSegment>(from.position, to.position));
        }
        return curves;
    }

    /// Join the selected objects onto this polyline, where they meet an end.
    void join(const std::vector<Handle>& picked, CommandContext& context) {
        Entity* self = context.db().getEntity(handle_);
        if (self == nullptr) {
            return;
        }
        std::vector<Entity*> sources{self};
        std::vector<std::size_t> curveCounts;
        std::vector<CurvePtr> curves = joinableCurves(*self);
        curveCounts.push_back(curves.size());
        for (const Handle handle : picked) {
            Entity* entity = context.db().getEntity(handle);
            if (entity == nullptr || entity->erased || entity == self || isLocked(*entity, context)) {
                continue;
            }
            std::vector<CurvePtr> more = joinableCurves(*entity);
            if (more.empty()) {
                continue;
            }
            sources.push_back(entity);
            curveCounts.push_back(more.size());
            for (CurvePtr& curve : more) {
                curves.push_back(std::move(curve));
            }
        }
        if (sources.size() < 2) {
            context.warn("Nothing meets the polyline's ends.");
            return;
        }
        // The chain that grew from this polyline is the result; the others
        // did not connect and are left as they were.
        const std::vector<Chain> chains = chainCurves(std::move(curves));
        const Chain* longest = nullptr;
        for (const Chain& chain : chains) {
            if (longest == nullptr || chain.curves.size() > longest->curves.size()) {
                longest = &chain;
            }
        }
        if (longest == nullptr || longest->curves.size() < 2) {
            context.warn("Nothing meets the polyline's ends.");
            return;
        }
        const std::optional<Polyline> polyline = polylineFromChain(*longest);
        if (!polyline) {
            context.warn("Those objects include geometry a polyline cannot hold.");
            return;
        }
        auto joined = std::make_unique<PolylineEntity>(PolylineEntity::fromPolyline(*polyline));
        copyProperties(*self, *joined);
        BlockTableRecord* owner = context.db().get<BlockTableRecord>(self->ownerHandle);
        const Handle fresh = context.transaction()
                                 .addEntity(std::move(joined),
                                            owner != nullptr ? *owner : context.db().currentSpace())
                                 .handle;
        // Only what the chain took in is consumed; the segment count says
        // how many curves it holds, and every source that supplied one of
        // them is gone. A source whose curves were left out stays.
        std::size_t taken = 0;
        for (std::size_t i = 0; i < sources.size(); ++i) {
            if (taken + curveCounts[i] <= longest->curves.size()) {
                taken += curveCounts[i];
                context.transaction().erase(*sources[i]);
            }
        }
        handle_ = fresh;
        context.log(std::to_string(sources.size() - 1) + " object(s) joined");
        ++changes_;
    }

    [[nodiscard]] StepResult askOption(CommandContext& context) const {
        const auto* poly = context.db().get<PolylineEntity>(handle_);
        InputRequest request;
        request.kind = InputRequest::Kind::Text;
        request.prompt = "Enter an option [Close/Open/Join/Width/Reverse/Decurve/eXit] <eXit>";
        request.keywords = {poly != nullptr && poly->closed ? kPeditOpenKeyword : kPeditCloseKeyword,
                            kPeditJoinKeyword, kPeditWidthKeyword, kPeditReverseKeyword,
                            kPeditDecurveKeyword, kPeditExitKeyword};
        request.allowEmpty = true;
        return StepResult::ask(request);
    }

    [[nodiscard]] StepResult finish() const {
        if (changes_ == 0) {
            return StepResult::cancelled();
        }
        return StepResult::done(std::to_string(changes_) + " change(s) to the polyline");
    }

    Handle handle_;
    bool joining_ = false;
    bool settingWidth_ = false;
    int changes_ = 0;
};

std::vector<CommandDefinition> reshapeCommands() {
    std::vector<CommandDefinition> commands;
    commands.push_back(makeDefinition("cad.modify.chamfer", "CHAMFER", {"CHA"}, "Chamfer",
                                      "Bevel the corner between two lines",
                                      [] { return std::make_unique<ChamferCommand>(); }, false));
    commands.push_back(makeDefinition("cad.modify.break", "BREAK", {"BR"}, "Break",
                                      "Split an object, removing the span between two points",
                                      [] { return std::make_unique<BreakCommand>(); }, false));
    commands.push_back(makeDefinition("cad.modify.join", "JOIN", {"J"}, "Join",
                                      "Merge objects that meet end to end into one polyline",
                                      [] { return std::make_unique<JoinCommand>(); }, true));
    commands.push_back(makeDefinition("cad.modify.lengthen", "LENGTHEN", {"LEN"}, "Lengthen",
                                      "Make a line or an arc longer or shorter at one end",
                                      [] { return std::make_unique<LengthenCommand>(); },
                                      false));
    commands.push_back(makeDefinition("cad.modify.pedit", "PEDIT", {"PE"}, "Edit Polyline",
                                      "Close, open, join, widen, reverse or straighten a polyline",
                                      [] { return std::make_unique<PeditCommand>(); }, true));
    commands.push_back(makeDefinition("cad.modify.explode", "EXPLODE", {"X"}, "Explode",
                                      "Replace blocks and polylines with their parts",
                                      [] { return std::make_unique<ExplodeCommand>(); }, true));
    commands.push_back(makeDefinition("cad.modify.stretch", "STRETCH", {"S"}, "Stretch",
                                      "Move the parts of objects inside a crossing window",
                                      [] { return std::make_unique<StretchCommand>(); }, true));
    return commands;
}

}  // namespace cad
