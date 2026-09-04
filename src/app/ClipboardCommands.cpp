#include "ClipboardCommands.hpp"

#include <QByteArray>
#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>

#include <memory>
#include <string>
#include <utility>

#include "cad/db/BlockTableRecord.hpp"
#include "cad/db/Layer.hpp"
#include "cad/db/Transfer.hpp"
#include "cad/io/DxfReader.hpp"
#include "cad/io/DxfWriter.hpp"

namespace cad::app {
namespace {

/// Put a drawing fragment on the system clipboard.
void writeClipboard(const Database& content) {
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        return;
    }
    const std::string text = io::writeDxf(content);
    auto* mime = new QMimeData;
    mime->setData(QString::fromLatin1(kClipboardMimeType),
                  QByteArray{text.data(), static_cast<qsizetype>(text.size())});
    clipboard->setMimeData(mime);
}

/// Read a drawing fragment back, or null when the clipboard holds something
/// else — which is the usual case, since the clipboard is shared with every
/// other application on the machine.
[[nodiscard]] std::unique_ptr<Database> readClipboard() {
    const QClipboard* clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        return nullptr;
    }
    const QMimeData* mime = clipboard->mimeData();
    const QString type = QString::fromLatin1(kClipboardMimeType);
    if (mime == nullptr || !mime->hasFormat(type)) {
        return nullptr;
    }

    const QByteArray payload = mime->data(type);
    io::DxfReadResult result =
        io::readDxf(std::string_view{payload.constData(), static_cast<std::size_t>(payload.size())});
    // Warnings are ordinary; only a read that produced no database is a
    // failure. A fragment written by an older build that this one reads
    // imperfectly should still paste what it understood.
    return std::move(result.database);
}

[[nodiscard]] bool clipboardHasDrawing() {
    const QClipboard* clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        return false;
    }
    const QMimeData* mime = clipboard->mimeData();
    return mime != nullptr && mime->hasFormat(QString::fromLatin1(kClipboardMimeType));
}

/// The space a paste lands in, falling back to model space.
[[nodiscard]] BlockTableRecord& targetSpace(const std::function<BlockTableRecord*()>& active,
                                            Database& db) {
    if (active) {
        if (BlockTableRecord* space = active(); space != nullptr) {
            return *space;
        }
    }
    return db.modelSpace();
}

/// How much to scale content coming from a drawing measured in other units.
///
/// A detail plate drawn in millimetres dropped into a site plan drawn in metres
/// has to arrive a thousand times smaller, or it covers the county. other CAD software
/// scales on paste for exactly this reason, and declines to guess when either
/// drawing says it is unitless.
[[nodiscard]] double insertionScale(const Database& content, const Database& target) {
    const double factor = unitConversionFactor(content.settings.insertionUnits,
                                               target.settings.insertionUnits);
    return factor > 0.0 ? factor : 1.0;
}

/// Copy the selection to the clipboard, and optionally erase it.
///
/// One class for both because cut *is* copy plus erase, and writing them
/// separately is how the two drift: a fix to what copy carries has to be
/// remembered twice.
class CopyToClipboardCommand final : public CommandInstance {
public:
    explicit CopyToClipboardCommand(bool erase) : erase_(erase) {}

    StepResult start(CommandContext& context) override {
        if (!context.initialSelection().empty()) {
            return copy(context.initialSelection(), context);
        }
        InputRequest request;
        request.kind = InputRequest::Kind::Selection;
        request.prompt = erase_ ? "Select objects to cut" : "Select objects to copy";
        request.minCount = 1;
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind != InputValue::Kind::Selection || input.selection.empty()) {
            return StepResult::cancelled();
        }
        return copy(input.selection, context);
    }

