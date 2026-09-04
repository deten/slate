#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "cad/db/Color.hpp"
#include "cad/db/DbObject.hpp"
#include "cad/math/Vec3.hpp"

namespace cad {

class Database;
class Transaction;

/// What kind of value a property holds, and therefore how it is edited.
///
/// Finer-grained than the underlying C++ type on purpose: `Distance` and
/// `Angle` are both doubles, but one is shown in drawing units and the other in
/// degrees, and conflating them puts radians in front of the user.
enum class PropertyType : std::uint8_t {
    Bool,
    Integer,
    /// A plain number with no unit.
    Number,
    /// A length in drawing units.
    Distance,
    /// An angle, stored in radians and shown in degrees.
    Angle,
    Text,
    Color,
    /// A reference to a symbol table record, chosen from that table.
    Reference,
    /// A fixed set of named values.
    Enumeration,
    Point,
};

/// Which symbol table a `Reference` property draws its candidates from.
enum class SymbolTableKind : std::uint8_t { None, Layers, Linetypes, TextStyles, DimStyles, Blocks };

using PropertyValue =
    std::variant<bool, std::int64_t, double, std::string, Color, Handle, Vec3>;

/// One option of an `Enumeration` property.
struct PropertyOption {
    std::int64_t value = 0;
    std::string label;
};

/// A property an object exposes for inspection and editing.
///
/// Descriptive data plus a getter and setter, rather than a virtual method per
/// property, because the *consumer* is generic: the properties palette, a
/// future scripting binding and the plugin ABI all want to enumerate what an
/// object has without knowing its type. An entity type added by a plugin
/// therefore appears in the palette with no change to the palette.
struct PropertyDefinition {
    /// Stable identifier, e.g. `geometry.radius`. Two objects of different
    /// types sharing an id means the property can be edited across a mixed
    /// selection — which is exactly why `general.layer` is spelled the same
    /// everywhere.
    std::string id;
    /// Shown to the user.
    std::string name;
    /// Groups rows in the palette: "General", "Geometry", "Text".
    std::string category;

    PropertyType type = PropertyType::Text;
    /// Candidates for a `Reference`.
    SymbolTableKind referenceTable = SymbolTableKind::None;
    /// Choices for an `Enumeration`.
    std::vector<PropertyOption> options;

    /// Read-only properties are shown but not editable — area and length are
    /// worth seeing and meaningless to set.
    bool readOnly = false;

    std::function<PropertyValue(const DbObject&)> get;
    /// Null exactly when `readOnly`.
    std::function<void(DbObject&, const PropertyValue&)> set;
};

/// The catalogue of properties, by object type.
///
/// Keyed on `DbObject::objectType()` rather than on a type id, so a plugin
/// registers against the same string its entity reports. Every object also
/// receives the entity-wide properties (layer, colour, linetype and the rest)
/// when it is an entity, so a new type gets those for free.
class PropertyRegistry {
public:
    /// A registry seeded with the built-in types.
    [[nodiscard]] static const PropertyRegistry& standard();

    /// Add properties for an object type. Later registrations for the same
    /// type append rather than replace, so a plugin can extend a built-in.
    void registerFor(std::string objectType, std::vector<PropertyDefinition> properties);

    /// Properties that apply to *every* entity.
    void registerCommon(std::vector<PropertyDefinition> properties);

    /// Everything `object` exposes, common properties first.
    [[nodiscard]] std::vector<const PropertyDefinition*> propertiesFor(
        const DbObject& object) const;

    [[nodiscard]] const PropertyDefinition* find(const DbObject& object,
                                                 const std::string& id) const;

private:
    std::vector<PropertyDefinition> common_;
    std::unordered_map<std::string, std::vector<PropertyDefinition>> byType_;
};

/// One row of a properties palette showing a whole selection.
///
/// A palette over several objects shows what they agree on and marks the rest
/// as varying — and, crucially, editing a varying row sets *all* of them. That
/// behaviour is the reason this is computed here rather than in the UI: the
/// same reduction is needed by scripting and by any future property API.
struct AggregatedProperty {
    const PropertyDefinition* definition = nullptr;
    /// The shared value, or empty when the objects disagree.
    std::optional<PropertyValue> value;
    /// How many of the selected objects have this property at all.
    std::size_t count = 0;
    /// True when at least two objects disagree.
    bool varies = false;
};

/// Reduce a selection to the properties they have in common.
///
/// An object missing a property simply does not contribute; a property only
/// *appears* when every selected object has it, because a row that silently
/// applies to half a selection is worse than no row.
[[nodiscard]] std::vector<AggregatedProperty> aggregateProperties(
    const Database& db, const std::vector<Handle>& handles,
    const PropertyRegistry& registry = PropertyRegistry::standard());

/// Set one property on many objects, as a single undoable change.
///
/// @returns how many objects were changed. Objects lacking the property, on a
///   locked layer, or already holding the value are skipped — so a no-op edit
///   leaves no undo entry.
std::size_t applyProperty(Transaction& transaction, const std::vector<Handle>& handles,
                          const std::string& id, const PropertyValue& value,
                          const PropertyRegistry& registry = PropertyRegistry::standard());

/// True when two property values are the same, comparing doubles by tolerance
/// so a palette does not report a difference the user cannot see.
[[nodiscard]] bool propertyValuesEqual(const PropertyValue& a, const PropertyValue& b);

}  // namespace cad
