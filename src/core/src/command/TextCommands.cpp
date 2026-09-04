/// TEXT and MTEXT — creating annotation.
///
/// The application could already read, render and edit text before these
/// existed; what it could not do was write any, which for a drafting product is
/// a hole rather than a missing convenience. Half of what goes on a real sheet
/// is notes, labels and callouts.
///
/// Both commands take their style from the drawing's current text style and
/// skip the height prompt when that style fixes the height — the rule other CAD software
/// uses, and the reason a drawing set with a house style needs one fewer answer
/// per note.

#include <cmath>
#include <functional>
#include <optional>
#include <cctype>
#include <string>
#include <utility>

#include "cad/command/StandardCommands.hpp"
#include "cad/db/DimensionEntity.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/db/TextStyle.hpp"

namespace cad {
namespace {

/// Fallback when the current style leaves the height to the object and the
/// user has not yet said what it should be.
constexpr double kDefaultTextHeight = 2.5;

/// The current text style, creating it if the name does not resolve.
[[nodiscard]] const TextStyle& currentStyle(CommandContext& context) {
    return context.db().ensureTextStyle(context.db().settings.currentTextStyleName);
}

[[nodiscard]] StepResult askHeight(double fallback) {
    InputRequest request;
    request.kind = InputRequest::Kind::Distance;
    request.prompt = "Specify text height";
    request.defaultNumber = fallback;
    // A zero-height text object is invisible rather than small, so it is
    // refused here rather than becoming a drawing full of nothing.
    request.minimum = 0.0;
    return StepResult::ask(request);
}

[[nodiscard]] StepResult askContents() {
    InputRequest request;
    // Kind::Text is what tells the command line that a space is a space rather
    // than the key that submits a command name.
    request.kind = InputRequest::Kind::Text;
    request.prompt = "Enter text";
    request.allowEmpty = true;
    return StepResult::ask(request);
}

/// Shared scaffolding: acquire a height, then a string.
///
/// TEXT and MTEXT differ only in how they are placed. Factoring out the rest
/// keeps the height rule, the empty-string rule and cancellation identical
/// between them, which is what stops the two feeling like different products.
class TextCommandBase : public CommandInstance {
protected:
    /// Settle the height from the current style, or ask for one.
    ///
    /// Returns the request when the user must answer, and nothing when the
    /// style already fixes the height. Deliberately *not* returning the next
    /// prompt itself: doing that made this decide what came after the height,
    /// which for TEXT is the rotation and for MTEXT the contents. It got that
    /// wrong, and the failure was quiet — the command line asked for text while
    /// the state machine was still waiting for a rotation, so a note typed
    /// under a fixed-height style was swallowed as the angle.
    [[nodiscard]] std::optional<StepResult> beginHeight(CommandContext& context) {
        const TextStyle& style = currentStyle(context);
        styleHandle_ = style.handle;
        if (!style.isHeightVariable()) {
            height_ = style.height;
            hasHeight_ = true;
            return std::nullopt;
        }
        return askHeight(kDefaultTextHeight);
    }

    /// Consume a height answer. Returns true when the height is settled and the
    /// caller should move on to the contents.
    bool takeHeight(const InputValue& input) {
        if (hasHeight_) {
            return true;
        }
        height_ = input.kind == InputValue::Kind::Number ? input.number : kDefaultTextHeight;
        if (height_ <= 0.0) {
            height_ = kDefaultTextHeight;
        }
        hasHeight_ = true;
        return false;
    }

    [[nodiscard]] bool hasHeight() const noexcept { return hasHeight_; }
    [[nodiscard]] double height() const noexcept { return height_; }
    [[nodiscard]] Handle styleHandle() const noexcept { return styleHandle_; }

private:
    double height_ = kDefaultTextHeight;
    bool hasHeight_ = false;
    Handle styleHandle_;
};

/// TEXT — a single line of text placed at a point.
///
/// Keeps prompting for further lines, stacking each one below the last, so a
/// short note is one command rather than one command per line. That is how
/// other CAD software's TEXT behaves and it is markedly faster than it sounds.
class TextCommand final : public TextCommandBase {
public:
    StepResult start(CommandContext& /*context*/) override {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = "Specify start point of text";
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return finish();
        }

        if (!hasPosition_) {
            if (input.kind != InputValue::Kind::Point) {
                return StepResult::cancelled();
            }
            position_ = input.point;
            hasPosition_ = true;
            if (std::optional<StepResult> ask = beginHeight(context)) {
                return *ask;
            }
            return askRotation();
        }

