/// UPDATEFIELD — recompute text whose value is derived.
///
/// A field is a promise that a number on a drawing follows the thing it
/// measures. The promise is only kept when something re-evaluates it, and
/// other CAD software makes that an explicit act as often as an automatic one, because
/// re-evaluating on every edit would be both slow and surprising: a drafter
/// mid-way through moving a wall does not want the schedule flickering.
///
/// So this is a command. It reports what it changed, and what it could not
/// evaluate and why, because a field that quietly kept a stale number is
/// indistinguishable from one that was correct — and on a construction drawing
/// those two are not the same at all.

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "cad/command/StandardCommands.hpp"
#include "cad/db/FieldEvaluator.hpp"
#include "cad/db/FieldObject.hpp"
#include "cad/db/TextEntities.hpp"

namespace cad {
namespace {

/// Apply a field's value to the entity carrying it.
///
/// @returns true when the entity's text changed.
[[nodiscard]] bool applyTo(Transaction& transaction, Entity& entity, const std::string& value) {
    if (auto* mtext = dynamic_cast<MTextEntity*>(&entity)) {
        if (mtext->contents == value) {
            return false;
        }
        transaction.openForWrite(*mtext).contents = value;
        return true;
    }
    if (auto* text = dynamic_cast<TextEntity*>(&entity)) {
        // Single-line text has no markup, so the field's value goes in as it
        // reads. A `_text` field's own line breaks cannot survive here, and
        // that is a property of the entity rather than something to work
        // around: a TEXT is one line.
        if (text->contents == value) {
            return false;
        }
        transaction.openForWrite(*text).contents = value;
        return true;
    }
    return false;
}

class UpdateFieldCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& context) override {
        FieldContext fieldContext;
        fieldContext.filePath = context.db().filePath;
        // The name, not the path: a title block that prints the whole path is
        // a title block nobody wanted.
        const std::size_t slash = fieldContext.filePath.find_last_of("/\\");
        fieldContext.fileName = slash == std::string::npos
                                    ? fieldContext.filePath
                                    : fieldContext.filePath.substr(slash + 1);

        const FieldEvaluator evaluator{context.db(), fieldContext};

        std::size_t changed = 0;
        std::size_t held = 0;
        std::map<std::string, std::size_t> problems;

        for (const Handle blockHandle : context.db().blocks().handles()) {
            auto* block = context.db().get<BlockTableRecord>(blockHandle);
            if (block == nullptr) {
                continue;
            }
            for (Entity* entity : context.db().entitiesIn(*block)) {
                if (entity == nullptr || entity->erased || entity->fieldHandle.isNull()) {
                    continue;
                }
                const auto* field = context.db().get<FieldObject>(entity->fieldHandle);
                if (field == nullptr) {
                    continue;
                }
                const FieldResult result = evaluator.evaluate(*field);
                if (!result.evaluated) {
                    ++held;
                    if (!result.problem.empty()) {
                        ++problems[result.problem];
                    }
                    // The cached value is written back anyway when it differs
                    // from what the entity shows: they can disagree after an
                    // undo, and the cache is still the better of the two.
                }
                if (applyTo(context.transaction(), *entity, result.text)) {
                    ++changed;
                }
            }
        }

        // Table cells are not entities, so the walk above does not reach them.
        // Their values are composed from child fields rather than stored, which
        // is why they need evaluating rather than reading.
        changed += refreshTableCells(context.db(), fieldContext);

        if (changed == 0 && held == 0) {
            return StepResult::failed("This drawing has no fields.");
        }

        context.log(changed == 1 ? "1 field updated." : std::to_string(changed) +
                                                            " fields updated.");
        if (held > 0) {
            context.log(std::to_string(held) +
                        " kept the value the drawing was saved with:");
            for (const auto& [problem, count] : problems) {
                context.log("  " + std::to_string(count) + "x " + problem);
            }
        }
        return StepResult::done();
    }

    StepResult supply(const InputValue&, CommandContext&) override {
        return StepResult::done();
    }
};

}  // namespace

std::vector<CommandDefinition> fieldCommands() {
    CommandDefinition definition;
    definition.id = "cad.annotate.updatefield";
    definition.name = "UPDATEFIELD";
    definition.aliases = {"FIELDEVAL"};
    definition.title = "Update Fields";
    definition.description = "Recompute text whose value is derived from the drawing";
    definition.category = "Annotate";
    definition.supportsPickfirst = false;
    definition.create = [] { return std::make_unique<UpdateFieldCommand>(); };

    std::vector<CommandDefinition> commands;
    commands.push_back(std::move(definition));
    return commands;
}

}  // namespace cad
