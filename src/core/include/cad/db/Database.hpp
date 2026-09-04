#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "cad/db/BlockTableRecord.hpp"
#include "cad/db/DimStyle.hpp"
#include "cad/db/DimensionEntity.hpp"
#include "cad/db/Entity.hpp"
#include "cad/db/Layer.hpp"
#include "cad/db/Layout.hpp"
#include "cad/db/Linetype.hpp"
#include "cad/db/SymbolTable.hpp"
#include "cad/db/TextStyle.hpp"
#include "cad/math/Angle.hpp"
#include "cad/math/Box3.hpp"

namespace cad {

/// A change to the database, broadcast so dependent systems — the spatial
/// index, the renderer's cache, the UI — can update incrementally rather than
/// rebuilding.
struct DatabaseEvent {
    enum class Kind : std::uint8_t {
        ObjectAdded,
        ObjectModified,
        ObjectErased,
        ObjectUnerased,
        Reset,
    };

    Kind kind = Kind::Reset;
    Handle handle;
};

using DatabaseListener = std::function<void(const DatabaseEvent&)>;

/// A referential-integrity problem found by `validate`.
struct DatabaseIssue {
    enum class Severity : std::uint8_t { Warning, Error };

    Severity severity = Severity::Error;
    Handle handle;
    std::string message;
};

/// Drawing units, matching the DXF `$INSUNITS` enumeration.
enum class DrawingUnits : std::uint8_t {
    Unitless = 0,
    Inches = 1,
    Feet = 2,
    Miles = 3,
    Millimeters = 4,
    Centimeters = 5,
    Meters = 6,
    Kilometers = 7,
    Microinches = 8,
    Mils = 9,
    Yards = 10,
    Angstroms = 11,
    Nanometers = 12,
    Microns = 13,
    Decimeters = 14,
    Decameters = 15,
    Hectometers = 16,
    Gigameters = 17,
    AstronomicalUnits = 18,
    LightYears = 19,
    Parsecs = 20,
};

/// Scale factor converting a length in `from` units to `to` units.
///
/// Returns 1 when either side is unitless, matching other CAD software's behaviour of
/// declining to guess. Getting this wrong on an xref silently produces a
/// drawing 25.4 times too large.
[[nodiscard]] double unitConversionFactor(DrawingUnits from, DrawingUnits to) noexcept;

/// Header variables and drafting settings.
///
/// These belong to the *document*, not to application preferences: two open
/// drawings can legitimately have different units, snap settings and linetype
/// scales.
struct DrawingSettings {
    DrawingUnits insertionUnits = DrawingUnits::Millimeters;
    int linearPrecision = 4;
    int angularPrecision = 2;
    /// Angle considered zero, in radians. Usually east.
    double angleBase = 0.0;
    /// True when angles increase clockwise.
    bool angleClockwise = false;

    /// Global linetype scale (LTSCALE).
    double globalLinetypeScale = 1.0;
    /// Linetype scale applied in paper space (PSLTSCALE).
    double paperSpaceLinetypeScale = 1.0;
    /// Point display style (PDMODE) and size (PDSIZE).
    int pointDisplayMode = 0;
    double pointDisplaySize = 0.0;
    /// Whether lineweights are shown on screen (LWDISPLAY).
    bool displayLineweights = false;

    Vec3 limitsMin;
    Vec3 limitsMax{420.0, 297.0, 0.0};
    Vec3 insertionBase;

    Vec3 gridSpacing{10.0, 10.0, 0.0};
    bool gridEnabled = true;
    Vec3 snapSpacing{10.0, 10.0, 0.0};
    bool snapEnabled = false;
    bool orthoEnabled = false;
    /// Polar tracking increment, in radians. Zero disables polar tracking.
    double polarAngleIncrement = kPi / 4.0;
    bool polarTrackingEnabled = false;
    /// Object snap tracking: alignment paths from snap points the pointer has
    /// rested on, so a point can be placed level with one thing and in line
    /// with another without drawing construction geometry to find it.
    bool objectSnapTrackingEnabled = false;

