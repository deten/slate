#include "ViewCommands.hpp"

#include "CanvasWidget.hpp"
#include "Document.hpp"
#include <cctype>

#include "cad/db/BlockTableRecord.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/Layout.hpp"
#include "cad/db/ViewportEntity.hpp"
#include "cad/render/Scene.hpp"

namespace cad::app {
namespace {

const CommandKeyword kAll{"All", "A", "Fit the whole drawing"};
const CommandKeyword kCenter{"Center", "C", "Centre on a point"};
const CommandKeyword kExtents{"Extents", "E", "Fit the drawn geometry"};
const CommandKeyword kPrevious{"Previous", "P", "Back to the last view"};
const CommandKeyword kWindow{"Window", "W", "Fit a rectangle"};

/// ZOOM — the view command, with other CAD software's options.
///
/// The keywords matter more than they look: `Z` then `E` is one of the most
/// worn paths in CAD, and a ZOOM that only did one thing would break the habit
/// of every drafter who has used other CAD software.
class ZoomCommand final : public CommandInstance {
public:
    explicit ZoomCommand(CanvasWidget* canvas) : canvas_(canvas) {}

    StepResult start(CommandContext& /*context*/) override {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = "Specify corner of window, or an option";
        request.keywords = {kAll, kCenter, kExtents, kPrevious, kWindow};
        request.allowEmpty = true;
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (canvas_ == nullptr) {
            return StepResult::failed("No view to zoom.");
        }
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }

        if (input.kind == InputValue::Kind::Keyword) {
            return applyKeyword(input.text, context);
        }

        if (input.kind == InputValue::Kind::Point) {
            if (wantsCenter_) {
                canvas_->panBy(input.point - canvas_->viewport().center());
                return StepResult::done("Centred");
            }
            // A bare point is the first corner of a zoom window, which is what
            // other CAD software does when you click rather than choose an option.
            if (!hasFirstCorner_) {
                firstCorner_ = input.point;
                hasFirstCorner_ = true;
                return askOppositeCorner();
            }
            canvas_->zoomToBox(Box3::fromCorners(firstCorner_, input.point));
            return StepResult::done("Zoomed to window");
        }

        // Enter with nothing typed: extents is the most useful default, and
        // the one a drafter reaches for without thinking.
        canvas_->zoomExtents();
        return StepResult::done("Zoomed to extents");
    }

private:
    [[nodiscard]] static StepResult askOppositeCorner() {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = "Specify opposite corner";
        return StepResult::ask(request);
    }

    StepResult applyKeyword(const std::string& keyword, CommandContext& context) {
        const char initial =
            keyword.empty() ? '\0' : static_cast<char>(std::toupper(
                                         static_cast<unsigned char>(keyword.front())));
        switch (initial) {
            case 'E':
                canvas_->zoomExtents();
                return StepResult::done("Zoomed to extents");

            case 'A': {
                // "All" is the drawing limits in other CAD software; with none set it is
                // the extents, which is also what it means here.
                canvas_->zoomExtents();
                return StepResult::done("Zoomed to the whole drawing");
            }

            case 'P':
                if (!canvas_->zoomPrevious()) {
                    return StepResult::failed("No previous view to return to.");
                }
                return StepResult::done("Previous view");

            case 'W':
                hasFirstCorner_ = false;
                return askFirstCorner();

            case 'C':
                wantsCenter_ = true;
                return askCenter();

            default:
                context.warn("Unrecognized option.");
                return start(context);
        }
    }

    [[nodiscard]] static StepResult askFirstCorner() {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = "Specify first corner";
        return StepResult::ask(request);
    }

    [[nodiscard]] static StepResult askCenter() {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = "Specify centre point";
        return StepResult::ask(request);
    }

    CanvasWidget* canvas_ = nullptr;
    Vec3 firstCorner_;
    bool hasFirstCorner_ = false;
    bool wantsCenter_ = false;
};

/// ZOOM's centre option needs a second step, kept separate for clarity.
class ZoomCenterCommand final : public CommandInstance {
public:
    explicit ZoomCenterCommand(CanvasWidget* canvas) : canvas_(canvas) {}

