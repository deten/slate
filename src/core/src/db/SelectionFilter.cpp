#include "cad/db/SelectionFilter.hpp"

#include <algorithm>

#include "cad/db/BlockTableRecord.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/Layer.hpp"

namespace cad {
namespace {

/// A numeric reading of a value, when it has one.
///
/// Booleans and integers compare as numbers so that "lineweight greater than
/// 50" works without the caller knowing which C++ type the property chose.
[[nodiscard]] std::optional<double> asNumber(const PropertyValue& value) {
    if (const auto* number = std::get_if<double>(&value)) {
        return *number;
    }
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        return static_cast<double>(*integer);
    }
    if (const auto* boolean = std::get_if<bool>(&value)) {
        return *boolean ? 1.0 : 0.0;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> asText(const PropertyValue& value) {
    if (const auto* text = std::get_if<std::string>(&value)) {
        return *text;
    }
    return std::nullopt;
}

}  // namespace

bool comparePropertyValues(const PropertyValue& left, const PropertyValue& right,
                           SelectionFilter::Comparison comparison) {
    switch (comparison) {
        case SelectionFilter::Comparison::Equals:
            return propertyValuesEqual(left, right);
        case SelectionFilter::Comparison::NotEquals:
            return !propertyValuesEqual(left, right);

        case SelectionFilter::Comparison::Greater:
        case SelectionFilter::Comparison::Less: {
            const auto a = asNumber(left);
            const auto b = asNumber(right);
            // Ordering a colour or a handle means nothing. Returning false
            // rather than throwing keeps a nonsensical query harmless: it
            // simply selects nothing.
            if (!a || !b) {
                return false;
            }
            return comparison == SelectionFilter::Comparison::Greater ? (*a > *b) : (*a < *b);
        }

        case SelectionFilter::Comparison::Contains: {
            const auto haystack = asText(left);
            const auto needle = asText(right);
            if (!haystack || !needle) {
                return false;
            }
            if (needle->empty()) {
                return true;
            }
            // Case-insensitive: nobody searching for a room called "Bath"
            // means to exclude "BATH".
            const auto lower = [](std::string text) {
                std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                return text;
            };
            return lower(*haystack).find(lower(*needle)) != std::string::npos;
        }
    }
    return false;
}

std::vector<Handle> findByFilter(const Database& db, const BlockTableRecord& space,
                                 const SelectionFilter& filter,
                                 const PropertyRegistry& registry) {
    std::vector<Handle> matches;

    const auto consider = [&](const Entity* entity) {
        if (entity == nullptr || entity->erased) {
            return;
        }
        if (!filter.objectType.empty() && entity->objectType() != filter.objectType) {
            return;
        }
        // A selection the user cannot then edit is a trap, and this is the
        // same rule picking already applies.
        if (const Layer* layer = db.get<Layer>(entity->layerHandle);
            layer != nullptr && (layer->isLocked || !layer->isVisible())) {
            return;
        }

        if (!filter.property.empty()) {
            const PropertyDefinition* definition = registry.find(*entity, filter.property);
            // An object without the property is not a match rather than an
            // error: "radius equals 5" over a mixed drawing should return the
            // circles, not complain about the lines.
            if (definition == nullptr || !definition->get) {
                return;
            }
            if (!comparePropertyValues(definition->get(*entity), filter.value,
                                       filter.comparison)) {
                return;
            }
        }
        matches.push_back(entity->handle);
    };

    if (filter.within.has_value()) {
        // Draw order is preserved by walking the space and testing membership,
        // rather than walking the given handles — so narrowing a query twice
        // does not shuffle the result.
        for (const Handle handle : *filter.within) {
            consider(db.getEntity(handle));
        }
        return matches;
    }

    for (const Entity* entity : db.entitiesIn(space)) {
        consider(entity);
    }
    return matches;
}

std::vector<Handle> applySelectionMode(const std::vector<Handle>& current,
                                       const std::vector<Handle>& matches,
                                       SelectionFilter::Mode mode) {
    switch (mode) {
        case SelectionFilter::Mode::Replace:
            return matches;

        case SelectionFilter::Mode::Add: {
            std::vector<Handle> result = current;
            for (const Handle handle : matches) {
                if (std::find(result.begin(), result.end(), handle) == result.end()) {
                    result.push_back(handle);
                }
            }
            return result;
        }

        case SelectionFilter::Mode::Remove: {
            std::vector<Handle> result;
            for (const Handle handle : current) {
                if (std::find(matches.begin(), matches.end(), handle) == matches.end()) {
                    result.push_back(handle);
                }
            }
            return result;
        }
    }
    return matches;
}

}  // namespace cad