    /// Bitmask of enabled running object snaps (OSMODE).
    int osnapModes = 0b0000'0010'1111;
    bool osnapEnabled = true;
    /// Pick aperture and selection pick box, in pixels.
    int apertureSize = 10;
    int pickboxSize = 5;

    /// The block record new entities go into: model space, a layout's sheet,
    /// or a block being edited in place. Null means model space. Kept by the
    /// shell in step with its tabs; commands draw into whatever it names.
    Handle currentSpaceHandle;

    /// Layers LAYISO turned off, so LAYUNISO can turn exactly those back on
    /// and leave layers that were off already as they were. Not saved.
    std::vector<Handle> isolatedLayers;

    std::string currentLayerName = "0";
    std::string currentLinetypeName = "ByLayer";
    std::string currentTextStyleName = "Standard";
    std::string currentDimStyleName = "Standard";

    /// Header variables read from file that this application does not model,
    /// preserved so saving does not reset a drawing's configuration.
    std::unordered_map<std::string, std::vector<UnknownGroup>> unknownHeaderVariables;
};

/// The drawing database — the single source of truth for a document.
///
/// Everything the application displays or edits lives here. The renderer reads
/// from it and never writes; commands write to it only inside a transaction.
/// That one-way flow is what the architecture rests on: if the renderer held
/// authoritative state, undo, save and multi-viewport display would each need
/// their own reconciliation logic.
///
/// Objects are stored flat, keyed by handle, with ownership expressed through
/// `ownerHandle` rather than by nesting. A flat store means a handle lookup is
/// O(1) regardless of nesting depth, and cross-references between unrelated
/// parts of the drawing — an xref layer named by a viewport's freeze list, say
/// — need no special traversal.
class Database {
public:
    Database();
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) = delete;
    Database& operator=(Database&&) = delete;

    /// Build a database with the standard records every drawing must have.
    ///
    /// A drawing without layer "0", linetype "Continuous", style "Standard" and
    /// a model-space block record is malformed, so these are created up front
    /// rather than lazily — no code downstream should have to defend against
    /// their absence.
    [[nodiscard]] static std::unique_ptr<Database> createEmpty();

    // ------------------------------------------------------------- objects

    /// Add an object, assigning a fresh handle if it does not already have one.
    ///
    /// Objects read from a file arrive with their original handle set, and that
    /// handle is kept — references between them depend on it.
    /// @throws std::invalid_argument on a handle collision.
    DbObject& addObject(DbObjectPtr object);

    /// Typed convenience: adds and returns a reference of the concrete type.
    template <typename T>
    T& add(std::unique_ptr<T> object) {
        T& reference = *object;
        addObject(std::move(object));
        return reference;
    }

    [[nodiscard]] DbObject* getObject(Handle handle);
    [[nodiscard]] const DbObject* getObject(Handle handle) const;

    /// Look up an object and downcast it; null when absent or the wrong type.
    template <typename T>
    [[nodiscard]] T* get(Handle handle) {
        return dynamic_cast<T*>(getObject(handle));
    }

    template <typename T>
    [[nodiscard]] const T* get(Handle handle) const {
        return dynamic_cast<const T*>(getObject(handle));
    }

    [[nodiscard]] Entity* getEntity(Handle handle) { return get<Entity>(handle); }
    [[nodiscard]] const Entity* getEntity(Handle handle) const { return get<Entity>(handle); }

    [[nodiscard]] bool contains(Handle handle) const { return objects_.contains(handle); }

    [[nodiscard]] std::size_t objectCount() const noexcept { return objects_.size(); }

    /// Every object's handle, in a stable order.
    ///
    /// Sorted, because the store is a hash map: without it the OBJECTS section
    /// would come out in a different order on every save, and a drawing under
    /// version control would show a whole-file diff after a no-op edit.
    [[nodiscard]] std::vector<Handle> objectHandles() const;

    /// Mark an object erased.
    ///
    /// Erased objects are retained so undo can restore them and references to
    /// them still resolve. Purging for real is a separate, explicit operation.
    bool eraseObject(Handle handle);

    /// Restore an erased object, re-attaching it to its owner at `index`.
    bool uneraseObject(Handle handle, std::size_t index = static_cast<std::size_t>(-1));

