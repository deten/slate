#pragma once

#include <cstdint>

#include <filesystem>
#include <memory>
#include <vector>

#include "cad/abi/PluginManager.hpp"
#include "cad/command/CommandConfig.hpp"
#include "cad/command/CommandExecutor.hpp"
#include "cad/command/StandardCommands.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/Transaction.hpp"
#include "cad/db/ViewportEntity.hpp"
#include "cad/render/SpatialIndex.hpp"

namespace cad::app {

/// One open drawing and everything that acts on it.
///
/// Groups the database, its undo history, its command registry and executor,
/// and the current selection into a single owner. Widgets hold a pointer to
/// this rather than to the pieces, so opening a file swaps one object and
/// nothing is left pointing at a destroyed database.
///
/// The registry is per-document, not global: a plugin registering a command for
/// one drawing must not leak into another.
class Document {
public:
    /// @param pluginDirectory searched for plugins whenever this document's
    ///   registry is built. Empty loads none, which is what tests want.
    /// Supplies commands the shell owns — ZOOM and PAN, which need a view.
    using CommandProvider = std::function<std::vector<CommandDefinition>()>;

    /// @param config user configuration, applied to the registry after plugins
    ///   load so an alias may point at a plugin's command. Not owned; it
    ///   belongs to the installation rather than to any one drawing, and must
    ///   outlive this document. Null applies no customisation.
    /// @param extraCommands invoked whenever the registry is rebuilt, before
    ///   aliases are applied, so a user alias can name one of these too. Taken
    ///   as a callback rather than a one-off registration because `reset`
    ///   replaces the registry, and anything registered from outside would
    ///   silently vanish on File > New.
    explicit Document(std::filesystem::path pluginDirectory = {},
                      const ConfigFile* config = nullptr,
                      CommandProvider extraCommands = {});
    ~Document();

    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    Document(Document&&) = delete;
    Document& operator=(Document&&) = delete;

    /// Take ownership of a freshly loaded database.
    void reset(std::unique_ptr<Database> database);

    [[nodiscard]] Database& db() const noexcept { return *db_; }
    [[nodiscard]] TransactionManager& transactions() const noexcept { return *transactions_; }
    [[nodiscard]] CommandRegistry& commands() const noexcept { return *registry_; }

    /// Commands the last registry rebuild could not accept, with the reason.
    ///
    /// Chiefly a user macro whose name is already taken. Surfaced rather than
    /// swallowed: a command that silently did not load is indistinguishable
    /// from one the user mistyped.
    [[nodiscard]] const std::vector<std::string>& commandProblems() const noexcept {
        return commandProblems_;
    }
    [[nodiscard]] CommandExecutor& executor() const noexcept { return *executor_; }
    [[nodiscard]] render::SpatialIndex& index() noexcept { return index_; }
    [[nodiscard]] const render::SpatialIndex& index() const noexcept { return index_; }

    /// Model space, indexed for a sheet's viewports to look through. Kept
    /// separately from `index()`, which covers whatever is on screen.
    [[nodiscard]] const render::SpatialIndex& modelIndex() const noexcept { return modelIndex_; }

    /// The current selection, in pick order.
    [[nodiscard]] const std::vector<Handle>& selection() const;
    void setSelection(std::vector<Handle> handles);
    void clearSelection();
    /// Add or remove a handle, for shift-click.
    void toggleSelection(Handle handle);
    [[nodiscard]] bool isSelected(Handle handle) const;

    /// Rebuild the spatial index from model space.
    ///
    /// Always model space, whichever layout is being shown: a paper-space
    /// sheet holds a handful of entities and is walked directly, while its
    /// viewports query this index for the part of the model each one shows.
    void reindex();

    /// Bumped by every `reindex`, which is to say by every committed edit.
    ///
    /// Anything caching work derived from the drawing's geometry compares this
    /// to know whether what it kept is still true. Cheaper than watching for
    /// the particular changes that would invalidate it, and impossible to get
    /// subtly wrong -- an edit that moved a block would otherwise leave a
    /// cached copy of it snapping at the old position.
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

    // ------------------------------------------------------------- layouts

    /// The layout currently being edited. Never null once a document exists.
    [[nodiscard]] Layout& activeLayout() const;

