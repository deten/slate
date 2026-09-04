/// BLOCK and INSERT — defining and placing block references.
///
/// The application could already read block definitions, resolve them, expand
/// them for drawing and explode them; it could not define one. A drawing with
/// a hundred and fifty blocks in it was strictly read-only in the one respect
/// that matters most for reuse.
///
/// Blocks are the CAD idea that makes a drawing maintainable: a door drawn once
/// and placed forty times is forty references to one definition, so correcting
/// the door corrects all of them. Anything less than a real definition — copying
/// geometry forty times — is what these commands exist to avoid.

#include <string>
#include <utility>
#include <vector>

#include "cad/command/StandardCommands.hpp"
#include "cad/db/BlockTableRecord.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/Layer.hpp"

namespace cad {
namespace {

[[nodiscard]] StepResult askName(std::string prompt) {
    InputRequest request;
    request.kind = InputRequest::Kind::Text;
    request.prompt = std::move(prompt);
    return StepResult::ask(request);
}

// Defined with INSERT below; BLOCK uses them for the reference it leaves.
[[nodiscard]] std::vector<Handle> attributeDefinitionsIn(const BlockTableRecord& block,
                                                         Database& db);
void attachAttributes(InsertEntity& placed, const std::vector<Handle>& definitions,
                      const std::vector<std::string>& values, CommandContext& context);

[[nodiscard]] StepResult askPoint(std::string prompt) {
    InputRequest request;
    request.kind = InputRequest::Kind::Point;
    request.prompt = std::move(prompt);
    return StepResult::ask(request);
}

[[nodiscard]] StepResult askNumber(std::string prompt, double fallback) {
    InputRequest request;
    request.kind = InputRequest::Kind::Distance;
    request.prompt = std::move(prompt);
    request.defaultNumber = fallback;
    return StepResult::ask(request);
}

/// BLOCK — turn a selection into a reusable definition, and place one.
///
/// The selection is *replaced* by a reference to the new block, which is what
/// other CAD software does and what the user means: having asked for these objects to
/// become a block, being left with the loose originals as well would be a
/// surprise and a duplicate.
class BlockCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& context) override {
        if (!context.initialSelection().empty()) {
            selection_ = context.initialSelection();
            return askPoint("Specify base point");
        }
        InputRequest request;
        request.kind = InputRequest::Kind::Selection;
        request.prompt = "Select objects for the block";
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
            return askPoint("Specify base point");
        }

        if (!hasBasePoint_) {
            if (input.kind != InputValue::Kind::Point) {
                return StepResult::cancelled();
            }
            basePoint_ = input.point;
            hasBasePoint_ = true;
            return askName("Enter block name");
        }

        if (input.kind != InputValue::Kind::Text || input.text.empty()) {
            return StepResult::cancelled("No name given");
        }
        return define(input.text, context);
    }

private:
    StepResult define(const std::string& name, CommandContext& context) {
        // Refused rather than redefined. Redefining silently changes every
        // existing reference in the drawing, which is a large and invisible
        // edit to make on the strength of a name collision.
        if (context.db().findBlock(name) != nullptr) {
            return StepResult::failed("A block named \"" + name +
                                      "\" already exists. Choose another name.");
        }

        std::vector<Entity*> sources;
        for (const Handle handle : selection_) {
            Entity* entity = context.db().getEntity(handle);
            if (entity == nullptr || entity->erased) {
                continue;
            }
            if (const Layer* layer = context.db().get<Layer>(entity->layerHandle);
                layer != nullptr && layer->isLocked) {
                context.warn("Skipped an object on a locked layer.");
                continue;
            }
            sources.push_back(entity);
        }
        if (sources.empty()) {
            return StepResult::failed("Nothing to make a block from.");
        }

        auto record = std::make_unique<BlockTableRecord>();
        record->name = name;
        auto& block = static_cast<BlockTableRecord&>(
            context.transaction().addObject(std::move(record)));

        // Contents are stored relative to the base point, so that an insert
        // places the base point *at* its insertion point. Storing world
        // coordinates instead would make every reference land at the origin.
        const Mat4 toBlockSpace = Mat4::translation(-basePoint_);
        for (const Entity* source : sources) {
            std::unique_ptr<Entity> copy = cloneEntity(*source);
            if (copy == nullptr) {
                continue;
            }
            copy->transformBy(toBlockSpace);
            context.transaction().addEntity(std::move(copy), block);
        }

        auto reference = std::make_unique<InsertEntity>(block.handle, basePoint_);
        reference->layerHandle = sources.front()->layerHandle;
        auto& placed = static_cast<InsertEntity&>(
            context.transaction().addToCurrentSpace(std::move(reference)));
        // The reference standing in for the sources shows what they showed:
        // each attribute definition becomes a value, at its default.
        attachAttributes(placed, attributeDefinitionsIn(block, context.db()), {}, context);

        for (Entity* source : sources) {
            context.transaction().erase(*source);
        }
        return StepResult::done("Block \"" + name + "\" defined from " +
                                std::to_string(sources.size()) + " object(s)");
    }

    std::vector<Handle> selection_;
    Vec3 basePoint_;
    bool hasBasePoint_ = false;
};