    StepResult start(CommandContext& /*context*/) override {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = "Specify centre point";
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& /*context*/) override {
        if (canvas_ == nullptr || input.kind != InputValue::Kind::Point) {
            return StepResult::cancelled();
        }
        canvas_->panBy(input.point - canvas_->viewport().center());
        return StepResult::done("Centred");
    }

private:
    CanvasWidget* canvas_ = nullptr;
};

/// ZOOMEXTENTS — the whole drawing, with no prompt.
///
/// Exists so that a single alias can mean it. other CAD software reaches extents as `Z`
/// then `E`, and that still works; this is for the drafter who would rather
/// bind one word to it.
class ZoomExtentsCommand final : public CommandInstance {
public:
    explicit ZoomExtentsCommand(CanvasWidget* canvas) : canvas_(canvas) {}

    StepResult start(CommandContext& /*context*/) override {
        if (canvas_ == nullptr) {
            return StepResult::failed("No view to zoom.");
        }
        canvas_->zoomExtents();
        return StepResult::done("Zoomed to extents");
    }

    StepResult supply(const InputValue&, CommandContext&) override {
        return StepResult::done();
    }

private:
    CanvasWidget* canvas_ = nullptr;
};

/// PAN — shift the view between two points.
///
/// The point-to-point form rather than a real-time drag, which is already on
/// the middle mouse button. Typed panning is what a keyboard-driven workflow
/// and a script both need.
class PanCommand final : public CommandInstance {
public:
    explicit PanCommand(CanvasWidget* canvas) : canvas_(canvas) {}

    StepResult start(CommandContext& /*context*/) override {
        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = "Specify base point";
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& /*context*/) override {
        if (canvas_ == nullptr || input.kind != InputValue::Kind::Point) {
            return StepResult::cancelled();
        }
        if (!hasBase_) {
            base_ = input.point;
            hasBase_ = true;
            InputRequest request;
            request.kind = InputRequest::Kind::Point;
            request.prompt = "Specify second point";
            request.basePoint = base_;
            return StepResult::ask(request);
        }
        canvas_->panBy(input.point - base_);
        return StepResult::done("Panned");
    }

private:
    CanvasWidget* canvas_ = nullptr;
    Vec3 base_;
    bool hasBase_ = false;
};

/// LAYOUT — switch to a sheet by name.
///
/// The tabs and Ctrl+PgUp/PgDn both reach layouts by position; this reaches one
/// by name, which is what a script, a macro or a batch plot needs.
/// MSPACE: step into a viewport. The only one on the sheet, or the one picked.
class MspaceCommand final : public CommandInstance {
public:
    explicit MspaceCommand(CanvasWidget* canvas) : canvas_(canvas) {}

    StepResult start(CommandContext& context) override {
        if (canvas_ == nullptr) {
            return StepResult::failed("No view.");
        }
        if (canvas_->viewportActive()) {
            return StepResult::done("Already in model space through a viewport");
        }
        if (context.db().currentSpace().isModelSpace()) {
            return StepResult::failed("MSPACE steps into a viewport on a sheet. Switch to a layout tab first.");
        }
        if (canvas_->activateSoleViewport()) {
            return StepResult::done("Model space, through the viewport");
        }
        InputRequest request;
        request.kind = InputRequest::Kind::Entity;
        request.prompt = "Select viewport";
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& /*context*/) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }
        Handle picked;
        if (input.kind == InputValue::Kind::Entity) {
            picked = input.entity;
        } else if (input.kind == InputValue::Kind::Selection && !input.selection.empty()) {
            picked = input.selection.front();
        }
        if (canvas_ == nullptr || !canvas_->activateViewport(picked)) {
            return StepResult::failed("That is not a viewport on this sheet.");
        }
        return StepResult::done("Model space, through the viewport");
    }