    /// Switch tabs. Names are matched case-insensitively, as everywhere else.
    /// @returns false when there is no such layout.
    bool setActiveLayout(std::string_view name);

    /// Tell the database which space is current, so commands draw there.
    void syncCurrentSpace();

    /// True when the active layout is a paper sheet rather than the model.
    [[nodiscard]] bool inPaperSpace() const;

    /// The block record new geometry goes into.
    ///
    /// The active layout's sheet, or the block being edited when one is open.
    /// Everything that draws or adds geometry asks this rather than the layout,
    /// which is what lets a block be edited in the drawing area without any of
    /// them knowing that is what is happening.
    [[nodiscard]] BlockTableRecord& activeSpace() const;

    /// The viewport stepped into on the active sheet, or null. While one is
    /// active the active space is the model, seen through it.
    [[nodiscard]] const ViewportEntity* activeViewport() const;
    void setActiveViewport(Handle viewport);

    /// Open a block definition for editing in the drawing area.
    ///
    /// Its contents are edited *in place*, so every insert of it updates as
    /// soon as the canvas is rebuilt. There is no separate step that writes the
    /// block back, because there is no copy to write back -- which is also why
    /// closing the editor cannot lose the work, and why undo reaches into it
    /// like any other edit.
    /// @returns false when the handle is not a block record, or is a layout
    ///   space, which is edited by switching to its tab instead.
    bool beginEditingBlock(Handle blockRecord);

    /// Return to the layout that was open before.
    void endEditingBlock();

    /// The block being edited, or null when a layout is.
    [[nodiscard]] BlockTableRecord* editedBlock() const;

    /// Plugins loaded for this drawing.
    ///
    /// Per-document, like the registry: a plugin's commands belong to the
    /// drawing they were registered against, so closing one drawing cannot
    /// leave another holding commands whose library has been unmapped.
    [[nodiscard]] const abi::PluginManager& plugins() const noexcept { return *plugins_; }

    /// What happened on the last load pass, for reporting to the user.
    [[nodiscard]] const std::vector<abi::PluginLoadResult>& pluginResults() const noexcept {
        return pluginResults_;
    }

    /// What the user's configuration did to this registry, for reporting.
    /// A misspelled alias must say so; silently doing nothing is worse.
    [[nodiscard]] const ConfigApplyResult& configResult() const noexcept {
        return configResult_;
    }

    /// Absolute path this drawing came from, empty for an unsaved drawing.
    std::string filePath;

    /// True when the drawing has unsaved changes.
    [[nodiscard]] bool isModified() const;

    /// Mark the current state as saved.
    void markSaved();

private:
    /// Selection storage that the executor can read and write.
    class Selection final : public SelectionProvider {
    public:
        [[nodiscard]] std::vector<Handle> selection() const override { return handles; }
        void setSelection(std::vector<Handle> next) override { handles = std::move(next); }
        void clearSelection() override { handles.clear(); }

        std::vector<Handle> handles;
    };

    std::filesystem::path pluginDirectory_;
    /// Not owned: installation-level, shared by every open drawing.
    const ConfigFile* config_ = nullptr;
    CommandProvider extraCommands_;
    ConfigApplyResult configResult_;
    /// Handle of the layout being edited, rather than a pointer, so undo
    /// replacing the object with a restored clone cannot leave this dangling.
    Handle activeLayout_;
    Handle activeViewport_;
    /// The block open for editing, if any.
    Handle editedBlock_;
    std::unique_ptr<Database> db_;
    std::unique_ptr<TransactionManager> transactions_;
    std::unique_ptr<CommandRegistry> registry_;
    std::unique_ptr<Selection> selection_;
    std::unique_ptr<CommandExecutor> executor_;
    /// Declared after the registry and executor it points at, so it is
    /// destroyed before them.
    std::unique_ptr<abi::HostDocument> hostDocument_;
    std::unique_ptr<abi::PluginManager> plugins_;
    std::vector<std::string> commandProblems_;
    std::vector<abi::PluginLoadResult> pluginResults_;
    render::SpatialIndex index_;
    render::SpatialIndex modelIndex_;
    std::uint64_t generation_ = 0;
    std::size_t savedUndoDepth_ = 0;
};

}  // namespace cad::app
