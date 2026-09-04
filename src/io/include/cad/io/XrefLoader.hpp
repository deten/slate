#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "cad/db/Database.hpp"
#include "cad/io/DxfReader.hpp"
#include "cad/io/ResourceResolver.hpp"

namespace cad::io {

/// Translates a DWG file into DXF text this application can read.
///
/// DWG is a closed binary format. Rather than inventing a parser — a multi-year
/// effort that would permanently trail other CAD software — this interface defines *how*
/// DWG data enters the application, and an implementation over the free ODA
/// File Converter supplies it. The interface, not the converter, is the
/// architectural commitment: a later native ODA SDK binding must be
/// substitutable here without any caller changing.
///
/// See docs/adr/0001-dwg-strategy.md.
class DwgTranslator {
public:
    DwgTranslator() = default;
    virtual ~DwgTranslator() = default;

    DwgTranslator(const DwgTranslator&) = delete;
    DwgTranslator& operator=(const DwgTranslator&) = delete;
    DwgTranslator(DwgTranslator&&) = delete;
    DwgTranslator& operator=(DwgTranslator&&) = delete;

    [[nodiscard]] virtual std::string id() const = 0;
    [[nodiscard]] virtual std::string displayName() const = 0;

    /// Whether this translator can run right now, and why not if it cannot.
    struct Status {
        bool available = false;
        std::string problem;
        /// What the user should do about it, shown verbatim in the UI.
        std::string remedy;
    };

    [[nodiscard]] virtual Status probe() const = 0;

    /// Translate a DWG file to DXF text.
    /// @throws std::runtime_error carrying a message the UI can show.
    [[nodiscard]] virtual std::string toDxf(const std::filesystem::path& source) const = 0;

    /// Whether this translator can also write DWG.
    ///
    /// Asked rather than assumed, because reading and writing are separable: a
    /// direct-read binding might never write, and requiring it to would keep a
    /// perfectly good reader out. The UI asks this to decide whether to offer
    /// DWG when saving, so a translator that cannot write produces a file
    /// dialog that does not promise one.
    [[nodiscard]] virtual bool canWriteDwg() const noexcept { return false; }

    /// Write DXF text out as a DWG.
    ///
    /// @throws std::runtime_error when this translator does not write, so a
    /// caller that skipped `canWriteDwg` fails loudly rather than silently
    /// producing nothing.
    virtual void writeDwg(const std::string& dxfText, const std::filesystem::path& target) const;
};

/// State of one node in the reference graph, for the reference manager UI.
struct XrefNode {
    /// Block record in the host database representing this reference.
    Handle blockHandle;
    std::string name;
    /// Path exactly as stored in the referencing drawing.
    std::string storedPath;
    /// Absolute path it resolved to, when found.
    std::filesystem::path resolvedPath;
    XrefStatus status = XrefStatus::NotAnXref;
    XrefType type = XrefType::None;
    /// Nesting depth; 0 for a reference attached directly to the host.
    int depth = 0;
    /// Entities brought in from this reference.
    std::size_t entityCount = 0;
    /// Why it failed, when it did.
    std::string problem;
    std::vector<std::filesystem::path> attemptedPaths;
};

struct XrefLoadResult {
    std::vector<XrefNode> nodes;
    std::vector<DxfDiagnostic> diagnostics;
    std::size_t resolvedCount = 0;
    std::size_t failedCount = 0;
};

struct XrefLoadOptions {
    /// Maximum nesting depth. Guards against pathological reference chains.
    int maxDepth = 16;
    /// Search paths for resolution, in priority order.
    std::vector<std::filesystem::path> searchPaths;
    /// Needed when a reference points at a DWG rather than a DXF.
    const DwgTranslator* translator = nullptr;
    /// Skip overlays when nested, which is what other CAD software does and what stops
    /// site plans multiplying through a drawing set.
    bool skipNestedOverlays = true;
    std::function<void(const std::string&)> onProgress;
};

/// Loads external references into a host database.
///
/// Three requirements shape this design:
///
///  1. **A missing reference must never prevent the host from opening.** Every
///     failure is recorded on the node and in the diagnostics, and loading
///     continues. Nothing here throws for a resolution failure.
///  2. **Nested references must load.** An xref may itself reference others,
///     several deep in professional drawings. Each level resolves relative to
///     *its own* file, not the host's, so a fresh resolver is derived per
///     drawing.
///  3. **Cycles must be detected, not survived by luck.** A references B
///     references A occurs in real project sets, usually by accident. The
///     visited set is keyed on the resolved absolute path, because the same
///     file can be reached by different stored paths.
///
/// Content is merged under the xref's block record, with symbol table records
/// renamed to the `xrefname|recordname` convention so a layer named "Walls" in
/// a reference never collides with — or silently alters — the host's own.
class XrefLoader {
public:
    explicit XrefLoader(Database& host) : host_(host) {}