private:
    CanvasWidget* canvas_ = nullptr;
};

/// PSPACE: back onto the sheet.
class PspaceCommand final : public CommandInstance {
public:
    explicit PspaceCommand(CanvasWidget* canvas) : canvas_(canvas) {}

    StepResult start(CommandContext& /*context*/) override {
        if (canvas_ == nullptr || !canvas_->viewportActive()) {
            return StepResult::done("Already in paper space");
        }
        canvas_->exitViewport();
        return StepResult::done("Paper space");
    }

    StepResult supply(const InputValue&, CommandContext&) override { return StepResult::done(); }

private:
    CanvasWidget* canvas_ = nullptr;
};

const CommandKeyword kNewLayoutKeyword{"New", "N", "Make a new sheet"};
const CommandKeyword kDeleteLayoutKeyword{"Delete", "D", "Remove a sheet and everything on it"};
const CommandKeyword kRenameLayoutKeyword{"Rename", "R", "Rename a sheet"};

[[nodiscard]] bool startsWithKeyword(const std::string& typed, char initial) {
    return !typed.empty() &&
           std::toupper(static_cast<unsigned char>(typed.front())) == initial;
}

/// LAYOUT: switch to a sheet by name, or make, remove and rename sheets.
///
/// Switching is the shell's business -- the tab bar and the view change --
/// and reaches it through a callback. Making and removing are the
/// database's, done through the transaction so they undo like anything else.
class LayoutCommand final : public CommandInstance {
public:
    explicit LayoutCommand(std::function<bool(const std::string&)> switchLayout)
        : switchLayout_(std::move(switchLayout)) {}

    StepResult start(CommandContext& context) override {
        // The available names go in the prompt: a sheet set has nine tabs with
        // similar names and nobody remembers them exactly.
        InputRequest request;
        request.kind = InputRequest::Kind::Text;
        request.prompt = "Enter layout name or [New/Delete/Rename]" + names(context);
        request.keywords = {kNewLayoutKeyword, kDeleteLayoutKeyword, kRenameLayoutKeyword};
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }
        switch (stage_) {
            case Stage::Choose:
                if (input.kind == InputValue::Kind::Keyword) {
                    if (startsWithKeyword(input.text, 'N')) {
                        stage_ = Stage::NewName;
                        return askText("Enter name for the new layout",
                                       context.db().layouts().size() > 1
                                           ? "Layout" + std::to_string(context.db().layouts().size())
                                           : "Layout1");
                    }
                    if (startsWithKeyword(input.text, 'D')) {
                        stage_ = Stage::DeleteName;
                        return askText("Enter name of layout to delete" + names(context), {});
                    }
                    if (startsWithKeyword(input.text, 'R')) {
                        stage_ = Stage::RenameFrom;
                        return askText("Enter name of layout to rename" + names(context), {});
                    }
                }
                if (input.kind != InputValue::Kind::Text || input.text.empty()) {
                    return StepResult::cancelled();
                }
                return switchTo(input.text, context);

            case Stage::NewName:
                if (input.kind != InputValue::Kind::Text || input.text.empty()) {
                    return StepResult::cancelled();
                }
                return create(input.text, context);

            case Stage::DeleteName:
                if (input.kind != InputValue::Kind::Text || input.text.empty()) {
                    return StepResult::cancelled();
                }
                return remove(input.text, context);

            case Stage::RenameFrom:
                if (input.kind != InputValue::Kind::Text || input.text.empty()) {
                    return StepResult::cancelled();
                }
                if (context.db().findLayout(input.text) == nullptr) {
                    return StepResult::failed("No layout named \"" + input.text + "\".");
                }
                renameFrom_ = input.text;
                stage_ = Stage::RenameTo;
                return askText("Enter new name for " + renameFrom_, {});

            case Stage::RenameTo:
                if (input.kind != InputValue::Kind::Text || input.text.empty()) {
                    return StepResult::cancelled();
                }
                return rename(renameFrom_, input.text, context);
        }
        return StepResult::cancelled();
    }

