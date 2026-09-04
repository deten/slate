#include "cad/agent/DrawingStandards.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

#include "cad/db/BlockTableRecord.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/DimStyle.hpp"
#include "cad/db/DimensionEntity.hpp"
#include "cad/db/HatchEntity.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/Layer.hpp"
#include "cad/db/Layout.hpp"
#include "cad/db/LeaderEntity.hpp"
#include "cad/db/Linetype.hpp"
#include "cad/db/MLeaderEntity.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/db/TextStyle.hpp"

namespace cad {
namespace {

constexpr double kDegreesPerRadian = 57.29577951308232;

/// A number the way a prompt should carry it: no exponent, no long tail of
/// zeros. A model reading `1.2000000000000002` learns nothing the shorter form
/// does not tell it, and spends more of its attention on it.
[[nodiscard]] std::string num(double value) {
    if (std::abs(value) < 5e-4) {
        value = 0.0;
    }
    std::ostringstream out;
    out.precision(3);
    out << std::fixed << value;
    std::string text = out.str();
    const std::size_t lastDigit = text.find_last_not_of('0');
    if (lastDigit != std::string::npos) {
        text.erase(text[lastDigit] == '.' ? lastDigit : lastDigit + 1);
    }
    return text;
}

[[nodiscard]] std::string point(const Vec3& p) { return num(p.x) + "," + num(p.y); }

[[nodiscard]] std::string quoted(std::string_view text) {
    constexpr std::size_t kMax = 80;
    std::string inner{text.substr(0, kMax)};
    std::replace(inner.begin(), inner.end(), '\n', ' ');
    if (text.size() > kMax) {
        inner += "...";
    }
    return "\"" + inner + "\"";
}

[[nodiscard]] double curveLength(const Entity& entity) {
    double total = 0.0;
    for (const CurvePtr& curve : entity.toCurves()) {
        if (curve != nullptr) {
            total += curve->length();
        }
    }
    return total;
}

[[nodiscard]] std::string dimensionKind(DimensionType type) {
    switch (type) {
        case DimensionType::Rotated: return "linear";
        case DimensionType::Aligned: return "aligned";
        case DimensionType::Angular2Line: return "angular";
        case DimensionType::Diameter: return "diameter";
        case DimensionType::Radius: return "radius";
        default: break;
    }
    return "dimension";
}

/// The layout spaces: model space and every sheet. Block definitions are left
/// out on purpose; a block's insides are described through the block.
[[nodiscard]] std::vector<const BlockTableRecord*> spacesOf(const Database& db) {
    std::vector<const BlockTableRecord*> spaces;
    for (const Handle handle : db.blocks().handles()) {
        if (const auto* block = db.get<BlockTableRecord>(handle);
            block != nullptr && !block->erased && block->isLayoutSpace()) {
            spaces.push_back(block);
        }
    }
    return spaces;
}

template <typename Map>
[[nodiscard]] std::string usual(const Map& votes) {
    std::string best;
    int most = -1;
    for (const auto& [value, count] : votes) {
        if (count > most) {
            best = value;
            most = count;
        }
    }
    return best;
}

template <typename Map, typename Weight>
[[nodiscard]] std::vector<typename Map::const_iterator> byWeight(const Map& map, Weight weight) {
    std::vector<typename Map::const_iterator> order;
    for (auto it = map.begin(); it != map.end(); ++it) {
        order.push_back(it);
    }
    std::stable_sort(order.begin(), order.end(), [&](const auto& a, const auto& b) {
        return weight(a->second) > weight(b->second);
    });
    return order;
}

[[nodiscard]] const char* plural(long count) { return count == 1 ? "" : "s"; }

}  // namespace

std::string drawingUnitsName(DrawingUnits units) {
    switch (static_cast<int>(units)) {
        case 1: return "inches";
        case 2: return "feet";
        case 3: return "miles";
        case 4: return "millimeters";
        case 5: return "centimeters";
        case 6: return "meters";
        case 7: return "kilometers";
        case 8: return "microinches";
        case 9: return "mils";
        case 10: return "yards";
        case 11: return "angstroms";
        case 12: return "nanometers";
        case 13: return "microns";
        case 14: return "decimeters";
        case 15: return "decameters";
        case 16: return "hectometers";
        case 17: return "gigameters";
        case 18: return "astronomical units";
        case 19: return "light years";
        case 20: return "parsecs";
        default: break;
    }
    return "unitless";
}

std::string colorNameOf(const Color& color) {
    if (color.isByLayer() || color.isByBlock()) {
        return color.toString();
    }
    if (color.toString().rfind("ACI ", 0) == 0) {
        const int index = color.index();
        switch (index) {
            case 1: return "1 (red)";
            case 2: return "2 (yellow)";
            case 3: return "3 (green)";
            case 4: return "4 (cyan)";
            case 5: return "5 (blue)";
            case 6: return "6 (magenta)";
            default: return std::to_string(index);
        }
    }
    return color.toString();
}

std::string layerNameOf(const Database& db, const Entity& entity) {
    if (const auto* layer = db.get<Layer>(entity.layerHandle); layer != nullptr) {
        return layer->name;
    }
    return "0";
}

std::string linetypeNameOf(const Database& db, Handle linetype) {
    if (const auto* record = db.get<Linetype>(linetype); record != nullptr) {
        return record->name;
    }
    return linetype.isNull() ? "Continuous" : "ByLayer";
}

std::string describeEntity(const Database& db, const Entity& entity) {
    std::ostringstream out;
    const std::string layer = " on layer " + layerNameOf(db, entity);

    if (const auto* line = dynamic_cast<const LineEntity*>(&entity)) {
        out << "Line from " << point(line->start) << " to " << point(line->end) << " (length "
            << num(line->start.distanceTo(line->end)) << ")" << layer;
    } else if (const auto* circle = dynamic_cast<const CircleEntity*>(&entity)) {
        out << "Circle centre " << point(circle->center) << " radius " << num(circle->radius)
            << layer;
    } else if (const auto* arc = dynamic_cast<const ArcEntity*>(&entity)) {
        out << "Arc centre " << point(arc->center) << " radius " << num(arc->radius) << " from "
            << num(arc->startAngle * kDegreesPerRadian) << " to "
            << num(arc->endAngle * kDegreesPerRadian) << " degrees" << layer;
    } else if (const auto* ellipse = dynamic_cast<const EllipseEntity*>(&entity)) {
        out << "Ellipse centre " << point(ellipse->center) << " major axis "
            << point(ellipse->majorAxis) << " ratio " << num(ellipse->ratio) << layer;
    } else if (const auto* polyline = dynamic_cast<const PolylineEntity*>(&entity)) {
        const Box3 bounds = polyline->bounds();
        out << (polyline->closed ? "Closed polyline" : "Polyline") << " with "
            << polyline->vertices.size() << " vertices, length " << num(curveLength(*polyline));
        if (!bounds.isEmpty()) {
            out << ", spanning " << point(bounds.min) << " to " << point(bounds.max);
        }
        out << layer;
    } else if (const auto* pointEntity = dynamic_cast<const PointEntity*>(&entity)) {
        out << "Point at " << point(pointEntity->position) << layer;
    } else if (const auto* definition = dynamic_cast<const AttributeDefinitionEntity*>(&entity)) {
        out << "Attribute definition " << definition->tag << " at " << point(definition->position)
            << layer;
    } else if (const auto* attribute = dynamic_cast<const AttributeEntity*>(&entity)) {
        out << "Attribute " << attribute->tag << " = " << quoted(attribute->contents) << layer;
    } else if (const auto* text = dynamic_cast<const TextEntity*>(&entity)) {
        out << "Text " << quoted(text->contents) << " at " << point(text->position) << " height "
            << num(text->height) << layer;
    } else if (const auto* mtext = dynamic_cast<const MTextEntity*>(&entity)) {
        out << "Multiline text " << quoted(mtext->contents) << " at " << point(mtext->position)
            << " height " << num(mtext->height) << layer;
    } else if (const auto* insert = dynamic_cast<const InsertEntity*>(&entity)) {
        const auto* block = db.get<BlockTableRecord>(insert->blockHandle);
        out << "Block " << (block != nullptr ? block->name : std::string{"?"}) << " at "
            << point(insert->position);
        if (std::abs(insert->rotation) > 1e-9) {
            out << " rotated " << num(insert->rotation * kDegreesPerRadian);
        }
        if (std::abs(insert->scale.x - 1.0) > 1e-9 || std::abs(insert->scale.y - 1.0) > 1e-9) {
            out << " scale " << num(insert->scale.x) << "," << num(insert->scale.y);
        }
        out << layer;
        bool first = true;
        for (const Handle handle : insert->attributeHandles) {
            if (const auto* value = db.get<AttributeEntity>(handle);
                value != nullptr && !value->erased) {
                out << (first ? " with " : ", ") << value->tag << "=" << quoted(value->contents);
                first = false;
            }
        }
    } else if (const auto* dimension = dynamic_cast<const DimensionEntity*>(&entity)) {
        out << "Dimension (" << dimensionKind(dimension->type) << ") reading ";
        if (!dimension->textOverride.empty()) {
            out << quoted(dimension->textOverride);
        } else {
            out << num(dimension->measure());
        }
        out << layer;
    } else if (const auto* hatch = dynamic_cast<const HatchEntity*>(&entity)) {
        const Box3 bounds = hatch->bounds();
        out << "Hatch " << hatch->pattern.name;
        if (!bounds.isEmpty()) {
            out << " spanning " << point(bounds.min) << " to " << point(bounds.max);
        }
        out << layer;
    } else if (const auto* mleader = dynamic_cast<const MLeaderEntity*>(&entity)) {
        out << "Leader with note " << quoted(mleader->text) << layer;
    } else if (const auto* leader = dynamic_cast<const LeaderEntity*>(&entity)) {
        out << "Leader";
        if (!leader->vertices.empty()) {
            out << " from " << point(leader->vertices.front()) << " to "
                << point(leader->vertices.back());
        }
        out << layer;
    } else {
        const Box3 bounds = entity.bounds();
        out << entity.objectType();
        if (!bounds.isEmpty()) {
            out << " spanning " << point(bounds.min) << " to " << point(bounds.max);
        }
        out << layer;
    }
    return out.str();
}

void DrawingStandards::learnFrom(const Database& db, std::string_view sourceName) {
    ++drawingCount;
    sources.emplace_back(sourceName);
    ++units[drawingUnitsName(db.settings.insertionUnits)];

    // Layers, as defined.
    for (const Handle handle : db.layers().handles()) {
        const auto* layer = db.get<Layer>(handle);
        if (layer == nullptr || layer->erased) {
            continue;
        }
        LayerUse& use = layers[layer->name];
        ++use.drawings;
        ++use.colors[colorNameOf(layer->color)];
        ++use.linetypes[linetypeNameOf(db, layer->linetypeHandle)];
        if (use.description.empty()) {
            use.description = layer->description;
        }
    }

    // Layers, as used, and blocks, as placed.
    std::map<Handle, long> insertsByBlock;
    for (const BlockTableRecord* space : spacesOf(db)) {
        for (const Entity* entity : db.entitiesIn(*space)) {
            if (entity == nullptr || entity->erased) {
                continue;
            }
            LayerUse& use = layers[layerNameOf(db, *entity)];
            ++use.objects;
            ++use.contents[entity->objectType()];
            if (const auto* insert = dynamic_cast<const InsertEntity*>(entity)) {
                ++insertsByBlock[insert->blockHandle];
            }
        }
    }
    for (const Handle handle : db.blocks().handles()) {
        const auto* block = db.get<BlockTableRecord>(handle);
        if (block == nullptr || block->erased || block->isLayoutSpace() || block->isAnonymous() ||
            block->isXref()) {
            continue;
        }
        BlockUse& use = blocks[block->name];
        ++use.drawings;
        use.inserts += insertsByBlock[handle];
        for (const Entity* entity : db.entitiesIn(*block)) {
            const auto* definition = dynamic_cast<const AttributeDefinitionEntity*>(entity);
            if (definition == nullptr || definition->erased) {
                continue;
            }
            if (std::find(use.attributeTags.begin(), use.attributeTags.end(), definition->tag) ==
                use.attributeTags.end()) {
                use.attributeTags.push_back(definition->tag);
            }
        }
    }

    for (const Handle handle : db.textStyles().handles()) {
        if (const auto* style = db.get<TextStyle>(handle); style != nullptr && !style->erased) {
            StyleUse& use = textStyles[style->name];
            ++use.drawings;
            if (use.detail.empty()) {
                use.detail = style->fontFile;
                if (style->height > 0.0) {
                    use.detail += ", height " + num(style->height);
                }
            }
        }
    }
    for (const Handle handle : db.dimStyles().handles()) {
        if (const auto* style = db.get<DimStyle>(handle); style != nullptr && !style->erased) {
            StyleUse& use = dimStyles[style->name];
            ++use.drawings;
            if (use.detail.empty()) {
                use.detail = "text " + num(style->textHeight) + ", arrows " + num(style->arrowSize);
            }
        }
    }

    // Each paper size once per drawing: a set with eight Letter sheets uses
    // Letter, it does not use it eight times.
    std::set<std::string> paperSizes;
    for (const Layout* layout : db.layouts()) {
        if (layout == nullptr || layout->erased) {
            continue;
        }
        const auto* block = db.get<BlockTableRecord>(layout->blockRecordHandle);
        if (block == nullptr || block->isModelSpace()) {
            continue;
        }
        std::string sheet = num(layout->plot.paperWidth) + " x " + num(layout->plot.paperHeight);
        if (!layout->plot.paperName.empty()) {
            sheet = layout->plot.paperName + " (" + sheet + ")";
        }
        paperSizes.insert(sheet);
    }
    for (const std::string& sheet : paperSizes) {
        ++sheets[sheet];
    }
}

std::string DrawingStandards::describe() const {
    if (empty()) {
        return {};
    }
    std::ostringstream out;
    out << "House standards, learned from " << drawingCount << " drawing" << plural(drawingCount)
        << ":\n";

    if (!units.empty()) {
        out << "Units:";
        bool first = true;
        for (const auto it : byWeight(units, [](int count) { return count; })) {
            out << (first ? " " : ", ") << it->first << " (" << it->second << ")";
            first = false;
        }
        out << "\n";
    }

    // Layers most used first, and only those that carry something or recur.
    // A layer one drawing defined and never drew on says nothing about how the
    // office works.
    constexpr std::size_t kMaxLayers = 60;
    const auto layerOrder =
        byWeight(layers, [](const LayerUse& use) { return use.objects + 100L * use.drawings; });
    out << "Layers (" << layers.size() << "), most used first:\n";
    std::size_t shown = 0;
    for (const auto it : layerOrder) {
        if (shown == kMaxLayers) {
            out << "  and " << (layerOrder.size() - shown) << " more\n";
            break;
        }
        const LayerUse& use = it->second;
        if (use.objects == 0 && use.drawings < 2 && drawingCount > 1) {
            continue;
        }
        ++shown;
        out << "  " << it->first << ": colour " << usual(use.colors) << ", " << usual(use.linetypes)
            << ", in " << use.drawings << " drawing" << plural(use.drawings) << ", " << use.objects
            << " object" << plural(use.objects);
        if (!use.contents.empty()) {
            out << " (";
            bool first = true;
            std::size_t kinds = 0;
            for (const auto kind : byWeight(use.contents, [](long count) { return count; })) {
                if (kinds++ == 4) {
                    out << ", ...";
                    break;
                }
                out << (first ? "" : ", ") << kind->second << " " << kind->first;
                first = false;
            }
            out << ")";
        }
        if (!use.description.empty()) {
            out << " - " << use.description;
        }
        out << "\n";
    }

    if (!blocks.empty()) {
        constexpr std::size_t kMaxBlocks = 40;
        out << "Blocks (" << blocks.size() << "), most placed first:\n";
        std::size_t count = 0;
        for (const auto it : byWeight(blocks, [](const BlockUse& use) {
                 return use.inserts + 10L * use.drawings;
             })) {
            if (count++ == kMaxBlocks) {
                out << "  and " << (blocks.size() - kMaxBlocks) << " more\n";
                break;
            }
            const BlockUse& use = it->second;
            out << "  " << it->first << ": placed " << use.inserts << " time" << plural(use.inserts)
                << " in " << use.drawings << " drawing" << plural(use.drawings);
            if (!use.attributeTags.empty()) {
                out << ", attributes";
                for (std::size_t i = 0; i < use.attributeTags.size(); ++i) {
                    out << (i == 0 ? " " : ", ") << use.attributeTags[i];
                }
            }
            out << "\n";
        }
    }

    const auto styles = [&](const char* title, const std::map<std::string, StyleUse>& map) {
        if (map.empty()) {
            return;
        }
        out << title << ":";
        bool first = true;
        std::size_t count = 0;
        for (const auto it : byWeight(map, [](const StyleUse& use) { return use.drawings; })) {
            if (count++ == 12) {
                out << ", ...";
                break;
            }
            out << (first ? " " : ", ") << it->first << " (" << it->second.detail << ")";
            first = false;
        }
        out << "\n";
    };
    styles("Text styles", textStyles);
    styles("Dimension styles", dimStyles);

    if (!sheets.empty()) {
        out << "Sheets:";
        bool first = true;
        for (const auto it : byWeight(sheets, [](int count) { return count; })) {
            out << (first ? " " : ", ") << it->first << " in " << it->second << " drawing"
                << plural(it->second);
            first = false;
        }
        out << "\n";
    }
    return out.str();
}

}  // namespace cad
