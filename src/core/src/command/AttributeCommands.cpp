/// Attributes: the tagged text a block carries, filled in per insert.
///
/// ATTDEF makes a definition -- tag, prompt, default and modes -- in the
/// current space, where BLOCK will sweep it into a definition. INSERT then
/// asks for a value per definition (see BlockCommands.cpp). ATTEDIT changes
/// the values on a reference already placed.

#include <cctype>
#include <string>

#include "cad/command/StandardCommands.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/db/TextStyle.hpp"

namespace cad {
namespace {

[[nodiscard]] bool matchesKeyword(const std::string& typed, char initial) {
    return !typed.empty() &&
           std::toupper(static_cast<unsigned char>(typed.front())) == initial;
}

[[nodiscard]] StepResult askText(std::string prompt, std::optional<std::string> fallback = {},
                                 bool allowEmpty = false) {
    InputRequest request;
    request.kind = InputRequest::Kind::Text;
    request.prompt = std::move(prompt);
    request.defaultText = std::move(fallback);
    request.allowEmpty = allowEmpty;
    return StepResult::ask(request);
}

[[nodiscard]] std::string textOf(const InputValue& input) {
    if (input.kind == InputValue::Kind::Text || input.kind == InputValue::Kind::Keyword) {
        return input.text;
    }
    return {};
}

const CommandKeyword kInvisibleKeyword{"Invisible", "I", "Not shown on an insert"};
const CommandKeyword kConstantKeyword{"Constant", "C", "Always the default; not asked for"};
const CommandKeyword kVerifyKeyword{"Verify", "V", "Asked to confirm on insert"};
const CommandKeyword kPresetKeyword{"Preset", "P", "Takes the default without asking"};

// ---------------------------------------------------------------- ATTDEF

/// ATTDEF: an attribute definition. Modes first, as -ATTDEF asks them, then
/// tag, prompt and default, then the text's place, height and angle.
class AttributeDefinitionCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& /*context*/) override { return askModes(); }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }
        switch (stage_) {
            case Stage::Modes:
                if (input.kind == InputValue::Kind::Keyword ||
                    (input.kind == InputValue::Kind::Text && !input.text.empty())) {
                    if (matchesKeyword(input.text, 'I')) {
                        flags_.isInvisible = !flags_.isInvisible;
                    } else if (matchesKeyword(input.text, 'C')) {
                        flags_.isConstant = !flags_.isConstant;
                    } else if (matchesKeyword(input.text, 'V')) {
                        flags_.isVerifiable = !flags_.isVerifiable;
                    } else if (matchesKeyword(input.text, 'P')) {
                        flags_.isPreset = !flags_.isPreset;
                    } else {
                        return StepResult::failed("Invisible, Constant, Verify or Preset.");
                    }
                    return askModes();
                }
                stage_ = Stage::Tag;
                return askText("Enter attribute tag name");

            case Stage::Tag: {
                std::string tag = textOf(input);
                if (tag.empty()) {
                    return StepResult::cancelled();
                }
                // Tags are single words, upper case, as other CAD software keeps them: a
                // tag is a name a field refers to, not a label.
                for (char& c : tag) {
                    c = std::isspace(static_cast<unsigned char>(c))
                            ? '_'
                            : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                }
                tag_ = tag;
                stage_ = Stage::Prompt;
                if (flags_.isConstant) {
                    // A constant attribute is never asked for, so it has no prompt.
                    stage_ = Stage::Default;
                    return askText("Enter attribute value", {}, true);
                }
                return askText("Enter attribute prompt", {}, true);
            }

            case Stage::Prompt:
                prompt_ = textOf(input);
                stage_ = Stage::Default;
                return askText("Enter default attribute value", {}, true);

            case Stage::Default: {
                default_ = textOf(input);
                stage_ = Stage::Position;
                InputRequest request;
                request.kind = InputRequest::Kind::Point;
                request.prompt = "Specify start point of text";
                return StepResult::ask(request);
            }

            case Stage::Position: {
                if (input.kind != InputValue::Kind::Point) {
                    InputRequest request;
                    request.kind = InputRequest::Kind::Point;
                    request.prompt = "Specify start point of text";
                    return StepResult::ask(request);
                }
                position_ = input.point;
                stage_ = Stage::Height;
                InputRequest request;
                request.kind = InputRequest::Kind::Distance;
                request.prompt = "Specify height";
                request.defaultNumber = defaultHeight(context);
                request.basePoint = position_;
                return StepResult::ask(request);
            }

            case Stage::Height: {
                if (input.kind == InputValue::Kind::Number) {
                    if (input.number <= 0.0) {
                        return StepResult::failed("The height must be positive.");
                    }
                    height_ = input.number;
                } else if (input.kind == InputValue::Kind::Point) {
                    height_ = position_.distanceTo(input.point);
                } else {
                    height_ = defaultHeight(context);
                }
                stage_ = Stage::Rotation;
                InputRequest request;
                request.kind = InputRequest::Kind::Angle;
                request.prompt = "Specify rotation angle of text";
                request.defaultNumber = 0.0;
                request.basePoint = position_;
                return StepResult::ask(request);
            }

            case Stage::Rotation: {
                double rotation = 0.0;
                if (input.kind == InputValue::Kind::Number) {
                    rotation = input.number;
                } else if (input.kind == InputValue::Kind::Point) {
                    rotation = (input.point - position_).angle2();
                }
                auto definition = std::make_unique<AttributeDefinitionEntity>();
                definition->position = position_;
                definition->contents = default_;
                definition->defaultValue = default_;
                definition->tag = tag_;
                definition->prompt = prompt_;
                definition->flags = flags_;
                definition->height = height_;
                definition->rotation = rotation;
                definition->styleHandle = currentStyle(context).handle;
                context.transaction().addToCurrentSpace(std::move(definition));
                return StepResult::done("Attribute " + tag_ + " defined");
            }
        }
        return StepResult::cancelled();
    }