private:
    enum class Stage { Choose, NewName, DeleteName, RenameFrom, RenameTo };

    [[nodiscard]] static std::string names(CommandContext& context) {
        std::string list;
        for (const Layout* layout : context.db().layouts()) {
            if (!list.empty()) {
                list += ", ";
            }
            list += layout->name;
        }
        return list.empty() ? std::string{} : " [" + list + "]";
    }

    [[nodiscard]] static StepResult askText(std::string prompt, std::optional<std::string> fallback) {
        InputRequest request;
        request.kind = InputRequest::Kind::Text;
        request.prompt = std::move(prompt);
        request.defaultText = std::move(fallback);
        return StepResult::ask(request);
    }

    StepResult switchTo(const std::string& name, CommandContext& context) {
        if (!switchLayout_) {
            return StepResult::failed("No view to switch.");
        }
        if (!switchLayout_(name)) {
            return StepResult::failed("No layout named \"" + name + "\".");
        }
        context.log("Switched to " + name);
        return StepResult::done();
    }

    StepResult create(const std::string& name, CommandContext& context) {
        Database& db = context.db();
        if (db.findLayout(name) != nullptr) {
            return StepResult::failed("There is already a layout named \"" + name + "\".");
        }
        Transaction& transaction = context.transaction();

        // The sheet is a block record, named as other CAD software names them, so the
        // file reads as one other CAD software wrote. A fresh drawing already carries an
        // empty *Paper_Space that no layout claims; the first sheet takes it,
        // as other CAD software's first layout does, rather than leaving it orphaned.
        Handle blockHandle;
        for (const Handle candidate : db.blocks().handles()) {
            const auto* existing = db.get<BlockTableRecord>(candidate);
            if (existing != nullptr && !existing->erased && !existing->isModelSpace() &&
                existing->isLayoutSpace() && db.layoutForBlock(candidate) == nullptr &&
                db.entitiesIn(*existing).empty()) {
                blockHandle = candidate;
                break;
            }
        }
        if (blockHandle.isNull()) {
            auto block = std::make_unique<BlockTableRecord>();
            block->name = db.nextPaperSpaceName();
            blockHandle = transaction.addObject(std::move(block)).handle;
        }

        int lastTab = 0;
        for (const Layout* existing : db.layouts()) {
            lastTab = std::max(lastTab, existing->tabOrder);
        }
        auto layout = std::make_unique<Layout>();
        layout->name = name;
        layout->blockRecordHandle = blockHandle;
        layout->tabOrder = lastTab + 1;
        // A4 landscape at 1:1 with 10 mm margins, until PAGESETUP says otherwise.
        layout->plot.paperName = "ISO A4 (210.00 x 297.00 MM)";
        layout->plot.paperWidth = 210.0;
        layout->plot.paperHeight = 297.0;
        layout->plot.rotation = 90;  // degrees: the portrait sheet turned to landscape
        layout->plot.marginLeft = layout->plot.marginBottom = 10.0;
        layout->plot.marginRight = layout->plot.marginTop = 10.0;
        layout->plot.numerator = layout->plot.denominator = 1.0;
        const Box3 sheet = layout->sheetRect();
        transaction.addObject(std::move(layout));

        // Every sheet carries a viewport numbered 1: the paper itself, as
        // other CAD software sees it. Without one, other CAD software took the first real viewport
        // for the paper view and the model vanished from the sheet.
        if (auto* record = db.get<BlockTableRecord>(blockHandle)) {
            auto paper = std::make_unique<ViewportEntity>();
            paper->viewportId = 1;
            paper->center = sheet.center();
            paper->width = sheet.width();
            paper->height = sheet.height();
            paper->viewCenter = paper->center;
            paper->viewHeight = sheet.height();
            // The flags other CAD software writes for a paper view: UCS icon at origin,
            // grid and snap settings, plan view. Copied rather than derived.
            paper->statusFlags = 819232;
            transaction.addEntity(std::move(paper), *record);
        }

        context.log("Layout \"" + name + "\" created.");
        if (switchLayout_) {
            switchLayout_(name);
        }
        return StepResult::done();
    }

    StepResult remove(const std::string& name, CommandContext& context) {
        Database& db = context.db();
        Layout* layout = db.findLayout(name);
        if (layout == nullptr) {
            return StepResult::failed("No layout named \"" + name + "\".");
        }
        if (layout->isModel()) {
            return StepResult::failed("The Model tab cannot be deleted.");
        }
        Transaction& transaction = context.transaction();
        // Everything on the sheet goes with it: the entities, the block that
        // held them, and the layout itself. All through the transaction, so
        // one undo brings the whole sheet back.
        std::size_t removed = 0;
        if (auto* block = db.get<BlockTableRecord>(layout->blockRecordHandle)) {
            for (Entity* entity : db.entitiesIn(*block)) {
                if (transaction.erase(*entity)) {
                    ++removed;
                }
            }
            transaction.erase(*block);
        }
        transaction.erase(*layout);
        context.log("Layout \"" + name + "\" deleted, with " + std::to_string(removed) +
                    " object(s).");
        if (switchLayout_) {
            switchLayout_(kLayoutModel);
        }
        return StepResult::done();
    }

    StepResult rename(const std::string& from, const std::string& to, CommandContext& context) {
        Database& db = context.db();
        Layout* layout = db.findLayout(from);
        if (layout == nullptr) {
            return StepResult::failed("No layout named \"" + from + "\".");
        }
        if (layout->isModel()) {
            return StepResult::failed("The Model tab cannot be renamed.");
        }
        if (db.findLayout(to) != nullptr) {
            return StepResult::failed("There is already a layout named \"" + to + "\".");
        }
        context.transaction().openForWrite(*layout).name = to;
        context.log("Layout \"" + from + "\" renamed to \"" + to + "\".");
        if (switchLayout_) {
            switchLayout_(to);
        }
        return StepResult::done();
    }

    Stage stage_ = Stage::Choose;
    std::string renameFrom_;
    std::function<bool(const std::string&)> switchLayout_;
};

