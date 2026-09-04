/// LAYER — make a layer, and make it the one being drawn on.
///
/// Layers were readable, renderable and round-trippable from the first day, and
/// there was no way to *make* one. A drawing started here could only ever have
/// layer 0, which for a drawing set is not a limitation so much as a refusal:
/// what is on which layer is how a sheet is plotted, frozen, coloured and
/// handed to somebody else.

#include <algorithm>
#include <cctype>

#include "cad/command/StandardCommands.hpp"
#include "cad/db/Layer.hpp"

namespace cad {
namespace {

/// LAYER: name it, colour it, draw on it.
///
/// One command rather than the dozen sub-options other CAD software offers. Make-or-set is what a
/// drafter reaches for a hundred times a day and every other layer operation --
/// freezing, locking, plotting -- already has a control in the layer panel.
class LayerCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& /*context*/) override {
        InputRequest request;
        request.kind = InputRequest::Kind::Text;
        request.prompt = "Layer name to draw on";
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }

        if (!named_) {
            if (input.kind != InputValue::Kind::Text || input.text.empty()) {
                return StepResult::cancelled();
            }
            name_ = input.text;
            named_ = true;

            // An existing layer is simply selected. Asking for its colour would
            // invite changing a drawing standard by accident, when all that was
            // wanted was to draw on it.
            if (context.db().findLayer(name_) != nullptr) {
                return apply(context);
            }

            InputRequest request;
            request.kind = InputRequest::Kind::Number;
            request.prompt = "Colour number for the new layer";
            request.defaultNumber = 7.0;
            request.allowEmpty = true;
            request.integerOnly = true;
            request.minimum = 1.0;
            request.maximum = 255.0;
            return StepResult::ask(request);
        }

        if (input.kind == InputValue::Kind::Number) {
            color_ = static_cast<int>(input.number);
        }
        return apply(context);
    }

private:
    StepResult apply(CommandContext& context) {
        Database& db = context.db();
        Layer* layer = db.findLayer(name_);
        const bool made = layer == nullptr;

        if (made) {
            auto fresh = std::make_unique<Layer>();
            fresh->name = name_;
            fresh->color = Color::fromIndex(color_);
            fresh->linetypeHandle = db.linetypes().find(kLinetypeContinuous);
            // Through the transaction, so making a layer undoes with whatever
            // else the command that needed it did.
            layer = &static_cast<Layer&>(context.transaction().addObject(std::move(fresh)));
        }

        // The name rather than the handle, because that is what the drawing
        // stores and what a save writes to $CLAYER.
        context.transaction().database().settings.currentLayerName = layer->name;

        return StepResult::done(made ? "Layer \"" + layer->name + "\" made and current"
                                     : "Layer \"" + layer->name + "\" is current");
    }

    std::string name_;
    bool named_ = false;
    int color_ = 7;
};

/// The layer tools: what a drafter does to layers by pointing at objects.
///
/// Off, Freeze, Lock and Unlock act on the layers of the selected objects;
/// Isolate turns every *other* layer off and Unisolate turns those back on;
/// AllOn and ThawAll take no selection. All go through the transaction, so a
/// wrong LAYISO is one undo away.
enum class LayerAction { Off, Freeze, Lock, Unlock, Isolate, Unisolate, AllOn, ThawAll };

class LayerToolCommand final : public CommandInstance {
public:
    explicit LayerToolCommand(LayerAction action) : action_(action) {}

    StepResult start(CommandContext& context) override {
        if (!needsSelection()) {
            return apply({}, context);
        }
        if (!context.initialSelection().empty()) {
            return apply(context.initialSelection(), context);
        }
        InputRequest request;
        request.kind = InputRequest::Kind::Selection;
        request.prompt = std::string{"Select objects on the layer(s) to "} + verb();
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }
        if (input.kind != InputValue::Kind::Selection || input.selection.empty()) {
            return StepResult::cancelled();
        }
        return apply(input.selection, context);
    }