private:
    enum class Stage { Modes, Tag, Prompt, Default, Position, Height, Rotation };

    [[nodiscard]] static const TextStyle& currentStyle(CommandContext& context) {
        return context.db().ensureTextStyle(context.db().settings.currentTextStyleName);
    }

    /// The style's fixed height when it has one, else the usual 2.5.
    [[nodiscard]] static double defaultHeight(CommandContext& context) {
        const double fixed = currentStyle(context).height;
        return fixed > 0.0 ? fixed : 2.5;
    }

    [[nodiscard]] StepResult askModes() const {
        const auto yesNo = [](bool on) { return on ? "Y" : "N"; };
        InputRequest request;
        request.kind = InputRequest::Kind::Text;
        request.prompt = std::string{"Attribute modes: Invisible="} + yesNo(flags_.isInvisible) +
                         " Constant=" + yesNo(flags_.isConstant) +
                         " Verify=" + yesNo(flags_.isVerifiable) +
                         " Preset=" + yesNo(flags_.isPreset) +
                         ". Enter a mode to change [Invisible/Constant/Verify/Preset] <done>";
        request.keywords = {kInvisibleKeyword, kConstantKeyword, kVerifyKeyword, kPresetKeyword};
        request.allowEmpty = true;
        return StepResult::ask(request);
    }

    Stage stage_ = Stage::Modes;
    AttributeFlags flags_;
    std::string tag_;
    std::string prompt_;
    std::string default_;
    Vec3 position_;
    double height_ = 2.5;
};

// --------------------------------------------------------------- ATTEDIT

/// ATTEDIT: new values for the attributes of one block reference, asked one
/// by one with the current value as the default.
class AttributeEditCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& context) override {
        // A reference already selected is the one meant.
        for (const Handle handle : context.initialSelection()) {
            if (const auto* insert = context.db().get<InsertEntity>(handle);
                insert != nullptr && !insert->erased) {
                return begin(*insert, context);
            }
        }
        InputRequest request;
        request.kind = InputRequest::Kind::Entity;
        request.prompt = "Select block reference";
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }
        if (insert_.isNull()) {
            Handle picked;
            if (input.kind == InputValue::Kind::Entity) {
                picked = input.entity;
            } else if (input.kind == InputValue::Kind::Selection && !input.selection.empty()) {
                picked = input.selection.front();
            }
            const auto* insert = context.db().get<InsertEntity>(picked);
            if (insert == nullptr || insert->erased) {
                return StepResult::failed("That is not a block reference.");
            }
            return begin(*insert, context);
        }

        // A value for the attribute currently asked about.
        Entity* entity = context.db().getEntity(pending_[next_]);
        if (auto* attribute = dynamic_cast<AttributeEntity*>(entity); attribute != nullptr) {
            if (input.kind == InputValue::Kind::Text && input.text != attribute->value()) {
                context.transaction().openForWrite(*attribute).setValue(input.text);
                ++changed_;
            }
        }
        ++next_;
        return askNext(context);
    }

private:
    StepResult begin(const InsertEntity& insert, CommandContext& context) {
        insert_ = insert.handle;
        for (const Handle handle : insert.attributeHandles) {
            const auto* attribute = context.db().get<AttributeEntity>(handle);
            if (attribute != nullptr && !attribute->erased && !attribute->flags.isConstant) {
                pending_.push_back(handle);
            }
        }
        if (pending_.empty()) {
            return StepResult::failed("That block reference has no editable attributes.");
        }
        return askNext(context);
    }

    StepResult askNext(CommandContext& context) {
        if (next_ >= pending_.size()) {
            return StepResult::done(std::to_string(changed_) + " attribute value(s) changed");
        }
        const auto* attribute = context.db().get<AttributeEntity>(pending_[next_]);
        if (attribute == nullptr) {
            ++next_;
            return askNext(context);
        }
        return askText("Enter new value for " + attribute->tag, attribute->value(), true);
    }

    Handle insert_;
    std::vector<Handle> pending_;
    std::size_t next_ = 0;
    int changed_ = 0;
};

[[nodiscard]] CommandDefinition define(std::string id, std::string name,
                                       std::vector<std::string> aliases, std::string title,
                                       std::string description,
                                       std::function<CommandInstancePtr()> create,
                                       bool pickfirst) {
    CommandDefinition definition;
    definition.id = std::move(id);
    definition.name = std::move(name);
    definition.aliases = std::move(aliases);
    definition.title = std::move(title);
    definition.description = std::move(description);
    definition.category = "Insert";
    definition.create = std::move(create);
    definition.supportsPickfirst = pickfirst;
    return definition;
}

}  // namespace

std::vector<CommandDefinition> attributeCommands() {
    std::vector<CommandDefinition> commands;
    commands.push_back(define("cad.block.attdef", "ATTDEF", {"ATT", "-ATTDEF"}, "Define Attribute",
                              "Tagged text a block asks for on insert",
                              [] { return std::make_unique<AttributeDefinitionCommand>(); },
                              false));
    commands.push_back(define("cad.block.attedit", "ATTEDIT", {"ATE", "EATTEDIT", "-ATTEDIT"},
                              "Edit Attributes", "Change the attribute values of a block reference",
                              [] { return std::make_unique<AttributeEditCommand>(); }, true));
    return commands;
}

}  // namespace cad