/// QSELECT — open the quick select dialog.
///
/// A command with no prompts of its own: the question is compound and is asked
/// in one place. Registered as a command anyway so it is reachable by name, by
/// alias, from a macro and from a keyboard shortcut, like everything else.
/// A command whose whole job is to open a window.
///
/// The shell owns the window and the engine owns the command, so the command
/// holds a callback and nothing else. Its own name is in the failure message
/// because "the dialog is not available" tells a user nothing about which one.
class OpenDialogCommand final : public CommandInstance {
public:
    OpenDialogCommand(std::function<void()> open, std::string what)
        : open_(std::move(open)), what_(std::move(what)) {}

    StepResult start(CommandContext& /*context*/) override {
        if (!open_) {
            return StepResult::failed(what_ + " needs the dialog, which is unavailable in a "
                                              "scripted run.");
        }
        open_();
        return StepResult::done();
    }

    StepResult supply(const InputValue&, CommandContext&) override {
        return StepResult::done();
    }

private:
    std::function<void()> open_;
    std::string what_;
};

/// LWDISPLAY — show lineweights at their width on screen, or as hairlines.
///
/// A setting of the drawing, saved with it. Plots always use the widths;
/// this is only about the screen.
class LineweightDisplayCommand final : public CommandInstance {
public:
    StepResult start(CommandContext& context) override {
        InputRequest request;
        request.kind = InputRequest::Kind::Keyword;
        request.prompt = "Show lineweights on screen [ON/OFF]";
        request.defaultText = context.db().settings.displayLineweights ? "OFF" : "ON";
        request.allowEmpty = true;
        request.keywords = {CommandKeyword{"ON", "ON", "At their width"},
                            CommandKeyword{"OFF", "OFF", "As hairlines"}};
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }
        bool& shown = context.db().settings.displayLineweights;
        if ((input.kind == InputValue::Kind::Keyword || input.kind == InputValue::Kind::Text) &&
            !input.text.empty()) {
            shown = !(input.text.size() >= 2 &&
                      std::toupper(static_cast<unsigned char>(input.text[1])) == 'F');
        } else {
            shown = !shown;
        }
        return StepResult::done(shown ? "Lineweights shown at their width"
                                      : "Lineweights shown as hairlines");
    }
};

