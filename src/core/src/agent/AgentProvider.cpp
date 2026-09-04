#include "cad/agent/AgentProvider.hpp"

#include <algorithm>
#include <map>
#include <sstream>

#include "cad/agent/DrawingStandards.hpp"
#include "cad/command/CommandRegistry.hpp"
#include "cad/db/BlockTableRecord.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/DimStyle.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/Layer.hpp"
#include "cad/db/Layout.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/db/TextStyle.hpp"

namespace cad {
namespace {

/// A number the way a prompt should carry it: no exponent, no long tail of
/// zeros. A model reading `1.2000000000000002` learns nothing the shorter form
/// does not tell it, and spends more of its attention on it.
[[nodiscard]] std::string rounded(double value) {
    std::ostringstream out;
    out.precision(3);
    out << std::fixed << value;
    std::string text = out.str();
    const std::size_t lastDigit = text.find_last_not_of('0');
    if (lastDigit != std::string::npos) {
        text.erase(text[lastDigit] == '.' ? lastDigit : lastDigit + 1);
    }
    return text;
}

[[nodiscard]] std::string span(const Box3& box) {
    return rounded(box.min.x) + "," + rounded(box.min.y) + " to " + rounded(box.max.x) + "," +
           rounded(box.max.y);
}

template <typename T>
[[nodiscard]] std::vector<const T*> records(const Database& db, const SymbolTableIndex& table) {
    std::vector<const T*> found;
    for (const Handle handle : table.handles()) {
        if (const auto* record = db.get<T>(handle); record != nullptr && !record->erased) {
            found.push_back(record);
        }
    }
    std::sort(found.begin(), found.end(),
              [](const T* a, const T* b) { return a->name < b->name; });
    return found;
}

}  // namespace

std::string describeDrawing(const Database& db, std::span<const Handle> selection) {
    std::ostringstream out;

    out << "Units: " << drawingUnitsName(db.settings.insertionUnits) << "\n";

    // Where new objects will go. A sheet and model space take different
    // coordinates, and a model told the wrong one draws in the wrong place.
    const BlockTableRecord& space = const_cast<Database&>(db).currentSpace();
    if (space.isModelSpace()) {
        out << "Current space: model space\n";
    } else if (const auto* layout = db.get<Layout>(space.layoutHandle); layout != nullptr) {
        out << "Current space: sheet \"" << layout->name << "\"\n";
    }

    const auto& model = const_cast<Database&>(db).modelSpace();
    const std::vector<const Entity*> entities = db.entitiesIn(model);

    std::map<std::string, int> counts;
    std::map<Handle, int> onLayer;
    Box3 extents;
    for (const Entity* entity : entities) {
        if (entity == nullptr || entity->erased) {
            continue;
        }
        ++counts[entity->objectType()];
        ++onLayer[entity->layerHandle];
        extents = extents.united(entity->bounds());
    }

    out << "Model space holds " << entities.size() << " objects";
    if (!counts.empty()) {
        out << ":";
        for (const auto& [type, count] : counts) {
            out << "\n  " << count << " x " << type;
        }
    }
    out << "\n";

    if (!extents.isEmpty()) {
        out << "Extents: " << span(extents) << "\n";
    } else {
        out << "The drawing is empty.\n";
    }

    out << "Current layer: " << db.settings.currentLayerName << "\n";

    // Layers with what they carry. The colour and linetype are what tells a
    // model that A-WALL-HEAV and A-WALL-LITE differ, and the count is what
    // tells it which layers the drawing actually lives on.
    std::vector<const Layer*> layers = records<Layer>(db, db.layers());
    // Busiest first, so the cap below cuts off the empty ones and never the
    // layer half the drawing lives on.
    const auto objectsOn = [&](const Layer* layer) {
        const auto count = onLayer.find(layer->handle);
        return count == onLayer.end() ? 0 : count->second;
    };
    std::stable_sort(layers.begin(), layers.end(), [&](const Layer* a, const Layer* b) {
        return objectsOn(a) > objectsOn(b);
    });
    out << "Layers (" << layers.size() << "), most used first:\n";
    // Capped. A drawing can carry hundreds, and a list that long crowds out the
    // instruction it is supposed to be supporting.
    constexpr std::size_t kMaxLayers = 40;
    for (std::size_t i = 0; i < layers.size() && i < kMaxLayers; ++i) {
        const Layer& layer = *layers[i];
        out << "  " << layer.name << ": colour " << colorNameOf(layer.color) << ", "
            << linetypeNameOf(db, layer.linetypeHandle);
        if (const auto count = onLayer.find(layer.handle); count != onLayer.end()) {
            out << ", " << count->second << " object" << (count->second == 1 ? "" : "s");
        }
        if (layer.isOff) {
            out << ", off";
        }
        if (layer.isFrozen) {
            out << ", frozen";
        }
        if (layer.isLocked) {
            out << ", locked";
        }
        if (!layer.description.empty()) {
            out << " - " << layer.description;
        }
        out << "\n";
    }
    if (layers.size() > kMaxLayers) {
        out << "  and " << (layers.size() - kMaxLayers) << " more\n";
    }

    // Blocks: what can be placed with INSERT, and what it asks for.
    std::map<Handle, int> placed;
    for (const Entity* entity : entities) {
        if (const auto* insert = dynamic_cast<const InsertEntity*>(entity);
            insert != nullptr && !insert->erased) {
            ++placed[insert->blockHandle];
        }
    }
    std::vector<const BlockTableRecord*> blocks;
    for (const BlockTableRecord* block : records<BlockTableRecord>(db, db.blocks())) {
        if (!block->isLayoutSpace() && !block->isAnonymous() && !block->isXref()) {
            blocks.push_back(block);
        }
    }
    const auto placements = [&](const BlockTableRecord* block) {
        const auto count = placed.find(block->handle);
        return count == placed.end() ? 0 : count->second;
    };
    std::stable_sort(blocks.begin(), blocks.end(),
                     [&](const BlockTableRecord* a, const BlockTableRecord* b) {
                         return placements(a) > placements(b);
                     });
    if (!blocks.empty()) {
        constexpr std::size_t kMaxBlocks = 30;
        out << "Blocks (" << blocks.size() << "), most placed first:\n";
        for (std::size_t i = 0; i < blocks.size() && i < kMaxBlocks; ++i) {
            const BlockTableRecord& block = *blocks[i];
            out << "  " << block.name;
            if (const auto count = placed.find(block.handle); count != placed.end()) {
                out << ": placed " << count->second << " time" << (count->second == 1 ? "" : "s");
            } else {
                out << ": not placed";
            }
            bool first = true;
            for (const Entity* entity : db.entitiesIn(block)) {
                if (const auto* definition = dynamic_cast<const AttributeDefinitionEntity*>(entity);
                    definition != nullptr && !definition->erased) {
                    out << (first ? ", attributes " : ", ") << definition->tag;
                    first = false;
                }
            }
            out << "\n";
        }
        if (blocks.size() > kMaxBlocks) {
            out << "  and " << (blocks.size() - kMaxBlocks) << " more\n";
        }
    }

    const auto names = [&](const char* title, const auto& list) {
        if (list.empty()) {
            return;
        }
        out << title << ":";
        for (std::size_t i = 0; i < list.size() && i < 12; ++i) {
            out << (i == 0 ? " " : ", ") << list[i]->name;
        }
        out << "\n";
    };
    names("Text styles", records<TextStyle>(db, db.textStyles()));
    names("Dimension styles", records<DimStyle>(db, db.dimStyles()));

    std::vector<const Layout*> sheets;
    for (const Layout* layout : db.layouts()) {
        const auto* block = db.get<BlockTableRecord>(layout->blockRecordHandle);
        if (block != nullptr && !block->isModelSpace()) {
            sheets.push_back(layout);
        }
    }
    if (!sheets.empty()) {
        out << "Sheets (" << sheets.size() << "):";
        for (std::size_t i = 0; i < sheets.size() && i < 12; ++i) {
            const Layout& layout = *sheets[i];
            // Counted from the sheet itself. The first viewport on a sheet is
            // the paper, not a window onto the model, so it is left out.
            std::size_t viewports = 0;
            if (const auto* block = db.get<BlockTableRecord>(layout.blockRecordHandle)) {
                for (const Entity* entity : db.entitiesIn(*block)) {
                    if (entity != nullptr && !entity->erased && entity->objectType() == "Viewport") {
                        ++viewports;
                    }
                }
            }
            viewports = viewports > 0 ? viewports - 1 : 0;
            out << (i == 0 ? " " : ", ") << layout.name << " (" << rounded(layout.plot.paperWidth)
                << " x " << rounded(layout.plot.paperHeight) << ", " << viewports << " viewport"
                << (viewports == 1 ? "" : "s") << ")";
        }
        out << "\n";
    }

    if (!selection.empty()) {
        out << "Selected: " << selection.size() << " object" << (selection.size() == 1 ? "" : "s");
        Box3 selected;
        for (const Handle handle : selection) {
            if (const Entity* entity = db.getEntity(handle);
                entity != nullptr && !entity->erased) {
                selected = selected.united(entity->bounds());
            }
        }
        if (!selected.isEmpty()) {
            out << ", spanning " << span(selected);
        }
        out << "\n";
        // Each one, so a request about "these" can be answered about these.
        constexpr std::size_t kMaxSelected = 20;
        std::size_t shown = 0;
        for (const Handle handle : selection) {
            const Entity* entity = db.getEntity(handle);
            if (entity == nullptr || entity->erased) {
                continue;
            }
            if (shown == kMaxSelected) {
                out << "  and " << (selection.size() - shown) << " more\n";
                break;
            }
            out << "  " << describeEntity(db, *entity) << "\n";
            ++shown;
        }
    }

    return out.str();
}

std::string agentSystemPrompt(const CommandRegistry& registry) {
    std::ostringstream out;
    out << "You are editing a 2D CAD drawing in Slate.\n"
           "Answer with the commands that carry out the request, in a fenced block, one "
           "command per line: the command name followed by its inputs, separated by spaces. "
           "Points are written x,y and may be prefixed with @ to mean an offset from the "
           "previous point. Angles are in degrees.\n\n"
           "For example:\n"
           "```\n"
           "RECTANGLE 0,0 400,300\n"
           "CIRCLE 200,150 50\n"
           "```\n\n"
           "Rules:\n"
           "- Use only the commands listed below. Anything else will be refused.\n"
           "- Put any explanation outside the fenced block, not in it.\n"
           "- If the request cannot be done with these commands, say so and give no "
           "commands rather than guessing at something close.\n"
           "- Build on what is already in the drawing: keep to its units, place new work "
           "in relation to the existing objects, and reuse its layers and blocks rather "
           "than inventing new ones.\n"
           "- Where house standards are given, follow them: put each kind of object on the "
           "layer the office uses for it, with that layer's colour and linetype, and use "
           "the office's block names. Make a standard layer with LAYER if the drawing lacks "
           "it.\n\n"
           "Available commands:\n";

    for (const auto& [category, commands] : registry.byCategory()) {
        out << category << ": ";
        bool first = true;
        for (const CommandDefinition* command : commands) {
            if (command == nullptr) {
                continue;
            }
            out << (first ? "" : ", ") << command->name;
            first = false;
        }
        out << "\n";
    }
    return out.str();
}

AgentPlan requestAgentPlan(AgentProvider& provider, const std::string& instruction,
                           const Database& db, const CommandRegistry& registry,
                           std::span<const Handle> selection, std::string* error,
                           std::string_view standards) {
    AgentRequest request;
    request.system = agentSystemPrompt(registry);
    request.context = describeDrawing(db, selection);
    if (!standards.empty()) {
        request.context += "\n";
        request.context += standards;
    }
    request.instruction = instruction;

    const AgentReply reply = provider.send(request);
    if (!reply.ok()) {
        if (error != nullptr) {
            *error = reply.error;
        }
        return {};
    }

    AgentPlan plan = parseAgentPlan(reply.text);
    validateAgentPlan(plan, registry);
    return plan;
}

}  // namespace cad