        if (!hasHeight()) {
            takeHeight(input);
            return askRotation();
        }

        if (!hasRotation_) {
            // A number is the angle in radians; a point is a direction picked
            // off the drawing. ROTATE accepts both and so does this, because a
            // prompt that takes an angle should mean the same thing everywhere.
            if (input.kind == InputValue::Kind::Number) {
                rotation_ = input.number;
            } else if (input.kind == InputValue::Kind::Point) {
                rotation_ = (input.point - position_).angle2();
            }
            hasRotation_ = true;
            return askContents();
        }

        // An empty line ends the command, which is what makes "keep prompting"
        // usable: Enter on a blank line is how a drafter stops.
        if (input.kind != InputValue::Kind::Text || input.text.empty()) {
            return finish();
        }

        auto entity = std::make_unique<TextEntity>(position_, input.text, height());
        entity->rotation = rotation_;
        entity->styleHandle = styleHandle();
        context.transaction().addToCurrentSpace(std::move(entity));
        ++placed_;

        // Stack the next line below this one, along the text's own baseline
        // direction so a rotated note stacks along its own axis rather than
        // straight down the sheet.
        const double lineHeight = height() * 1.5;
        position_ = position_ + Vec3{std::sin(rotation_) * lineHeight,
                                     -std::cos(rotation_) * lineHeight, 0.0};
        return askContents();
    }

private:
    [[nodiscard]] static StepResult askRotation() {
        InputRequest request;
        request.kind = InputRequest::Kind::Angle;
        request.prompt = "Specify rotation angle of text";
        request.defaultNumber = 0.0;
        return StepResult::ask(request);
    }

    [[nodiscard]] StepResult finish() const {
        if (placed_ == 0) {
            return StepResult::cancelled();
        }
        return StepResult::done(std::to_string(placed_) + " line(s) of text placed");
    }

    Vec3 position_;
    bool hasPosition_ = false;
    double rotation_ = 0.0;
    bool hasRotation_ = false;
    int placed_ = 0;
};

/// MTEXT — a paragraph placed in a rectangle.
///
/// The rectangle is what distinguishes MTEXT from TEXT: its width is the wrap
/// column, and the corner picked first is the corner the text hangs from. Both
/// are recorded rather than normalised away, so dragging the box later means
/// something.
class MTextCommand final : public TextCommandBase {
public:
    StepResult start(CommandContext& /*context*/) override {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = "Specify first corner";
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }

        if (!hasFirstCorner_) {
            if (input.kind != InputValue::Kind::Point) {
                return StepResult::cancelled();
            }
            firstCorner_ = input.point;
            hasFirstCorner_ = true;

            InputRequest request;
            request.kind = InputRequest::Kind::Point;
            request.prompt = "Specify opposite corner";
            request.basePoint = firstCorner_;
            return StepResult::ask(request);
        }

        if (!hasSecondCorner_) {
            if (input.kind != InputValue::Kind::Point) {
                return StepResult::cancelled();
            }
            secondCorner_ = input.point;
            hasSecondCorner_ = true;
            if (std::optional<StepResult> ask = beginHeight(context)) {
                return *ask;
            }
            return askContents();
        }

        if (!hasHeight()) {
            takeHeight(input);
            return askContents();
        }

        if (input.kind != InputValue::Kind::Text || input.text.empty()) {
            return StepResult::cancelled("No text entered");
        }

        const Box3 box = Box3::fromCorners(firstCorner_, secondCorner_);
        // Attached at the top-left and wrapped to the box width, which is what
        // dragging a rectangle left-to-right means everywhere else in CAD.
        auto entity = std::make_unique<MTextEntity>(Vec3{box.min.x, box.max.y, box.min.z},
                                                    input.text, height());
        entity->width = box.width();
        entity->attachment = MTextAttachment::TopLeft;
        entity->styleHandle = styleHandle();
        context.transaction().addToCurrentSpace(std::move(entity));
        return StepResult::done();
    }

private:
    Vec3 firstCorner_;
    Vec3 secondCorner_;
    bool hasFirstCorner_ = false;
    bool hasSecondCorner_ = false;
};

// ------------------------------------------------------------------ editing