/// XATTACH — bring another drawing in as an external reference.
///
/// The file, whether it is attached or overlaid, then where and how big, the
/// way INSERT asks. The block record and its contents come from the window,
/// which has the file layer; the reference placed here is an ordinary insert
/// in the transaction, so undo takes it away like any other.
class AttachXrefCommand final : public CommandInstance {
public:
    explicit AttachXrefCommand(AttachXrefHook attach) : attach_(std::move(attach)) {}

    StepResult start(CommandContext& /*context*/) override {
        if (!attach_) {
            return StepResult::failed("Attaching a reference needs the file layer, which is "
                                      "unavailable here.");
        }
        InputRequest request;
        request.kind = InputRequest::Kind::Text;
        request.prompt = "Enter the drawing file to attach";
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }
        switch (stage_) {
            case Stage::Path: {
                if (input.kind != InputValue::Kind::Text || input.text.empty()) {
                    return StepResult::cancelled("No file given");
                }
                path_ = input.text;
                stage_ = Stage::Type;
                InputRequest request;
                request.kind = InputRequest::Kind::Keyword;
                request.prompt = "Reference type [Attach/Overlay]";
                request.defaultText = "Attach";
                request.allowEmpty = true;
                request.keywords = {
                    CommandKeyword{"Attach", "A", "Carried along when this drawing is itself referenced"},
                    CommandKeyword{"Overlay", "O", "Seen here only, not through further references"}};
                return StepResult::ask(request);
            }
            case Stage::Type: {
                overlay_ = (input.kind == InputValue::Kind::Keyword ||
                            input.kind == InputValue::Kind::Text) &&
                           !input.text.empty() &&
                           std::toupper(static_cast<unsigned char>(input.text.front())) == 'O';
                const std::string problem = attach_(path_, overlay_, &block_);
                if (!problem.empty()) {
                    return StepResult::failed(problem);
                }
                stage_ = Stage::Point;
                InputRequest request;
                request.kind = InputRequest::Kind::Point;
                request.prompt = "Specify insertion point";
                return StepResult::ask(request);
            }
            case Stage::Point: {
                if (input.kind != InputValue::Kind::Point) {
                    return StepResult::cancelled();
                }
                position_ = input.point;
                stage_ = Stage::Scale;
                InputRequest request;
                request.kind = InputRequest::Kind::Number;
                request.prompt = "Specify scale factor";
                request.defaultNumber = 1.0;
                request.allowEmpty = true;
                request.minimum = 1e-9;
                return StepResult::ask(request);
            }
            case Stage::Scale: {
                if (input.kind == InputValue::Kind::Number && input.number > 0.0) {
                    scale_ = input.number;
                }
                stage_ = Stage::Rotation;
                InputRequest request;
                request.kind = InputRequest::Kind::Angle;
                request.prompt = "Specify rotation angle";
                request.defaultNumber = 0.0;
                request.allowEmpty = true;
                return StepResult::ask(request);
            }
            case Stage::Rotation:
            default: {
                if (input.kind == InputValue::Kind::Number) {
                    rotation_ = input.number;
                }
                const auto* block = context.db().get<BlockTableRecord>(block_);
                if (block == nullptr) {
                    return StepResult::failed("The reference's block is gone.");
                }
                auto reference = std::make_unique<InsertEntity>(block_, position_);
                reference->scale = Vec3{scale_, scale_, scale_};
                reference->rotation = rotation_;
                context.transaction().addToCurrentSpace(std::move(reference));
                return StepResult::done("Reference \"" + block->name + "\" " +
                                        (overlay_ ? "overlaid" : "attached"));
            }
        }
    }