private:
    StepResult copy(const std::vector<Handle>& selection, CommandContext& context) {
        const ExtractedContent content = extractEntities(context.db(), selection);
        if (content.isEmpty()) {
            return StepResult::cancelled("Nothing to copy");
        }
        writeClipboard(*content.database);

        const std::size_t copied = content.report.entities.size();
        if (!erase_) {
            return StepResult::done(std::to_string(copied) + " object(s) copied");
        }

        // Erased only after the clipboard is written, so a failure to write
        // cannot destroy the drafter's only copy of the geometry.
        std::size_t removed = 0;
        std::size_t locked = 0;
        for (const Handle handle : selection) {
            Entity* entity = context.db().getEntity(handle);
            if (entity == nullptr || entity->erased) {
                continue;
            }
            const Layer* layer = context.db().get<Layer>(entity->layerHandle);
            if (layer != nullptr && layer->isLocked) {
                ++locked;
                continue;
            }
            if (context.transaction().erase(*entity)) {
                ++removed;
            }
        }
        if (locked > 0) {
            context.warn(std::to_string(locked) +
                         " object(s) on a locked layer were copied but not cut.");
        }
        return StepResult::done(std::to_string(removed) + " object(s) cut");
    }

    bool erase_;
};

/// Paste the clipboard, either under the pointer or back where it came from.
class PasteCommand final : public CommandInstance {
public:
    PasteCommand(std::function<BlockTableRecord*()> activeSpace, bool atOriginalPosition)
        : activeSpace_(std::move(activeSpace)), atOriginalPosition_(atOriginalPosition) {}

    StepResult start(CommandContext& context) override {
        content_ = readClipboard();
        if (content_ == nullptr) {
            return StepResult::failed("The clipboard holds nothing this drawing can paste.");
        }
        scale_ = insertionScale(*content_, context.db());
        basePoint_ = content_->settings.insertionBase;

        if (atOriginalPosition_) {
            // The fragment's own coordinates, not rebased on its base point.
            // Between two sheets of one set this is what keeps a detail lined
            // up with whatever it was traced from.
            return place(Mat4::scaling(scale_), context);
        }

        InputRequest request;
        request.kind = InputRequest::Kind::Point;
        request.prompt = "Specify insertion point";
        return StepResult::ask(request);
    }

    StepResult supply(const InputValue& input, CommandContext& context) override {
        if (input.kind == InputValue::Kind::Cancel) {
            return StepResult::cancelled();
        }
        if (input.kind != InputValue::Kind::Point) {
            return StepResult::cancelled();
        }
        return place(placementFor(input.point), context);
    }

    [[nodiscard]] std::vector<std::unique_ptr<Entity>> preview(
        const Vec3& cursor, CommandContext& context) const override {
        if (content_ == nullptr || atOriginalPosition_) {
            return {};
        }

        // A ghost of what is about to land, so the insertion point is chosen
        // against the drawing rather than guessed and then undone.
        constexpr std::size_t kMaxGhosts = 400;
        const Mat4 placement = placementFor(cursor);

        std::vector<std::unique_ptr<Entity>> ghosts;
        const auto* model =
            content_->get<BlockTableRecord>(content_->blocks().find(kModelSpace));
        if (model == nullptr) {
            return ghosts;
        }
        for (const Entity* source : content_->entitiesIn(*model)) {
            if (ghosts.size() >= kMaxGhosts) {
                break;
            }
            if (source == nullptr || source->erased) {
                continue;
            }
            DbObjectPtr cloned = source->clone();
            std::unique_ptr<Entity> ghost{dynamic_cast<Entity*>(cloned.release())};
            if (ghost == nullptr) {
                continue;
            }
            // The clone names a layer in the clipboard's database. Left alone,
            // a ghost would resolve its colour against nothing and draw in the
            // wrong one, which is exactly what the preview is for getting right.
            ghost->layerHandle = matchingLayer(source->layerHandle, context.db());
            ghost->linetypeHandle = Handle::null();
            ghost->transformBy(placement);
            ghosts.push_back(std::move(ghost));
        }
        return ghosts;
    }

private:
    [[nodiscard]] Mat4 placementFor(const Vec3& at) const {
        // Read right to left: bring the fragment's base point to the origin,
        // convert its units, then put it where the user asked.
        return Mat4::translation(at) * Mat4::scaling(scale_) * Mat4::translation(-basePoint_);
    }