/// The editable string an annotation object carries, if it has one.
///
/// A drafter double-clicks a label and expects to change the words. What the
/// words *are* differs by type — a TEXT holds them plainly, an MTEXT holds them
/// with formatting codes, a dimension holds an override that is empty when the
/// measurement speaks for itself — so the type question is answered once, here,
/// rather than in every caller.
struct EditableText {
    /// What the user sees and amends.
    std::string contents;
    /// Written back by `apply`, which knows which field it came from.
    std::function<void(Transaction&, Entity&, const std::string&)> apply;
    /// For the prompt, so it is clear what is being edited.
    std::string description;
};

}  // namespace

bool hasEditableText(const Entity& entity) {
    // The same three types `editableTextOf` handles, asked as a question rather
    // than answered with a way to write. A double-click has to know whether
    // TEXTEDIT will have anything to do *before* starting it: a command that
    // opens and then asks the user to find it something to work on is worse
    // than one that never opened.
    return dynamic_cast<const MTextEntity*>(&entity) != nullptr ||
           dynamic_cast<const TextEntity*>(&entity) != nullptr ||
           dynamic_cast<const DimensionEntity*>(&entity) != nullptr;
}

namespace {

[[nodiscard]] std::optional<EditableText> editableTextOf(Entity& entity) {
    if (auto* mtext = dynamic_cast<MTextEntity*>(&entity)) {
        return EditableText{mtext->contents,
                            [](Transaction& transaction, Entity& target,
                               const std::string& value) {
                                transaction.openForWrite(static_cast<MTextEntity&>(target))
                                    .contents = value;
                            },
                            "paragraph text"};
    }
    // Attributes and attribute definitions derive from TextEntity, so this one
    // branch covers TEXT, ATTRIB and ATTDEF. An attribute's *value* is exactly
    // what a user wants to change when they double-click a title block.
    if (auto* text = dynamic_cast<TextEntity*>(&entity)) {
        return EditableText{text->contents,
                            [](Transaction& transaction, Entity& target,
                               const std::string& value) {
                                transaction.openForWrite(static_cast<TextEntity&>(target))
                                    .contents = value;
                            },
                            "text"};
    }
    if (auto* dimension = dynamic_cast<DimensionEntity*>(&entity)) {
        return EditableText{dimension->textOverride,
                            [](Transaction& transaction, Entity& target,
                               const std::string& value) {
                                transaction.openForWrite(static_cast<DimensionEntity&>(target))
                                    .textOverride = value;
                            },
                            "dimension text override"};
    }
    return std::nullopt;
}

/// TEXTEDIT — change the words in an annotation object.
///
/// Text could be created and its contents could be changed through the
/// properties palette, and neither of those is *editing text*. A drafter edits
/// a note by pointing at it, and the two-step path through a palette is why
/// annotation in a half-finished CAD program feels like data entry.
///
/// Repeats until the user presses Enter, which is other CAD software's DDEDIT: correcting
/// a sheet's worth of labels is one command, not one command per label.
class TextEditCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& context) override {
        // A pre-selected single object is the answer to the question this
        // command is about to ask, so it does not ask it.
        if (context.initialSelection().size() == 1) {
            if (Entity* entity = context.db().getEntity(context.initialSelection().front())) {
                if (std::optional<EditableText> editable = editableTextOf(*entity)) {
                    handle_ = entity->handle;
                    warnIfDerived(*entity, context);
                    return askContentsFor(*editable, handle_);
                }
            }
        }
        return askEntity();
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }
        // Enter with nothing selected ends the command, the way DDEDIT does.
        if (input.kind == InputValue::Kind::Empty && handle_.isNull()) {
            return StepResult::done();
        }

        if (handle_.isNull()) {
            if (input.kind != InputValue::Kind::Entity) {
                return StepResult::done();
            }
            Entity* entity = context.db().getEntity(input.entity);
            if (entity == nullptr) {
                return askEntity();
            }
            std::optional<EditableText> editable = editableTextOf(*entity);
            if (!editable) {
                // Named, not merely refused: "that is a polyline" tells the user
                // what to click instead, and "cannot edit that" does not.
                context.warn(entity->objectType() + " has no text to edit");
                return askEntity();
            }
            handle_ = entity->handle;
            warnIfDerived(*entity, context);
            return askContentsFor(*editable, handle_);
        }

        Entity* entity = context.db().getEntity(handle_);
        if (entity == nullptr) {
            handle_ = Handle{};
            return askEntity();
        }
        std::optional<EditableText> editable = editableTextOf(*entity);
        if (!editable) {
            handle_ = Handle{};
            return askEntity();
        }

        // Enter on an unchanged line means "leave it alone", not "empty it".
        // Deleting a label by pressing Enter would be a trap.
        if (input.kind == InputValue::Kind::Text && input.text != editable->contents) {
            editable->apply(context.transaction(), *entity, input.text);
            ++edited_;
        }

        handle_ = Handle{};
        return askEntity();
    }