    /// Announce that an object's contents changed.
    void notifyModified(Handle handle);

    /// Permanently remove erased objects that nothing references.
    ///
    /// Deliberately conservative: an object still referenced by anything —
    /// including a reference we did not understand — is kept, because dropping
    /// it would corrupt whatever holds the reference.
    std::size_t purgeErased();

    // -------------------------------------------------------- symbol tables

    Layer& addLayer(std::unique_ptr<Layer> layer);
    Linetype& addLinetype(std::unique_ptr<Linetype> linetype);
    TextStyle& addTextStyle(std::unique_ptr<TextStyle> style);
    DimStyle& addDimStyle(std::unique_ptr<DimStyle> style);
    BlockTableRecord& addBlock(std::unique_ptr<BlockTableRecord> block);

    [[nodiscard]] Layer* findLayer(std::string_view name);
    [[nodiscard]] Linetype* findLinetype(std::string_view name);
    [[nodiscard]] TextStyle* findTextStyle(std::string_view name);
    [[nodiscard]] DimStyle* findDimStyle(std::string_view name);
    [[nodiscard]] BlockTableRecord* findBlock(std::string_view name);

    [[nodiscard]] const SymbolTableIndex& layers() const noexcept { return layers_; }
    [[nodiscard]] const SymbolTableIndex& linetypes() const noexcept { return linetypes_; }
    [[nodiscard]] const SymbolTableIndex& textStyles() const noexcept { return textStyles_; }
    [[nodiscard]] const SymbolTableIndex& dimStyles() const noexcept { return dimStyles_; }
    [[nodiscard]] const SymbolTableIndex& blocks() const noexcept { return blocks_; }

    [[nodiscard]] SymbolTableIndex& layersMutable() noexcept { return layers_; }
    [[nodiscard]] SymbolTableIndex& blocksMutable() noexcept { return blocks_; }

    /// Layer "0", which is guaranteed to exist.
    /// @throws std::logic_error when the database was not initialized properly.
    [[nodiscard]] Layer& defaultLayer();

    /// @throws std::logic_error when model space is missing.
    [[nodiscard]] BlockTableRecord& modelSpace();

    /// The space new entities are drawn into -- see
    /// `DrawingSettings::currentSpaceHandle`. Model space when that names
    /// nothing usable, so a drawing command can never have nowhere to draw.
    [[nodiscard]] BlockTableRecord& currentSpace();

    /// The name the next paper-space block should have: `*Paper_Space` for
    /// the first, then `*Paper_Space0`, `*Paper_Space1`, ... whichever is free
    /// -- the sequence other CAD software uses, so a file written here reads as one
    /// written there.
    [[nodiscard]] std::string nextPaperSpaceName() const;

    /// Find or create a layer by name.
    ///
    /// Used by importers, which must not fail because a drawing references a
    /// layer its own table omitted — a real and surprisingly common defect in
    /// files produced by other software.
    Layer& ensureLayer(std::string_view name);
    Linetype& ensureLinetype(std::string_view name);
    TextStyle& ensureTextStyle(std::string_view name);
    DimStyle& ensureDimStyle(std::string_view name);

    /// The style a dimension should be drawn with.
    ///
    /// Falls back through the drawing's current style to Standard to a
    /// built-in default, because a dimension must draw *something* even in a
    /// file whose style table is incomplete — and files like that exist.
    [[nodiscard]] const DimStyle& dimStyleFor(const DimensionEntity& dimension) const;

    /// The same lookup for anything else that takes its arrows and text from a
    /// dimension style -- a leader does -- given the handle rather than an
    /// entity of one particular type.
    [[nodiscard]] const DimStyle& dimStyleByHandle(Handle handle) const;

    /// The style a dimension actually draws with: its named style plus any
    /// per-entity overrides it carries in XData.
    ///
    /// Returned by value because an overridden style is a new object. The copy
    /// is skipped entirely when there are no overrides, which is the common
    /// case; when there are, a style is two strings and some scalars and the
    /// dimension being drawn costs far more.
    [[nodiscard]] DimStyle effectiveDimStyle(const DimensionEntity& dimension) const;

    // ------------------------------------------------------------ entities

