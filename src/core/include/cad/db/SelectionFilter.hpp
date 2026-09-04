#pragma once

#include <optional>
#include <string>
#include <vector>

#include "cad/db/Property.hpp"

namespace cad {

class Database;
class BlockTableRecord;

/// A query over the drawing, as QSELECT builds one.
///
/// The point of quick select is reaching objects that share a property but are
/// nowhere near each other — every door block, every dimension on a demolition
/// layer, everything red. On a drawing with eight thousand objects that is not
/// a convenience: window-selecting them is not possible at all.
///
/// Filtering runs through the *property registry*, the same one behind the
/// properties palette, rather than a hand-written list of filterable fields.
/// That means an entity type or property contributed by a plugin is queryable
/// with no change here — which is the whole reason the registry is generic.
struct SelectionFilter {
    enum class Comparison : std::uint8_t {
        Equals,
        NotEquals,
        Greater,
        Less,
        /// Substring match, for text properties.
        Contains,
    };

    /// How the result joins what is already selected.
    enum class Mode : std::uint8_t {
        /// Replace the selection with the matches.
        Replace,
        /// Add matches to the selection.
        Add,
        /// Remove matches from the selection.
        Remove,
    };

    /// `Entity::objectType()` to match, or empty for every type.
    std::string objectType;

    /// Property id from the registry, e.g. `general.layer`. Empty filters on
    /// type alone, which is how "select every dimension" is expressed.
    std::string property;

    Comparison comparison = Comparison::Equals;
    PropertyValue value;

    Mode mode = Mode::Replace;

    /// Search only these objects rather than the whole space. This is
    /// other CAD software's "apply to current selection", and it is what makes successive
    /// queries narrow rather than start over.
    std::optional<std::vector<Handle>> within;
};

/// Everything in `space` the filter matches, in draw order.
///
/// Objects on a locked or frozen layer are excluded: a selection the user
/// cannot then edit is a trap, and it is the same rule picking already applies.
[[nodiscard]] std::vector<Handle> findByFilter(
    const Database& db, const BlockTableRecord& space, const SelectionFilter& filter,
    const PropertyRegistry& registry = PropertyRegistry::standard());

/// Combine matches with an existing selection according to the filter's mode.
[[nodiscard]] std::vector<Handle> applySelectionMode(const std::vector<Handle>& current,
                                                     const std::vector<Handle>& matches,
                                                     SelectionFilter::Mode mode);

/// Compare two property values. Ordering comparisons are only meaningful for
/// numbers; for anything else `Greater` and `Less` are false rather than an
/// error, so a nonsensical query returns nothing instead of failing.
[[nodiscard]] bool comparePropertyValues(const PropertyValue& left, const PropertyValue& right,
                                         SelectionFilter::Comparison comparison);

}  // namespace cad