private:
    /// Say so when the words are not the user's to keep.
    ///
    /// A field's text is *derived* — the number in "MASTER BEDROOM 423.08" comes
    /// from the area of a polyline, and the next UPDATEFIELD will put it back.
    /// Typing over it looks like it worked and silently will not last, which is
    /// the worst behaviour available; saying so costs one line.
    static void warnIfDerived(const Entity& entity, CommandContext& context) {
        if (!entity.fieldHandle.isNull()) {
            context.warn(
                "This text comes from a field and will be recomputed; edit the field instead");
        }
    }

    [[nodiscard]] StepResult askEntity() {
        InputRequest request;
        request.kind = InputRequest::Kind::Entity;
        request.prompt = edited_ == 0 ? "Select an annotation object to edit"
                                      : "Select another object to edit, or press Enter";
        request.allowEmpty = true;
        return StepResult::ask(request);
    }

    [[nodiscard]] static StepResult askContentsFor(const EditableText& editable, Handle entity) {
        InputRequest request;
        request.kind = InputRequest::Kind::Text;
        request.prompt = "Edit " + editable.description;
        request.allowEmpty = true;
        // The existing words, in the box, selected. This is the whole point.
        request.editText = editable.contents;
        request.defaultText = editable.contents;
        // And *which* words, so a shell that can edit them where they sit knows
        // where that is.
        request.editEntity = entity;
        return StepResult::ask(request);
    }

    Handle handle_;
    std::size_t edited_ = 0;
};

[[nodiscard]] CommandDefinition makeDefinition(std::string id, std::string name,
                                               std::vector<std::string> aliases,
                                               std::string title, std::string description,
                                               std::function<CommandInstancePtr()> create) {
    CommandDefinition definition;
    definition.id = std::move(id);
    definition.name = std::move(name);
    definition.aliases = std::move(aliases);
    definition.title = std::move(title);
    definition.description = std::move(description);
    definition.category = "Annotate";
    definition.supportsPickfirst = false;
    definition.create = std::move(create);
    return definition;
}

}  // namespace

/// STYLE: make or change a text style, and draw with it from now on.
///
/// The command-line form, as -STYLE asks it: name, font, height, width
/// factor, obliquing angle. A height of 0 leaves the height to each text
/// command, which is the usual arrangement. `?` lists the styles there are.
class TextStyleCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& context) override {
        InputRequest request;
        request.kind = InputRequest::Kind::Text;
        request.prompt = "Enter text style name or [?]";
        request.defaultText = context.db().settings.currentTextStyleName;
        request.allowEmpty = true;
        request.keywords = {CommandKeyword{"?", "?", "List the styles"}};
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }
        Database& db = context.db();
        switch (stage_) {
            case Stage::Name: {
                const std::string typed = input.kind == InputValue::Kind::Text ||
                                                  input.kind == InputValue::Kind::Keyword
                                              ? input.text
                                              : std::string{};
                if (typed == "?") {
                    for (const Handle handle : db.textStyles().handles()) {
                        if (const auto* style = db.get<TextStyle>(handle)) {
                            context.log("  " + style->name + "  " + style->fontFile +
                                        (style->height > 0.0 ? "  height " + std::to_string(style->height) : ""));
                        }
                    }
                    return start(context);
                }
                name_ = typed.empty() ? db.settings.currentTextStyleName : typed;
                // A new style starts from the current one, as other CAD software starts
                // one from Standard; an existing style shows its own values.
                const TextStyle& base = db.findTextStyle(name_) != nullptr
                                            ? *db.findTextStyle(name_)
                                            : db.ensureTextStyle(db.settings.currentTextStyleName);
                font_ = !base.typeface.empty() ? base.typeface : base.fontFile;
                height_ = base.height;
                width_ = base.widthFactor;
                oblique_ = base.obliqueAngle;
                stage_ = Stage::Font;
                return askText("Specify font file or name", font_);
            }
            case Stage::Font:
                if (input.kind == InputValue::Kind::Text && !input.text.empty()) {
                    font_ = input.text;
                }
                stage_ = Stage::Height;
                return askNumber("Specify height (0 for not fixed)", height_, InputRequest::Kind::Distance);
            case Stage::Height:
                if (input.kind == InputValue::Kind::Number) {
                    if (input.number < 0.0) {
                        return StepResult::failed("A height cannot be negative.");
                    }
                    height_ = input.number;
                }
                stage_ = Stage::Width;
                return askNumber("Specify width factor", width_, InputRequest::Kind::Number);
            case Stage::Width:
                if (input.kind == InputValue::Kind::Number) {
                    if (input.number <= 0.0) {
                        return StepResult::failed("A width factor must be positive.");
                    }
                    width_ = input.number;
                }
                stage_ = Stage::Oblique;
                return askNumber("Specify obliquing angle", oblique_, InputRequest::Kind::Angle);
            case Stage::Oblique:
                if (input.kind == InputValue::Kind::Number) {
                    oblique_ = input.number;
                }
                return apply(context);
        }
        return StepResult::cancelled();
    }