    /// Add an entity to a block record, which may be a layout space.
    ///
    /// Defaults the layer to "0" when unset, so an importer or plugin that
    /// forgets to assign one produces a valid drawing rather than an entity
    /// that cannot resolve its own colour.
    Entity& addEntity(std::unique_ptr<Entity> entity, BlockTableRecord& owner);

    Entity& addToModelSpace(std::unique_ptr<Entity> entity);

    /// Live entities directly owned by a block record, in draw order.
    [[nodiscard]] std::vector<Entity*> entitiesIn(const BlockTableRecord& block);
    [[nodiscard]] std::vector<const Entity*> entitiesIn(const BlockTableRecord& block) const;

    // ------------------------------------------------------------- extents

    /// Extents of a block's contents in its own coordinate system.
    ///
    /// Frozen layers are excluded, matching ZOOM EXTENTS. The depth limit and
    /// visited set guard against a corrupt file whose blocks reference each
    /// other in a cycle, which would otherwise hang the application on load.
    [[nodiscard]] Box3 blockExtents(const BlockTableRecord& block, int maxDepth = 32);

    /// Extents of model space — what ZOOM EXTENTS targets.
    [[nodiscard]] Box3 modelExtents();

    // ---------------------------------------------------------- properties

    /// Build the context needed to resolve an entity's ByLayer/ByBlock
    /// properties. Centralized here because it needs the layer table, which
    /// entities deliberately cannot reach.
    [[nodiscard]] ResolutionContext resolutionContextFor(const Entity& entity) const;

    /// The same, for an entity being drawn inside a block reference.
    [[nodiscard]] ResolutionContext resolutionContextFor(const Entity& entity,
                                                         const Entity& blockReference) const;

    // ------------------------------------------------------------- layouts

    /// Every layout, in tab order, Model first.
    ///
    /// Sorted on demand rather than kept in a list, because tab order is a
    /// property of the layouts themselves and two sources of truth for it would
    /// drift the first time one is renumbered.
    [[nodiscard]] std::vector<Layout*> layouts();
    [[nodiscard]] std::vector<const Layout*> layouts() const;

    [[nodiscard]] Layout* findLayout(std::string_view name);

    /// The layout whose sheet is this block record, or null.
    [[nodiscard]] Layout* layoutForBlock(Handle blockRecord);

    /// Find or create the Model layout.
    ///
    /// A drawing with no layouts at all is legal — a bare DXF written by a
    /// script has none — and the UI still needs a tab to show.
    Layout& ensureModelLayout();

    // --------------------------------------------------------------- xrefs

    /// Every block record that is an external reference.
    [[nodiscard]] std::vector<BlockTableRecord*> externalReferences();

    /// Xrefs that failed to resolve, for the reference manager and status bar.
    [[nodiscard]] std::vector<BlockTableRecord*> unresolvedReferences();

    // -------------------------------------------------------------- events

    /// Register a listener; the returned id removes it.
    std::size_t addListener(DatabaseListener listener);
    void removeListener(std::size_t id);

    /// Run `fn` without emitting per-object events, then emit a single reset.
    ///
    /// Loading a large drawing would otherwise fire hundreds of thousands of
    /// notifications that every listener immediately discards.
    template <typename F>
    decltype(auto) bulkLoad(F&& fn) {
        const bool wasLoading = bulkLoading_;
        bulkLoading_ = true;
        struct Guard {
            Database& db;
            bool previous;
            ~Guard() {
                db.bulkLoading_ = previous;
                if (!previous) {
                    db.notifyListeners({DatabaseEvent::Kind::Reset, Handle::null()});
                }
            }
        } guard{*this, wasLoading};
        return std::forward<F>(fn)();
    }

    /// Note a handle seen during load so future allocations avoid it.
    void observeHandle(Handle handle) { allocator_.observe(handle); }

    /// The handle that would be allocated next, for the file header.
    [[nodiscard]] Handle peekNextHandle() const { return allocator_.peek(); }

    // --------------------------------------------------------- diagnostics

    /// Check referential integrity.
    ///
    /// Run after loading a file and in tests. Real-world DWGs do contain
    /// dangling references, so this reports rather than throws — the drawing
    /// must still open — but the report drives what the UI tells the user.
    [[nodiscard]] std::vector<DatabaseIssue> validate() const;

