#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cad/db/Database.hpp"
#include "cad/io/DxfTokenizer.hpp"

namespace cad::io {

/// Accumulates DXF group pairs into text.
///
/// A thin writer rather than a formatting library, because DXF's rules are
/// specific and mechanical: CRLF endings, the code on one line and the value on
/// the next, and — critically — a *fixed decimal representation* for doubles.
/// Scientific notation is legal in the format but chokes several real
/// consumers, so it is never emitted.
class DxfBuilder {
public:
    void pair(int code, std::string_view value);
    void pair(int code, const char* value) { pair(code, std::string_view{value}); }
    void pair(int code, const std::string& value) { pair(code, std::string_view{value}); }
    void pair(int code, double value);
    void pair(int code, int value);
    void pair(int code, std::int64_t value);
    void pair(int code, Handle handle);
    void pair(int code, bool value);

    /// Write a point as its `code`, `code+10`, `code+20` triple.
    void point(int xCode, const Vec3& value, bool includeZ = true);

    /// Emit an object's preserved group codes and XData.
    ///
    /// This is the payoff for keeping them on read: a drawing containing
    /// features this application does not implement survives an
    /// open-edit-save cycle intact instead of being silently stripped.
    void preserved(const DbObject& object);

    /// A record's handle followed by its `102 {...}` application groups.
    ///
    /// One call rather than two so the groups cannot drift away from the header
    /// slot they have to occupy: an extension dictionary referenced from the
    /// end of a record is an extension dictionary other readers discard.
    void identity(const DbObject& object);

    /// The same, for a record whose handle is written under a different code.
    /// DIMSTYLE is the one case: its handle is group 105, not 5.
    void identity(const DbObject& object, int handleCode);

    /// Append already-formatted group pairs.
    ///
    /// Used to assemble the file out of order: the body is generated first so
    /// the header can report a `$HANDSEED` that exceeds every handle the body
    /// actually used.
    void append(std::string_view formatted) { text_ += formatted; }

    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    [[nodiscard]] std::string take() { return std::move(text_); }

private:
    std::string text_;
};

/// Turns an entity into DXF group pairs.
///
/// Registered per type, mirroring the reader, so support for a new entity type
/// is symmetric — and so a type that can be read but not written is an obvious
/// gap rather than a silent one.
using EntityWriter = std::function<void(const Entity&, const Database&, DxfBuilder&)>;

class EntityWriterRegistry {
public:
    [[nodiscard]] static EntityWriterRegistry standard();

    void registerWriter(std::string dxfType, EntityWriter writer);

    /// Look up by the entity's own object type, e.g. "Line".
    [[nodiscard]] const EntityWriter* find(const std::string& objectType) const;

    /// The DXF type name to emit for an entity's object type.
    [[nodiscard]] std::string dxfTypeFor(const Entity& entity) const;

private:
    struct Registration {
        std::string dxfType;
        EntityWriter writer;
    };
    std::unordered_map<std::string, Registration> writers_;
};

struct DxfWriteOptions {
    /// Version written to `$ACADVER`. other CAD software by default, which every
    /// current consumer reads.
    std::string version = "AC1032";
};

/// Writes a database as ASCII DXF.
///
/// Round-tripping is the requirement, not merely producing a readable file:
/// what was read must come back out, including the parts this application does
/// not interpret. Anything it cannot write is reported rather than dropped
/// quietly.
class DxfWriter {
public:
    DxfWriter();
    explicit DxfWriter(EntityWriterRegistry registry);

    [[nodiscard]] std::string write(const Database& db, const DxfWriteOptions& options = {});

    /// Problems encountered while writing — chiefly entity types with no
    /// registered writer.
    [[nodiscard]] const std::vector<DxfDiagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }

private:
    void writeHeader(const Database& db, const DxfWriteOptions& options, DxfBuilder& out);
    void writeTables(const Database& db, DxfBuilder& out);
    void writeBlocks(const Database& db, DxfBuilder& out);
    void writeEntities(const Database& db, DxfBuilder& out);

    /// Layouts, plus everything from the file's own OBJECTS section that was
    /// kept verbatim. Both have to be written or a save deletes them.
    void writeObjects(const Database& db, DxfBuilder& out);

    /// Emit one preserved structure's groups, without a section wrapper.
    static void writePreservedSection(const PreservedSection& section, DxfBuilder& out);

    /// Emit the preserved sections, wrapped. `classes` selects the CLASSES
    /// section, which goes immediately after HEADER; everything else goes after
    /// OBJECTS, with ACDSDATA last because that is where the format puts it.
    static void writePreservedSections(const Database& db, DxfBuilder& out, bool classes);

    void writeEntity(const Entity& entity, const Database& db, DxfBuilder& out);

    /// Common properties every entity carries: handle, owner, layer, colour,
    /// linetype, lineweight, transparency, extrusion.
    void writeCommonProperties(const Entity& entity, const Database& db, DxfBuilder& out);

    /// Which associative hatches each boundary entity feeds, gathered before
    /// any entity is written: the boundary has to name its hatches in a
    /// reactor list, or other CAD software makes the hatch static on open.
    void collectHatchReactors(const Database& db);
    std::unordered_map<Handle, std::vector<Handle>> hatchReactors_;

    /// The one record for an entity, common properties through preserved
    /// groups. `writeEntity` wraps it with what an entity's *place* in the
    /// file requires: an attribute is written only after its block reference,
    /// and a reference's attributes are followed by SEQEND.
    void writeRecord(const Entity& entity, const Database& db, DxfBuilder& out);
    void collectOwnedAttributes(const Database& db);
    std::unordered_set<Handle> ownedAttributes_;

    /// A handle for a structural record the database does not own — chiefly
    /// ENDBLK, which DXF requires to carry one.
    ///
    /// Derived from the database's next-free handle rather than from a nearby
    /// object's, so a fabricated handle can never collide with a real one.
    [[nodiscard]] Handle nextSpareHandle();

    EntityWriterRegistry registry_;
    std::vector<DxfDiagnostic> diagnostics_;
    std::uint64_t nextSpare_ = 1;
};

/// Write a database to DXF text using the standard entity writers.
[[nodiscard]] std::string writeDxf(const Database& db, const DxfWriteOptions& options = {});

/// Write a database to a file.
/// @throws std::runtime_error when the file cannot be written.
void writeDxfFile(const Database& db, const std::filesystem::path& path,
                  const DxfWriteOptions& options = {});

}  // namespace cad::io