    /// Resolve and load every external reference declared in the host database.
    [[nodiscard]] XrefLoadResult loadAll(const std::filesystem::path& hostPath,
                                         const XrefLoadOptions& options = {});

    /// Load one reference that was just attached, leaving the others alone.
    ///
    /// Loading fills the block by appending, so a reference that is already
    /// resolved must not be loaded again; this is the way to bring in a new
    /// one without touching those.
    [[nodiscard]] XrefLoadResult loadReference(BlockTableRecord& block,
                                               const std::filesystem::path& hostPath,
                                               const XrefLoadOptions& options = {});

private:
    void loadInto(Database& database, ResourceResolver& resolver, int depth,
                  const XrefLoadOptions& options);

    void loadOne(BlockTableRecord& block, ResourceResolver& resolver, int depth,
                 const XrefLoadOptions& options);

    /// Read a referenced drawing, translating from DWG when necessary.
    [[nodiscard]] DxfReadResult readDrawing(const std::filesystem::path& path,
                                            const XrefLoadOptions& options) const;

    /// Copy a referenced drawing's model space into the host, under `block`.
    [[nodiscard]] std::size_t mergeInto(BlockTableRecord& block, Database& source);

    Database& host_;
    std::vector<XrefNode> nodes_;
    std::vector<DxfDiagnostic> diagnostics_;
    /// Absolute paths currently being loaded, for cycle detection.
    std::set<std::filesystem::path> loading_;
};

/// A file the drawing *displays* rather than merges: a raster image, a PDF or
/// DGN underlay.
///
/// Reported separately from xrefs because the failure looks different to a
/// user. A missing xref leaves a gap where geometry should be; a missing image
/// leaves a rectangle, and the only thing that turns that rectangle back into a
/// site photograph is somebody being told which path could not be found.
struct AttachedFileNode {
    /// The IMAGEDEF or underlay definition object.
    Handle definitionHandle;
    /// "Image", "PDF", "DWF" or "DGN".
    std::string kind;
    /// Path exactly as stored in the drawing.
    std::string storedPath;
    /// Absolute path it resolved to, when found.
    std::filesystem::path resolvedPath;
    bool found = false;
    /// How many entities place this file.
    std::size_t placements = 0;
    /// How many of those placements are actually reached from a layout.
    ///
    /// Not the same number, and the difference is the answer to "the path is
    /// repaired, so why is there still nothing on the screen". An IMAGE inside
    /// a block that no layout inserts is in the drawing and is drawn nowhere;
    /// counting it as a placement says the file is in use when it is not.
    std::size_t drawnPlacements = 0;
    /// Blocks holding placements that no layout reaches, named so the user can
    /// look for them. Empty when every placement is drawn.
    std::vector<std::string> unplacedBlocks;
    std::string problem;
};

/// List every attached image and underlay, without touching the filesystem.
///
/// The state as the database currently holds it: what each definition says its
/// path is, where it was last found, and how many entities place it. For a
/// reference manager, which has to show what is wrong before anything is done
/// about it — and must not stall on a network path that is slow to fail.
[[nodiscard]] std::vector<AttachedFileNode> attachedFiles(const Database& db);

/// Point a definition at a different file.
///
/// The *stored* path is what changes, because that is what the drawing keeps
/// and what everyone else who opens it will resolve. Writing the absolute path
/// we happened to resolve would fix the file on this machine and break it on
/// every other, which is the failure this whole mechanism exists to avoid — so
/// a path inside the drawing's own folder is stored relative to it.
///
/// @returns false when the handle is not a definition.
bool repathAttachedFile(Database& db, Handle definition, const std::filesystem::path& file);

/// Resolve every attached image and underlay against the filesystem.
///
/// Resolution is recorded on the definitions themselves, so that everything
/// downstream — the renderer, the properties palette, a future reference
/// manager — asks the definition rather than repeating the search.
[[nodiscard]] std::vector<AttachedFileNode> resolveAttachedFiles(
    Database& db, const std::filesystem::path& hostPath, const XrefLoadOptions& options = {});

/// Load a drawing and all its references in one call.
struct DrawingLoadResult {
    std::unique_ptr<Database> database;
    std::vector<DxfDiagnostic> diagnostics;
    XrefLoadResult xrefs;
    std::vector<AttachedFileNode> attachments;
};

[[nodiscard]] DrawingLoadResult loadDrawingWithReferences(const std::filesystem::path& path,
                                                          const XrefLoadOptions& options = {});

/// True when a path looks like a DWG by extension.
[[nodiscard]] bool isDwgPath(const std::filesystem::path& path);

}  // namespace cad::io