    // ------------------------------------------------------------ document

    DrawingSettings settings;

    /// Absolute path this drawing was loaded from, if any.
    std::string filePath;

    /// Original file format, so a save can round-trip to the same version.
    std::string sourceFormat;

    /// Whole sections and symbol tables of the source file that this reader
    /// does not interpret, kept verbatim so a save does not delete them.
    ///
    /// The same principle as `DbObject::preserved`, one level up. CLASSES tells
    /// other CAD software how to construct the object types a drawing uses that are not in
    /// its base set — drop it and the objects that depend on it become proxies
    /// or vanish. ACDSDATA is other CAD software's own data store. The VPORT table is how
    /// the model space view is configured. None of them are ours to discard
    /// merely because we have nothing to say about them.
    std::vector<PreservedSection> preservedSections;

    /// Applications registered in the source file's APPID table, in file order.
    ///
    /// Every XData block names an application, and a name with no APPID entry
    /// is a dangling reference that readers are entitled to reject — ODA's does,
    /// with "Invalid RegApp", which is enough to stop a drawing being written at
    /// all. Rebuilding the table from the XData actually seen is not sufficient:
    /// XData hangs off block records, layers and dictionary objects as well as
    /// entities, so a table built from entities alone drops registrations the
    /// rest of the drawing still refers to.
    ///
    /// Kept in file order rather than sorted, because a table that comes back in
    /// a different order is a diff in a file nobody edited.
    std::vector<std::string> registeredApplications;

    /// What each symbol TABLE header carried, by table name.
    ///
    /// other CAD software gives the tables fixed low handles -- BLOCK_RECORD is 1, LAYER
    /// is 2 -- and every record in a table names its table as its owner. A
    /// writer that allocates a fresh handle for the header instead makes every
    /// record in it point at something that does not exist, and anything else
    /// in the file that referred to the real table dangles.
    ///
    /// A table header is a record like any other: it can own an extension
    /// dictionary, and the LAYER table's holds this drawing's layer states and
    /// filters. Dropping its `102` group leaves that dictionary unreferenced
    /// and it is discarded with everything under it.
    struct SymbolTableHeader {
        Handle handle;
        std::vector<UnknownGroup> applicationGroups;

        /// Records in a modelled table that this reader still cannot represent,
        /// kept verbatim and written back among their siblings.
        ///
        /// The STYLE table is the case: an entry with an empty name is a *shape
        /// file* registration rather than a text style, and the symbol index is
        /// keyed by name, so there is nowhere to put it. Dropping it silently
        /// deletes the drawing's registration of a shape file it uses.
        std::vector<std::vector<UnknownGroup>> unmodelledRecords;
    };

    std::map<std::string, SymbolTableHeader> symbolTableHeaders;

private:
    friend class Transaction;

    /// Deliberately not named `emit`: Qt defines that as a macro, and a core
    /// header must not become unparseable merely because some *other* layer
    /// includes Qt. (The app additionally builds with QT_NO_KEYWORDS.)
    void notifyListeners(const DatabaseEvent& event);

    [[nodiscard]] Box3 blockExtentsImpl(const BlockTableRecord& block, int maxDepth,
                                        std::vector<Handle>& visiting);

    /// The name index an object belongs in, or null when it is not a symbol
    /// table record. Used to keep the indices consistent through erase and
    /// unerase, which undo drives.
    [[nodiscard]] SymbolTableIndex* tableFor(const DbObject& object) noexcept;

    std::unordered_map<Handle, DbObjectPtr> objects_;
    HandleAllocator allocator_{1};

    SymbolTableIndex layers_{"Layers"};
    SymbolTableIndex linetypes_{"Linetypes"};
    SymbolTableIndex textStyles_{"TextStyles"};
    SymbolTableIndex dimStyles_{"DimStyles"};
    SymbolTableIndex blocks_{"Blocks"};

    std::vector<std::pair<std::size_t, DatabaseListener>> listeners_;
    std::size_t nextListenerId_ = 1;
    bool bulkLoading_ = false;
};

}  // namespace cad