    /// The target's layer of the same name, or its default.
    [[nodiscard]] Handle matchingLayer(Handle sourceLayer, const Database& target) const {
        const auto* layer = content_->get<Layer>(sourceLayer);
        if (layer != nullptr) {
            if (const Handle match = target.layers().find(layer->name); !match.isNull()) {
                return match;
            }
        }
        return target.layers().find("0");
    }

    StepResult place(const Mat4& placement, CommandContext& context) {
        const TransferReport report =
            pasteContent(*content_, targetSpace(activeSpace_, context.db()), placement,
                         context.transaction());

        if (report.entities.empty()) {
            return StepResult::cancelled("The clipboard held no geometry");
        }

        for (const std::string& name : report.blocksReused) {
            context.warn("Block \"" + name +
                         "\" already exists here; this drawing's definition was kept.");
        }
        if (report.droppedReferences > 0) {
            context.warn(std::to_string(report.droppedReferences) +
                         " reference(s) to objects that were not copied were cleared.");
        }
        if (!report.layersAdded.empty()) {
            context.log(std::to_string(report.layersAdded.size()) + " layer(s) added");
        }
        return StepResult::done(std::to_string(report.entities.size()) + " object(s) pasted");
    }

    std::function<BlockTableRecord*()> activeSpace_;
    bool atOriginalPosition_;
    std::unique_ptr<Database> content_;
    Vec3 basePoint_;
    double scale_ = 1.0;
};

[[nodiscard]] CommandDefinition makeDefinition(std::string id, std::string name,
                                               std::vector<std::string> aliases,
                                               std::string title, std::string description,
                                               CommandTransactionMode mode,
                                               std::function<CommandInstancePtr()> create) {
    CommandDefinition definition;
    definition.id = std::move(id);
    definition.name = std::move(name);
    definition.aliases = std::move(aliases);
    definition.title = std::move(title);
    definition.description = std::move(description);
    definition.category = "Edit";
    definition.transactionMode = mode;
    definition.create = std::move(create);
    return definition;
}

}  // namespace

std::vector<CommandDefinition> clipboardCommands(std::function<BlockTableRecord*()> activeSpace) {
    std::vector<CommandDefinition> commands;

    CommandDefinition copy = makeDefinition(
        "cad.edit.copyclip", "COPYCLIP", {"COPYCLIPBOARD"}, "Copy",
        "Copy the selection to the clipboard", CommandTransactionMode::ReadOnly,
        [] { return std::make_unique<CopyToClipboardCommand>(false); });
    copy.supportsPickfirst = true;
    // Repeating a copy with Enter would silently re-copy whatever happens to be
    // selected, which is never what the second Enter meant.
    copy.repeatable = false;
    commands.push_back(std::move(copy));

    CommandDefinition cut =
        makeDefinition("cad.edit.cutclip", "CUTCLIP", {}, "Cut",
                       "Copy the selection to the clipboard and erase it",
                       CommandTransactionMode::Single,
                       [] { return std::make_unique<CopyToClipboardCommand>(true); });
    cut.supportsPickfirst = true;
    cut.repeatable = false;
    commands.push_back(std::move(cut));

    CommandDefinition paste = makeDefinition(
        "cad.edit.pasteclip", "PASTECLIP", {}, "Paste",
        "Paste the clipboard at a point you pick", CommandTransactionMode::Single,
        [activeSpace] { return std::make_unique<PasteCommand>(activeSpace, false); });
    paste.isAvailable = [](const Database&, const std::vector<Handle>&) {
        return clipboardHasDrawing();
    };
    commands.push_back(std::move(paste));

    CommandDefinition pasteOriginal = makeDefinition(
        "cad.edit.pasteorig", "PASTEORIG", {}, "Paste to Original Coordinates",
        "Paste the clipboard where it was cut or copied from",
        CommandTransactionMode::Single,
        [activeSpace] { return std::make_unique<PasteCommand>(activeSpace, true); });
    pasteOriginal.isAvailable = [](const Database&, const std::vector<Handle>&) {
        return clipboardHasDrawing();
    };
    commands.push_back(std::move(pasteOriginal));

    return commands;
}

}  // namespace cad::app
