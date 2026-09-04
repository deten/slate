#pragma once

#include <map>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "cad/db/DbObject.hpp"
#include "cad/io/DxfTokenizer.hpp"
#include "cad/math/Vec3.hpp"

namespace cad::io {

/// The accumulated group pairs describing one DXF object.
///
/// DXF gives no schema: an object is whatever groups appear between its type
/// marker and the next. Reading is therefore "pull the codes I understand, and
/// keep the rest". This class provides the pulling; the *keeping* is what makes
/// structure preservation possible, and is why consumed codes are tracked.
///
/// Repeated codes are common and meaningful — a polyline's vertex coordinates,
/// an MTEXT's continued contents, a hatch boundary's edges — so every code maps
/// to a list, not a single value.
///
/// A record can also carry a second object inside it, introduced by group
/// `101 Embedded Object`. Everything from that marker onwards belongs to the
/// embedded object and *restarts the group-code numbering* — so the record's
/// own accessors must not see it. An annotative MTEXT is the common case: its
/// embedded object repeats the insertion point as code 11, which is the MTEXT's
/// own code for its X-axis direction vector. Read together, a horizontal room
/// label acquires the angle of its own position vector and renders askew.
class DxfRecord {
public:
    DxfRecord(std::string type, std::vector<DxfGroup> groups);

    [[nodiscard]] const std::string& type() const noexcept { return type_; }

    /// The record's own groups in file order, which some readers depend on.
    /// Stops at any embedded object.
    [[nodiscard]] std::span<const DxfGroup> groups() const noexcept {
        return std::span{groups_}.first(ownCount_);
    }

    /// The embedded object's groups, in file order, starting with the code-101
    /// marker itself and excluding any trailing XData.
    ///
    /// Uninterpreted: this exists so the tail can be written back exactly as it
    /// arrived, rather than scattered among the unknown codes where its reused
    /// codes would be sorted into nonsense.
    [[nodiscard]] const std::vector<UnknownGroup>& embeddedGroups() const noexcept {
        return embedded_;
    }

    [[nodiscard]] bool has(int code) const { return byCode_.contains(code); }

    /// The owner handle: the code-330 that is not inside a `102` group.
    ///
    /// `text(330)` is the wrong way to ask. Code 330 is also how a record names
    /// its *reactors*, and those are wrapped in an application group —
    /// `102 {ACAD_REACTORS}` … `102 }` — that comes **before** the owner. On any
    /// drawing where other CAD software attached a reactor, the first 330 is a reactor
    /// handle, so reading it as the owner files the entity under whatever that
    /// handle happens to resolve to, or under nothing at all.
    ///
    /// It resolves to nothing far more often than it resolves to something
    /// wrong, which is exactly why this survived: an entity with no owner falls
    /// back to model space, and most entities belong there anyway. The failure
    /// waits for a reactor on a paper-space entity, and then a sheet's contents
    /// land on the model.
    [[nodiscard]] std::optional<std::string> ownerHandleText() const;

    /// Every value for a code, marking it consumed.
    [[nodiscard]] std::vector<std::string> allStrings(int code) const;

    [[nodiscard]] std::string text(int code, std::string fallback = {}) const;
    [[nodiscard]] std::optional<std::string> optionalText(int code) const;

    [[nodiscard]] double number(int code, double fallback = 0.0) const;
    [[nodiscard]] std::optional<double> optionalNumber(int code) const;

    [[nodiscard]] std::int64_t integer(int code, std::int64_t fallback = 0) const;
    [[nodiscard]] std::optional<std::int64_t> optionalInteger(int code) const;

    [[nodiscard]] bool boolean(int code, bool fallback = false) const;

    /// Read a point from the `code`, `code+10`, `code+20` triple.
    ///
    /// A missing Z is 0 rather than an error: 2D DXF omits it routinely, and
    /// treating that as a failure would reject most real files.
    [[nodiscard]] Vec3 point(int xCode, const Vec3& fallback = Vec3::zero()) const;
    [[nodiscard]] std::optional<Vec3> optionalPoint(int xCode) const;

    /// Extrusion direction (code 210), defaulting to world +Z and normalized —
    /// a file may store an unnormalized vector, and every consumer assumes a
    /// unit normal.
    [[nodiscard]] Vec3 extrusion() const;

    /// Extended data, grouped by the registered application name.
    [[nodiscard]] const std::unordered_map<std::string, std::vector<XDataItem>>& xdata()
        const noexcept {
        return xdata_;
    }

    /// Group codes this reader did not claim.
    ///
    /// Everything here is written back out on save. Without it, opening and
    /// saving a drawing would quietly strip every feature not yet implemented —
    /// the single most damaging thing a CAD file reader can do.
    [[nodiscard]] std::vector<UnknownGroup> unconsumed() const;

    /// Mark codes consumed without reading them, for codes handled inline.
    void markConsumed(std::initializer_list<int> codes) const;

private:
    [[nodiscard]] const DxfGroup* first(int code) const;

    std::string type_;
    std::vector<DxfGroup> groups_;
    /// How many leading groups belong to this record rather than to an
    /// embedded object. Equals `groups_.size()` when there is no embedded
    /// object, which is the overwhelmingly common case.
    std::size_t ownCount_ = 0;
    /// The embedded object's groups in file order, kept verbatim for writing.
    std::vector<UnknownGroup> embedded_;
    /// Code -> indices into `groups_`, in order.
    std::map<int, std::vector<std::size_t>> byCode_;
    /// Mutable so the const accessors can record what they claimed; this is
    /// bookkeeping, not observable object state.
    mutable std::vector<int> consumed_;
    std::unordered_map<std::string, std::vector<XDataItem>> xdata_;
};

}  // namespace cad::io