private:
    [[nodiscard]] bool needsSelection() const {
        return action_ != LayerAction::Unisolate && action_ != LayerAction::AllOn &&
               action_ != LayerAction::ThawAll;
    }

    [[nodiscard]] const char* verb() const {
        switch (action_) {
            case LayerAction::Off: return "turn off";
            case LayerAction::Freeze: return "freeze";
            case LayerAction::Lock: return "lock";
            case LayerAction::Unlock: return "unlock";
            case LayerAction::Isolate: return "isolate";
            default: return "change";
        }
    }

    /// The distinct layers of the selected objects, in selection order.
    [[nodiscard]] static std::vector<Layer*> layersOf(const std::vector<Handle>& selection,
                                                      Database& db) {
        std::vector<Layer*> layers;
        for (const Handle handle : selection) {
            const Entity* entity = db.getEntity(handle);
            if (entity == nullptr || entity->erased) {
                continue;
            }
            Layer* layer = db.get<Layer>(entity->layerHandle);
            if (layer != nullptr && std::find(layers.begin(), layers.end(), layer) == layers.end()) {
                layers.push_back(layer);
            }
        }
        return layers;
    }

    [[nodiscard]] static std::string names(const std::vector<Layer*>& layers) {
        std::string list;
        for (const Layer* layer : layers) {
            list += (list.empty() ? "" : ", ") + layer->name;
        }
        return list;
    }

    StepResult apply(const std::vector<Handle>& selection, CommandContext& context) {
        Database& db = context.db();
        Transaction& transaction = context.transaction();
        const std::string current = db.settings.currentLayerName;
        std::vector<Layer*> chosen = layersOf(selection, db);
        std::vector<Layer*> changed;

        switch (action_) {
            case LayerAction::Off:
                for (Layer* layer : chosen) {
                    // The current layer stays on: turning it off hides what is
                    // drawn next, and other CAD software asks before doing that. Here it
                    // is skipped and said.
                    if (layer->name == current) {
                        context.warn("Layer \"" + layer->name + "\" is current and stays on.");
                        continue;
                    }
                    if (!layer->isOff) {
                        transaction.openForWrite(*layer).isOff = true;
                        changed.push_back(layer);
                    }
                }
                return report("turned off", changed);

            case LayerAction::Freeze:
                for (Layer* layer : chosen) {
                    if (layer->name == current) {
                        context.warn("Layer \"" + layer->name + "\" is current and cannot be frozen.");
                        continue;
                    }
                    if (!layer->isFrozen) {
                        transaction.openForWrite(*layer).isFrozen = true;
                        changed.push_back(layer);
                    }
                }
                return report("frozen", changed);

            case LayerAction::Lock:
                for (Layer* layer : chosen) {
                    if (!layer->isLocked) {
                        transaction.openForWrite(*layer).isLocked = true;
                        changed.push_back(layer);
                    }
                }
                return report("locked", changed);

            case LayerAction::Unlock:
                for (Layer* layer : chosen) {
                    if (layer->isLocked) {
                        transaction.openForWrite(*layer).isLocked = false;
                        changed.push_back(layer);
                    }
                }
                return report("unlocked", changed);

            case LayerAction::Isolate: {
                if (chosen.empty()) {
                    return StepResult::failed("Nothing selected to isolate.");
                }
                // Everything not chosen goes off, and is remembered so that
                // LAYUNISO puts back exactly that -- not layers that were off
                // before, which stay off.
                std::vector<Handle> hidden;
                for (const Handle handle : db.layers().handles()) {
                    Layer* layer = db.get<Layer>(handle);
                    if (layer == nullptr || layer->isOff ||
                        std::find(chosen.begin(), chosen.end(), layer) != chosen.end()) {
                        continue;
                    }
                    transaction.openForWrite(*layer).isOff = true;
                    hidden.push_back(layer->handle);
                    changed.push_back(layer);
                }
                db.settings.isolatedLayers = std::move(hidden);
                // The first chosen layer becomes current, so what is drawn
                // next is on a layer that shows.
                db.settings.currentLayerName = chosen.front()->name;
                return StepResult::done("Isolated " + names(chosen) + "; " +
                                        std::to_string(changed.size()) + " layer(s) turned off");
            }

            case LayerAction::Unisolate:
                for (const Handle handle : db.settings.isolatedLayers) {
                    Layer* layer = db.get<Layer>(handle);
                    if (layer != nullptr && layer->isOff) {
                        transaction.openForWrite(*layer).isOff = false;
                        changed.push_back(layer);
                    }
                }
                db.settings.isolatedLayers.clear();
                if (changed.empty()) {
                    return StepResult::done("No isolation to undo");
                }
                return report("turned back on", changed);

            case LayerAction::AllOn:
                for (const Handle handle : db.layers().handles()) {
                    Layer* layer = db.get<Layer>(handle);
                    if (layer != nullptr && layer->isOff) {
                        transaction.openForWrite(*layer).isOff = false;
                        changed.push_back(layer);
                    }
                }
                db.settings.isolatedLayers.clear();
                return report("turned on", changed);

            case LayerAction::ThawAll:
                for (const Handle handle : db.layers().handles()) {
                    Layer* layer = db.get<Layer>(handle);
                    if (layer != nullptr && layer->isFrozen) {
                        transaction.openForWrite(*layer).isFrozen = false;
                        changed.push_back(layer);
                    }
                }
                return report("thawed", changed);
        }
        return StepResult::cancelled();
    }

    [[nodiscard]] static StepResult report(const char* did, const std::vector<Layer*>& changed) {
        if (changed.empty()) {
            return StepResult::done(std::string{"No layer "} + did);
        }
        return StepResult::done(std::to_string(changed.size()) + " layer(s) " + did + ": " +
                                names(changed));
    }

    LayerAction action_;
};