/// INSERT — place a reference to an existing block.
/// The non-constant attribute definitions in a block, in draw order. These
/// are the questions an insert answers; constant ones are drawn from the
/// definition and never asked.
[[nodiscard]] std::vector<Handle> attributeDefinitionsIn(const BlockTableRecord& block,
                                                         Database& db) {
    std::vector<Handle> definitions;
    for (const Entity* entity : db.entitiesIn(block)) {
        const auto* definition = dynamic_cast<const AttributeDefinitionEntity*>(entity);
        if (definition != nullptr && !definition->erased && !definition->flags.isConstant) {
            definitions.push_back(definition->handle);
        }
    }
    return definitions;
}

/// One ATTRIB per definition on a placed reference, carried where the
/// definition's text sits once the block is placed, scaled and turned. Owned
/// by the reference: written after it in the file, moved with it on screen.
void attachAttributes(InsertEntity& placed, const std::vector<Handle>& definitions,
                      const std::vector<std::string>& values, CommandContext& context) {
    const Mat4 placement = placed.blockTransform();
    const double heightScale = std::abs(placed.scale.y);
    for (std::size_t i = 0; i < definitions.size(); ++i) {
        const auto* definition = context.db().get<AttributeDefinitionEntity>(definitions[i]);
        if (definition == nullptr) {
            continue;
        }
        auto attribute = std::make_unique<AttributeEntity>();
        attribute->tag = definition->tag;
        attribute->flags = definition->flags;
        attribute->contents = i < values.size() ? values[i] : definition->defaultValue;
        attribute->position = placement.transformPoint(definition->position);
        if (definition->alignmentPoint) {
            attribute->alignmentPoint = placement.transformPoint(*definition->alignmentPoint);
        }
        attribute->horizontalAlignment = definition->horizontalAlignment;
        attribute->verticalAlignment = definition->verticalAlignment;
        attribute->height = definition->height * heightScale;
        attribute->rotation = definition->rotation + placed.rotation;
        attribute->widthFactor = definition->widthFactor;
        attribute->obliqueAngle = definition->obliqueAngle;
        attribute->styleHandle = definition->styleHandle;
        attribute->layerHandle = definition->layerHandle;
        BlockTableRecord* owner = context.db().get<BlockTableRecord>(placed.ownerHandle);
        const Handle handle =
            context.transaction()
                .addEntity(std::move(attribute), owner != nullptr ? *owner : context.db().currentSpace())
                .handle;
        placed.attributeHandles.push_back(handle);
    }
}

class InsertCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& context) override {
        if (context.db().blocks().size() == 0) {
            return StepResult::failed("This drawing has no blocks to insert.");
        }
        return askName("Enter block name");
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }

        if (blockHandle_.isNull()) {
            if (input.kind != InputValue::Kind::Text || input.text.empty()) {
                return StepResult::cancelled();
            }
            const BlockTableRecord* block = context.db().findBlock(input.text);
            if (block == nullptr) {
                return StepResult::failed("No block named \"" + input.text + "\".");
            }
            // Layout block records hold a sheet's contents rather than a
            // reusable symbol; inserting one would nest paper space inside the
            // model.
            if (block->isLayoutSpace()) {
                return StepResult::failed("\"" + input.text +
                                          "\" is a layout, not an insertable block.");
            }
            blockHandle_ = block->handle;
            name_ = block->name;
            return askPoint("Specify insertion point");
        }

        if (!hasPosition_) {
            if (input.kind != InputValue::Kind::Point) {
                return StepResult::cancelled();
            }
            position_ = input.point;
            hasPosition_ = true;
            return askNumber("Specify X scale factor", 1.0);
        }

        if (!hasScaleX_) {
            scaleX_ = input.kind == InputValue::Kind::Number ? input.number : 1.0;
            if (scaleX_ == 0.0) {
                return StepResult::failed("A scale factor of zero would collapse the block.");
            }
            hasScaleX_ = true;
            // Defaulting Y to X makes the uniform case one keystroke while
            // leaving a stretched insert available.
            return askNumber("Specify Y scale factor", scaleX_);
        }

        if (!hasScaleY_) {
            scaleY_ = input.kind == InputValue::Kind::Number ? input.number : scaleX_;
            if (scaleY_ == 0.0) {
                return StepResult::failed("A scale factor of zero would collapse the block.");
            }
            hasScaleY_ = true;
            InputRequest request;
            request.kind = InputRequest::Kind::Angle;
            request.prompt = "Specify rotation angle";
            request.defaultNumber = 0.0;
            request.basePoint = position_;
            return StepResult::ask(request);
        }

        if (!hasRotation_) {
            if (input.kind == InputValue::Kind::Number) {
                rotation_ = input.number;
            } else if (input.kind == InputValue::Kind::Point) {
                rotation_ = (input.point - position_).angle2();
            }
            hasRotation_ = true;

            // The block's attribute definitions are questions; each gets its
            // answer before the reference is placed. Constant ones are never
            // asked, preset ones take their default without asking.
            definitions_ = attributeDefinitionsIn(
                *context.db().get<BlockTableRecord>(blockHandle_), context.db());
            for (const Handle handle : definitions_) {
                const auto* definition = context.db().get<AttributeDefinitionEntity>(handle);
                values_.push_back(definition != nullptr ? definition->defaultValue : std::string{});
            }
            return askNextValue(context);
        }

        // A value for the definition currently asked about.
        if (input.kind == InputValue::Kind::Text) {
            values_[asked_] = input.text;
        }
        ++asked_;
        return askNextValue(context);
    }

private:
    [[nodiscard]] StepResult askNextValue(CommandContext& context) {
        while (asked_ < definitions_.size()) {
            const auto* definition =
                context.db().get<AttributeDefinitionEntity>(definitions_[asked_]);
            if (definition == nullptr || definition->flags.isPreset) {
                ++asked_;
                continue;
            }
            InputRequest request;
            request.kind = InputRequest::Kind::Text;
            request.prompt = definition->prompt.empty() ? definition->tag : definition->prompt;
            request.defaultText = definition->defaultValue;
            request.allowEmpty = true;
            return StepResult::ask(request);
        }
        return place(context);
    }

    StepResult place(CommandContext& context) {
        auto reference = std::make_unique<InsertEntity>(blockHandle_, position_);
        reference->scale = Vec3{scaleX_, scaleY_, 1.0};
        reference->rotation = rotation_;
        auto& placed = static_cast<InsertEntity&>(
            context.transaction().addToCurrentSpace(std::move(reference)));
        attachAttributes(placed, definitions_, values_, context);
        return StepResult::done("Inserted \"" + name_ + "\"" +
                                (definitions_.empty()
                                     ? std::string{}
                                     : " with " + std::to_string(definitions_.size()) +
                                           " attribute(s)"));
    }

    Handle blockHandle_;
    std::string name_;
    Vec3 position_;
    double scaleX_ = 1.0;
    double scaleY_ = 1.0;
    double rotation_ = 0.0;
    bool hasPosition_ = false;
    bool hasScaleX_ = false;
    bool hasScaleY_ = false;
    bool hasRotation_ = false;
    std::vector<Handle> definitions_;
    std::vector<std::string> values_;
    std::size_t asked_ = 0;
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
    definition.category = "Block";
    definition.supportsPickfirst = pickfirst;
    definition.create = std::move(create);
    return definition;
}

}  // namespace

std::vector<CommandDefinition> blockCommands() {
    std::vector<CommandDefinition> commands;
    commands.push_back(makeDefinition("cad.block.define", "BLOCK", {"B", "BMAKE"}, "Create Block",
                                      "Turn a selection into a reusable block definition",
                                      [] { return std::make_unique<BlockCommand>(); }, true));
    commands.push_back(makeDefinition("cad.block.insert", "INSERT", {"I", "DDINSERT"}, "Insert",
                                      "Place a reference to an existing block",
                                      [] { return std::make_unique<InsertCommand>(); }, false));
    return commands;
}

}  // namespace cad