private:
    enum class Stage : std::uint8_t { Path, Type, Point, Scale, Rotation };

    AttachXrefHook attach_;
    Stage stage_ = Stage::Path;
    std::string path_;
    bool overlay_ = false;
    Handle block_;
    Vec3 position_;
    double scale_ = 1.0;
    double rotation_ = 0.0;
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
    definition.category = "View";
    definition.supportsPickfirst = false;
    // Changing the view is not a change to the drawing, so it must not open a
    // transaction — otherwise every zoom would land on the undo stack and
    // Ctrl+Z would walk back through the view history instead of the edits.
    definition.transactionMode = CommandTransactionMode::ReadOnly;
    definition.create = std::move(create);
    return definition;
}

/// AI — hand a request in plain words to whichever assistant is configured.
///
/// An ordinary command, so it can be typed, scripted, put on the ribbon and
/// repeated like any other. The instruction arrives as text at a prompt, which
/// is also why a scripted run can drive it: `AI` then the sentence.
class AssistantCommand final : public CommandInstance {
public:
    explicit AssistantCommand(std::function<void(const std::string&)> ask)
        : ask_(std::move(ask)) {}

    StepResult start(CommandContext& /*context*/) override {
        if (!ask_) {
            return StepResult::failed("The assistant is unavailable in this build.");
        }
        InputRequest request;
        request.kind = InputRequest::Kind::Text;
        request.prompt = "Describe the change";
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& /*context*/) override {
        if (input.kind != InputValue::Kind::Text || input.text.empty()) {
            return StepResult::cancelled();
        }
        // The command ends here and the work happens outside it. An assistant
        // proposes edits that are themselves commands, and running commands
        // inside a running command would nest the executor in itself.
        ask_(input.text);
        return StepResult::done();
    }

private:
    std::function<void(const std::string&)> ask_;
};

}  // namespace