[[nodiscard]] CommandDefinition layerTool(std::string id, std::string name,
                                          std::vector<std::string> aliases, std::string title,
                                          std::string description, LayerAction action) {
    CommandDefinition definition;
    definition.id = std::move(id);
    definition.name = std::move(name);
    definition.aliases = std::move(aliases);
    definition.title = std::move(title);
    definition.description = std::move(description);
    definition.category = "Tools";
    definition.supportsPickfirst = true;
    definition.create = [action] { return std::make_unique<LayerToolCommand>(action); };
    return definition;
}

}  // namespace

std::vector<CommandDefinition> layerCommands() {
    CommandDefinition layer;
    layer.id = "cad.layer.make";
    layer.name = "LAYER";
    layer.aliases = {"LA", "CLAYER", "LAYMCUR"};
    layer.title = "Layer";
    layer.description = "Make a layer, or draw on an existing one";
    layer.category = "Tools";
    layer.create = [] { return std::make_unique<LayerCommand>(); };

    std::vector<CommandDefinition> commands;
    commands.push_back(std::move(layer));
    commands.push_back(layerTool("cad.layer.isolate", "LAYISO", {}, "Isolate",
                                 "Turn off every layer but the selected objects'",
                                 LayerAction::Isolate));
    commands.push_back(layerTool("cad.layer.unisolate", "LAYUNISO", {}, "Unisolate",
                                 "Turn back on the layers LAYISO turned off",
                                 LayerAction::Unisolate));
    commands.push_back(layerTool("cad.layer.off", "LAYOFF", {}, "Off",
                                 "Turn off the selected objects' layers", LayerAction::Off));
    commands.push_back(layerTool("cad.layer.on", "LAYON", {}, "All On", "Turn every layer on",
                                 LayerAction::AllOn));
    commands.push_back(layerTool("cad.layer.freeze", "LAYFRZ", {}, "Freeze",
                                 "Freeze the selected objects' layers", LayerAction::Freeze));
    commands.push_back(layerTool("cad.layer.thaw", "LAYTHW", {}, "Thaw All",
                                 "Thaw every layer", LayerAction::ThawAll));
    commands.push_back(layerTool("cad.layer.lock", "LAYLCK", {}, "Lock",
                                 "Lock the selected objects' layers", LayerAction::Lock));
    commands.push_back(layerTool("cad.layer.unlock", "LAYULK", {}, "Unlock",
                                 "Unlock the selected objects' layers", LayerAction::Unlock));
    return commands;
}

}  // namespace cad
