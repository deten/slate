#include "cad/agent/AgentProvider.hpp"

#include <algorithm>
#include <map>
#include <sstream>

#include "cad/command/CommandRegistry.hpp"
#include "cad/db/BlockTableRecord.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/Layer.hpp"

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

}  // namespace

std::string describeDrawing(const Database& db, std::span<const Handle> selection) {
    std::ostringstream out;

    const auto& model = const_cast<Database&>(db).modelSpace();
    const std::vector<const Entity*> entities = db.entitiesIn(model);

    std::map<std::string, int> counts;
    Box3 extents;
    for (const Entity* entity : entities) {
        if (entity == nullptr || entity->erased) {
            continue;
        }
        ++counts[entity->objectType()];
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
        out << "Extents: " << rounded(extents.min.x) << "," << rounded(extents.min.y) << " to "
            << rounded(extents.max.x) << "," << rounded(extents.max.y) << "\n";
    } else {
        out << "The drawing is empty.\n";
    }

    out << "Current layer: " << db.settings.currentLayerName << "\n";

    std::vector<std::string> layerNames;
    for (const Handle handle : db.layers().handles()) {
        if (const auto* layer = db.get<Layer>(handle); layer != nullptr && !layer->erased) {
            layerNames.push_back(layer->name);
        }
    }
    std::sort(layerNames.begin(), layerNames.end());
    out << "Layers (" << layerNames.size() << "):";
    // Capped. A drawing can carry hundreds, and a list that long crowds out the
    // instruction it is supposed to be supporting.
    constexpr std::size_t kMaxLayers = 40;
    for (std::size_t i = 0; i < layerNames.size() && i < kMaxLayers; ++i) {
        out << (i == 0 ? " " : ", ") << layerNames[i];
    }
    if (layerNames.size() > kMaxLayers) {
        out << ", and " << (layerNames.size() - kMaxLayers) << " more";
    }
    out << "\n";

    if (!selection.empty()) {
        out << "Selected: " << selection.size() << " object(s)";
        Box3 selected;
        for (const Handle handle : selection) {
            if (const Entity* entity = db.getEntity(handle);
                entity != nullptr && !entity->erased) {
                selected = selected.united(entity->bounds());
            }
        }
        if (!selected.isEmpty()) {
            out << ", spanning " << rounded(selected.min.x) << "," << rounded(selected.min.y)
                << " to " << rounded(selected.max.x) << "," << rounded(selected.max.y);
        }
        out << "\n";
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
           "commands rather than guessing at something close.\n\n"
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
                           std::span<const Handle> selection, std::string* error) {
    AgentRequest request;
    request.system = agentSystemPrompt(registry);
    request.context = describeDrawing(db, selection);
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
