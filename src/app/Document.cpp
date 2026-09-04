#include "Document.hpp"

#include <algorithm>

#include "cad/render/Scene.hpp"

namespace cad::app {

Document::Document(std::filesystem::path pluginDirectory, const ConfigFile* config,
                   CommandProvider extraCommands)
    : pluginDirectory_(std::move(pluginDirectory)),
      config_(config),
      extraCommands_(std::move(extraCommands)) {
    reset(Database::createEmpty());
}

Document::~Document() = default;

void Document::reset(std::unique_ptr<Database> database) {
    editedBlock_ = Handle{};
    // Order matters on teardown as well as construction. Plugins go first:
    // unregistering their commands touches the registry, and the registry is
    // about to be replaced. Then the executor, which holds references to the
    // manager and the selection.
    plugins_.reset();
    hostDocument_.reset();
    executor_.reset();
    transactions_.reset();
    selection_.reset();

    db_ = std::move(database);
    transactions_ = std::make_unique<TransactionManager>(*db_);
    registry_ = createStandardRegistry();
    selection_ = std::make_unique<Selection>();
    executor_ = std::make_unique<CommandExecutor>(*db_, *transactions_, *registry_,
                                                  *selection_);

    hostDocument_ = std::make_unique<abi::HostDocument>(*db_, *transactions_, executor_.get());
    plugins_ = std::make_unique<abi::PluginManager>(*registry_, *hostDocument_);
    // Loaded before the caller builds its menus, so plugin commands appear in
    // them on the same pass as the built-ins rather than needing a refresh.
    pluginResults_ = pluginDirectory_.empty()
                         ? std::vector<abi::PluginLoadResult>{}
                         : plugins_->loadDirectory(pluginDirectory_);

    // Shell-owned commands, registered through the same path a plugin uses.
    // Released rather than kept: the registry is rebuilt wholesale by the next
    // reset, so there is nothing to unregister from.
    commandProblems_.clear();
    if (extraCommands_) {
        // Lenient rather than atomic: this batch carries the user's own macros,
        // and one file naming itself LINE must not take ZOOM and the clipboard
        // down with it.
        registry_->registerLenient(extraCommands_(), commandProblems_).release();
    }

    // User aliases last, so one may point at a command a plugin just
    // registered — and so a user's preference wins over both the built-ins and
    // whatever a plugin claimed. Applied here rather than by the caller because
    // reset() rebuilds the registry, and a caller that forgot to reapply would
    // silently lose every alias on File > New.
    configResult_ = ConfigApplyResult{};
    if (config_ != nullptr) {
        configResult_ = applyAliases(*registry_, *config_);
    }

    // Every drawing gets a Model tab, even one written by a script with no
    // layouts at all, because the shell needs something to show.
    activeLayout_ = db_->ensureModelLayout().handle;
    syncCurrentSpace();

    savedUndoDepth_ = 0;
    reindex();
}

Layout& Document::activeLayout() const {
    if (auto* layout = db_->get<Layout>(activeLayout_)) {
        return *layout;
    }
    // The active layout was deleted or the handle went stale. Falling back to
    // Model is the only answer that leaves the application usable.
    return db_->ensureModelLayout();
}

bool Document::setActiveLayout(std::string_view name) {
    Layout* layout = db_->findLayout(name);
    if (layout == nullptr) {
        return false;
    }
    activeLayout_ = layout->handle;
    // A viewport belongs to its sheet; stepping to another tab steps out.
    activeViewport_ = Handle{};
    syncCurrentSpace();
    return true;
}

const ViewportEntity* Document::activeViewport() const {
    const auto* window = db_->get<ViewportEntity>(activeViewport_);
    if (window == nullptr || window->erased || !inPaperSpace() ||
        window->ownerHandle != activeLayout().blockRecordHandle) {
        return nullptr;
    }
    return window;
}

void Document::setActiveViewport(Handle viewport) {
    activeViewport_ = viewport;
    syncCurrentSpace();
}

void Document::syncCurrentSpace() {
    // Commands draw into whatever the database names as current. Kept in
    // step here, at every change of tab or block, rather than looked up by
    // each command: a line drawn on a sheet used to land in model space,
    // because every command asked for model space by name.
    db_->settings.currentSpaceHandle = activeSpace().handle;
}

bool Document::inPaperSpace() const {
    return !activeLayout().isModel();
}

BlockTableRecord& Document::activeSpace() const {
    if (auto* block = db_->get<BlockTableRecord>(editedBlock_);
        block != nullptr && !block->erased) {
        return *block;
    }
    // Through an active viewport the sheet shows the model, and the model is
    // what is drawn into and picked from.
    if (activeViewport() != nullptr) {
        return db_->modelSpace();
    }
    if (auto* sheet = db_->get<BlockTableRecord>(activeLayout().blockRecordHandle)) {
        return *sheet;
    }
    return db_->modelSpace();
}

bool Document::beginEditingBlock(Handle blockRecord) {
    auto* block = db_->get<BlockTableRecord>(blockRecord);
    if (block == nullptr || block->erased) {
        return false;
    }
    // A layout space is already editable by switching to its tab, and treating
    // model space as a block to open would give a drawing two ways into the
    // same place that behave differently.
    if (block->isLayoutSpace()) {
        return false;
    }
    editedBlock_ = blockRecord;
    syncCurrentSpace();
    return true;
}

void Document::endEditingBlock() {
    editedBlock_ = Handle{};
    syncCurrentSpace();
}

BlockTableRecord* Document::editedBlock() const {
    auto* block = db_->get<BlockTableRecord>(editedBlock_);
    return block != nullptr && !block->erased ? block : nullptr;
}

const std::vector<Handle>& Document::selection() const {
    return selection_->handles;
}

void Document::setSelection(std::vector<Handle> handles) {
    selection_->handles = std::move(handles);
}

void Document::clearSelection() {
    selection_->handles.clear();
}

void Document::toggleSelection(Handle handle) {
    auto& handles = selection_->handles;
    const auto it = std::find(handles.begin(), handles.end(), handle);
    if (it == handles.end()) {
        handles.push_back(handle);
    } else {
        handles.erase(it);
    }
}

bool Document::isSelected(Handle handle) const {
    const auto& handles = selection_->handles;
    return std::find(handles.begin(), handles.end(), handle) != handles.end();
}

void Document::reindex() {
    // Whatever is being edited, which is model space until a block is opened.
    // Indexing model space regardless would leave picking, snapping and window
    // selection all working on geometry that is not on the screen.
    render::SceneBuilder builder{*db_, index_};
    builder.reindex(activeSpace());
    // A sheet's viewports look into the model, which is not on screen and so
    // not in the index above. Indexed separately, and only when a sheet is
    // showing: on the Model tab the two would be the same index twice.
    if (&activeSpace() != &db_->modelSpace() || activeViewport() != nullptr) {
        render::SceneBuilder modelBuilder{*db_, modelIndex_};
        modelBuilder.reindex(db_->modelSpace());
    }
    ++generation_;
}

bool Document::isModified() const {
    // Undo depth is a proxy for "has anything happened since the last save".
    // It is not perfect — undoing back past the save point reports modified
    // when the content matches — but it never reports *unmodified* when there
    // are real changes, which is the direction that matters.
    return transactions_->undoDepth() != savedUndoDepth_;
}

void Document::markSaved() {
    savedUndoDepth_ = transactions_->undoDepth();
}

}  // namespace cad::app
