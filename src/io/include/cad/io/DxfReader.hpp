#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "cad/db/Database.hpp"
#include "cad/io/DxfRecord.hpp"

namespace cad::io {

/// Outcome of reading a DXF file.
struct DxfReadResult {
    std::unique_ptr<Database> database;
    std::vector<DxfDiagnostic> diagnostics;
    /// `$ACADVER` value, e.g. `AC1032`.
    std::string version;
    std::string versionName;
    std::size_t entityCount = 0;

    /// True when no error-severity diagnostic was recorded. Warnings are
    /// expected on real files and do not make a read unsuccessful.
    [[nodiscard]] bool succeeded() const;
};

/// Context shared across entity reads within one file.
///
/// Carries the database being populated and the deferred work that cannot be
/// done until the whole file is read — chiefly resolving names to handles,
/// since a DXF entity may reference a layer or block defined later.
struct ReadContext {
    Database& db;
    std::vector<DxfDiagnostic>& diagnostics;
    std::vector<std::function<void()>>& deferred;
    /// Block name (normalized) to record handle, filled as blocks are read.
    std::unordered_map<std::string, Handle>& blockNamesToHandles;
};

/// Turns a DXF record into an entity.
///
/// Readers live in a registry rather than a switch, so support for a new entity
/// type — including from a plugin — is an entry in a map, with no change to the
/// reader's control flow.
using EntityReader = std::function<std::unique_ptr<Entity>(const DxfRecord&, ReadContext&)>;

/// A placeholder for an entity type this reader does not implement.
///
/// The alternative — dropping the entity — would silently delete geometry from
/// a professional drawing on save. A placeholder keeps every group code, so the
/// object survives a round-trip even though it cannot yet be displayed or
/// edited. It renders as nothing and does not participate in extents.
class UnsupportedEntity final : public Entity {
public:
    std::string dxfType;

    [[nodiscard]] std::string objectType() const override {
        return "Unsupported:" + dxfType;
    }

    /// Contributes nothing to extents: an entity whose geometry was never
    /// interpreted would otherwise report a bogus box and break ZOOM EXTENTS.
    [[nodiscard]] Box3 bounds() const override { return Box3::empty(); }

    [[nodiscard]] double distanceToPoint(const Vec3&) const override {
        return std::numeric_limits<double>::infinity();
    }

    /// Cannot transform geometry that was never interpreted. Ignoring is the
    /// honest behaviour; the alternative is corrupting preserved data -- but
    /// ignoring it *silently* is not, so commands ask `canTransform` first and
    /// report what they left alone.
    void transformBy(const Mat4&) override {}

    [[nodiscard]] bool canTransform() const override { return false; }

    [[nodiscard]] DbObjectPtr clone() const override;
    void restoreFrom(const DbObject& snapshot) override;
};

/// The registry mapping DXF type names to readers.
///
/// Exported so a plugin can register support for an entity type without
/// modifying the reader — one of the extensibility requirements.
class EntityReaderRegistry {
public:
    /// A registry seeded with the built-in entity readers.
    [[nodiscard]] static EntityReaderRegistry standard();

    void registerReader(std::string dxfType, EntityReader reader);

    [[nodiscard]] const EntityReader* find(const std::string& dxfType) const;

    [[nodiscard]] std::size_t size() const noexcept { return readers_.size(); }

private:
    std::unordered_map<std::string, EntityReader> readers_;
};

/// Reader for ASCII DXF.
///
/// Structured as a section dispatcher over a group cursor. The design priority
/// throughout is *resilience*: a real professional drawing is large, was
/// produced by software of varying quality, and frequently contains
/// inconsistencies. A reader that throws on the first surprise is useless for
/// the stated goal of opening real files, so problems are collected as
/// diagnostics and reading continues wherever the file remains interpretable.
class DxfReader {
public:
    DxfReader();
    explicit DxfReader(EntityReaderRegistry registry);

    /// Read DXF text into a fresh database.
    [[nodiscard]] DxfReadResult read(std::string_view text);

private:
    class Impl;

    EntityReaderRegistry registry_;
};

/// Read DXF text into a fresh database, using the standard entity readers.
[[nodiscard]] DxfReadResult readDxf(std::string_view text);

/// Apply the properties every entity shares.
///
/// Called for every entity before its type-specific fields, so a new entity
/// type automatically gets correct layer, colour and linetype handling.
void readCommonProperties(Entity& entity, const DxfRecord& record, ReadContext& context);

/// Decode the escape sequences DXF uses inside string values.
///
/// `%%d`-style codes are legacy symbol escapes that still appear constantly in
/// older drawings — degree signs on angular dimensions, diameter symbols on
/// hole callouts. Leaving them raw shows users `%%c` instead of a diameter sign.
[[nodiscard]] std::string decodeDxfText(std::string_view raw);

/// Strip other CAD software's MTEXT inline formatting codes, leaving readable text.
///
/// Handles the codes that appear in practically every real drawing: paragraph
/// breaks, font and colour runs, stacked fractions, and the brace grouping that
/// scopes them. The raw contents are preserved on the entity; this is only for
/// display and measurement.
[[nodiscard]] std::string stripMTextFormatting(std::string_view raw);

}  // namespace cad::io