std::vector<CommandDefinition> viewCommands(CanvasWidget* canvas,
                                            std::function<bool(const std::string&)> switchLayout,
                                            std::function<void()> quickSelect,
                                            std::function<void()> references,
                                            std::function<void()> purge,
                                            std::function<void(const std::string&)> assistant,
                                            AttachXrefHook attachXref) {
    std::vector<CommandDefinition> commands;

    {
        CommandDefinition definition;
        definition.id = "cad.view.lwdisplay";
        definition.name = "LWDISPLAY";
        definition.aliases = {"LWT"};
        definition.title = "Lineweights";
        definition.description = "Show lineweights at their width on screen, or as hairlines";
        definition.category = "View";
        definition.supportsPickfirst = false;
        // A display setting, not an edit: nothing to undo.
        definition.transactionMode = CommandTransactionMode::ReadOnly;
        definition.create = [] { return std::make_unique<LineweightDisplayCommand>(); };
        commands.push_back(std::move(definition));
    }

    {
        CommandDefinition definition;
        definition.id = "cad.insert.xattach";
        definition.name = "XATTACH";
        definition.aliases = {"XA"};
        definition.title = "Attach Reference";
        definition.description = "Bring another drawing in as an external reference";
        definition.category = "Insert";
        definition.supportsPickfirst = false;
        definition.create = [attachXref] {
            return std::make_unique<AttachXrefCommand>(attachXref);
        };
        commands.push_back(std::move(definition));
    }

    {
        CommandDefinition layout = makeDefinition(
            "cad.view.layout", "LAYOUT", {"LO"}, "Layout",
            "Switch to a layout by name, or make, delete and rename sheets",
            [switchLayout] { return std::make_unique<LayoutCommand>(switchLayout); });
        // Switching only looks; New, Delete and Rename change the drawing,
        // and do so through the transaction so that they undo.
        layout.transactionMode = CommandTransactionMode::Single;
        commands.push_back(std::move(layout));
    }

    {
        CommandDefinition definition;
        definition.id = "cad.tools.assistant";
        definition.name = "AI";
        definition.aliases = {"ASSISTANT", "AGENT"};
        definition.title = "AI Assistant";
        definition.description = "Describe a change and let the configured assistant make it";
        definition.category = "Tools";
        definition.supportsPickfirst = true;
        definition.repeatable = false;
        definition.create = [assistant] {
            return std::make_unique<AssistantCommand>(assistant);
        };
        commands.push_back(std::move(definition));
    }

    {
        CommandDefinition definition;
        definition.id = "cad.select.quick";
        definition.name = "QSELECT";
        definition.aliases = {"QS"};
        definition.title = "Quick Select";
        definition.description = "Build a selection by filtering on object properties";
        definition.category = "Tools";
        definition.supportsPickfirst = false;
        // Selecting is not a change to the drawing.
        definition.transactionMode = CommandTransactionMode::ReadOnly;
        definition.create = [quickSelect] {
            return std::make_unique<OpenDialogCommand>(quickSelect, "Quick select");
        };
        commands.push_back(std::move(definition));
    }

    {
        CommandDefinition definition;
        definition.id = "cad.insert.references";
        definition.name = "REFERENCES";
        // other CAD software reaches this by several names depending on which kind of
        // reference the user has in mind; all of them land here, because the
        // answer to "where is my missing image" and "where is my missing xref"
        // is the same window.
        definition.aliases = {"XREF", "XR", "IMAGE", "IM", "ER", "EXTERNALREFERENCES"};
        definition.title = "References";
        definition.description = "Show every file this drawing depends on, and repair its path";
        definition.category = "Insert";
        definition.supportsPickfirst = false;
        definition.transactionMode = CommandTransactionMode::ReadOnly;
        definition.create = [references] {
            return std::make_unique<OpenDialogCommand>(references, "The reference manager");
        };
        commands.push_back(std::move(definition));
    }

    {
        CommandDefinition definition;
        definition.id = "cad.file.purgedialog";
        definition.name = "PURGE";
        definition.aliases = {"PU"};
        definition.title = "Purge";
        definition.description = "Remove blocks, layers and styles nothing uses";
        definition.category = "Tools";
        definition.supportsPickfirst = false;
        definition.repeatable = false;
        definition.transactionMode = CommandTransactionMode::ReadOnly;
        definition.create = [purge] {
            return std::make_unique<OpenDialogCommand>(purge, "Purge");
        };
        commands.push_back(std::move(definition));
    }

    commands.push_back(makeDefinition(
        "cad.view.zoom", "ZOOM", {"Z"}, "Zoom",
        "Zoom to a window, the extents, or the previous view",
        [canvas] { return std::make_unique<ZoomCommand>(canvas); }));

    commands.push_back(makeDefinition(
        "cad.view.zoomextents", "ZOOMEXTENTS", {"ZE"}, "Zoom Extents",
        "Fit the whole drawing in the window",
        [canvas] { return std::make_unique<ZoomExtentsCommand>(canvas); }));

    commands.push_back(makeDefinition(
        "cad.view.zoomcenter", "ZOOMCENTER", {}, "Zoom Center",
        "Centre the view on a point",
        [canvas] { return std::make_unique<ZoomCenterCommand>(canvas); }));

    commands.push_back(makeDefinition(
        "cad.view.mspace", "MSPACE", {"MS"}, "Model Space",
        "Step into a viewport on this sheet and work in the model through it",
        [canvas] { return std::make_unique<MspaceCommand>(canvas); }));
    commands.push_back(makeDefinition(
        "cad.view.pspace", "PSPACE", {"PS"}, "Paper Space",
        "Step back out of the viewport onto the sheet",
        [canvas] { return std::make_unique<PspaceCommand>(canvas); }));
    commands.push_back(makeDefinition("cad.view.pan", "PAN", {"P"}, "Pan",
                                      "Shift the view between two points",
                                      [canvas] { return std::make_unique<PanCommand>(canvas); }));

    return commands;
}

}  // namespace cad::app