private:
    enum class Stage { Name, Font, Height, Width, Oblique };

    [[nodiscard]] static StepResult askText(std::string prompt, std::string fallback) {
        InputRequest request;
        request.kind = InputRequest::Kind::Text;
        request.prompt = std::move(prompt);
        request.defaultText = std::move(fallback);
        request.allowEmpty = true;
        return StepResult::ask(request);
    }

    [[nodiscard]] static StepResult askNumber(std::string prompt, double fallback,
                                              InputRequest::Kind kind) {
        InputRequest request;
        request.kind = kind;
        request.prompt = std::move(prompt);
        request.defaultNumber = fallback;
        request.allowEmpty = true;
        return StepResult::ask(request);
    }

    StepResult apply(CommandContext& context) {
        Database& db = context.db();
        Transaction& transaction = context.transaction();
        TextStyle* style = db.findTextStyle(name_);
        const bool made = style == nullptr;
        if (made) {
            auto fresh = std::make_unique<TextStyle>();
            fresh->name = name_;
            style = &static_cast<TextStyle&>(transaction.addObject(std::move(fresh)));
        }
        TextStyle& target = transaction.openForWrite(*style);
        // A file name is a font file; anything else is a typeface the system
        // provides, which is how a TrueType face is named.
        const std::string lower = [&] {
            std::string s = font_;
            for (char& c : s) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return s;
        }();
        if (lower.ends_with(".shx") || lower.ends_with(".ttf") || lower.ends_with(".otf")) {
            target.fontFile = font_;
            target.typeface.clear();
        } else {
            target.typeface = font_;
        }
        target.height = height_;
        target.widthFactor = width_;
        target.obliqueAngle = oblique_;
        db.settings.currentTextStyleName = target.name;
        return StepResult::done((made ? "Text style \"" : "Text style \"") + target.name +
                                (made ? "\" made and current" : "\" changed and current"));
    }

    Stage stage_ = Stage::Name;
    std::string name_;
    std::string font_;
    double height_ = 0.0;
    double width_ = 1.0;
    double oblique_ = 0.0;
};

std::vector<CommandDefinition> textCommands() {
    std::vector<CommandDefinition> commands;
    commands.push_back(makeDefinition("cad.annotate.style", "STYLE", {"ST", "-STYLE"}, "Text Style",
                                      "Make or change a text style and draw with it",
                                      [] { return std::make_unique<TextStyleCommand>(); }));
    commands.push_back(makeDefinition("cad.annotate.text", "TEXT", {"DT", "DTEXT"}, "Text",
                                      "Place single lines of text",
                                      [] { return std::make_unique<TextCommand>(); }));
    commands.push_back(makeDefinition("cad.annotate.mtext", "MTEXT", {"T", "MT"}, "Mtext",
                                      "Place a paragraph of text in a rectangle",
                                      [] { return std::make_unique<MTextCommand>(); }));

    CommandDefinition edit = makeDefinition(
        "cad.annotate.textedit", "TEXTEDIT", {"DDEDIT", "ED", "MTEDIT"}, "Edit Text",
        "Change the words in text, an attribute or a dimension override",
        [] { return std::make_unique<TextEditCommand>(); });
    // A selected label is the answer to "which object", so the command takes it
    // rather than asking again.
    edit.supportsPickfirst = true;
    commands.push_back(std::move(edit));
    return commands;
}

}  // namespace cad
