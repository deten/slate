#include "cad/agent/DrawingReading.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

#include "cad/agent/DrawingStandards.hpp"
#include "cad/db/BlockGeometry.hpp"
#include "cad/db/BlockTableRecord.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/DimensionEntity.hpp"
#include "cad/db/HatchEntity.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/Layer.hpp"
#include "cad/db/Layout.hpp"
#include "cad/db/LeaderEntity.hpp"
#include "cad/db/MLeaderEntity.hpp"
#include "cad/db/RasterImageEntity.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/db/UnderlayEntity.hpp"
#include "cad/db/ViewportEntity.hpp"
#include "cad/geom/BoundaryTrace.hpp"
#include "cad/math/Mat4.hpp"
#include "cad/project/Project.hpp"

namespace cad {
namespace {

[[nodiscard]] std::string upper(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

[[nodiscard]] std::string trimmed(std::string_view text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(begin, end - begin + 1));
}

[[nodiscard]] bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

/// Layer names are built from tokens: M-DUCT-SUPP, HSDUCT, mech-exist.
[[nodiscard]] std::vector<std::string> tokensOf(std::string_view name) {
    std::vector<std::string> tokens;
    std::string current;
    for (const char c : upper(name)) {
        if (c == '-' || c == '_' || c == ' ' || c == '.') {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

[[nodiscard]] bool hasToken(const std::vector<std::string>& tokens, std::string_view token) {
    return std::find(tokens.begin(), tokens.end(), token) != tokens.end();
}

[[nodiscard]] std::string num(double value) {
    std::ostringstream out;
    if (std::abs(value) >= 100.0) {
        out << std::fixed << std::setprecision(0) << value;
    } else if (std::abs(value) >= 10.0) {
        out << std::fixed << std::setprecision(1) << value;
    } else {
        out << std::fixed << std::setprecision(2) << value;
    }
    std::string text = out.str();
    if (text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0') {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '.') {
            text.pop_back();
        }
    }
    return text == "-0" ? "0" : text;
}

[[nodiscard]] std::string point(const Vec3& p) { return num(p.x) + "," + num(p.y); }

[[nodiscard]] bool insidePolygon(const std::vector<Vec3>& polygon, const Vec3& p) {
    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const Vec3& a = polygon[i];
        const Vec3& b = polygon[j];
        if ((a.y > p.y) != (b.y > p.y)) {
            const double x = (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x;
            if (p.x < x) {
                inside = !inside;
            }
        }
    }
    return inside;
}

[[nodiscard]] double polygonArea(const std::vector<Vec3>& polygon) {
    double twice = 0.0;
    for (std::size_t i = 0, n = polygon.size(); i < n; ++i) {
        const Vec3& a = polygon[i];
        const Vec3& b = polygon[(i + 1) % n];
        twice += a.x * b.y - b.x * a.y;
    }
    return std::abs(twice) / 2.0;
}

/// A label as it will be reasoned about: the text, where it is, how big.
struct Label {
    Handle handle;
    std::string text;
    Box3 bounds;
    double height = 0.0;
    bool used = false;
    std::string layer;
};

[[nodiscard]] bool isNumberLike(std::string_view text) {
    // "101", "101A", "B-12": short, starts with a digit or is mostly digits.
    if (text.empty() || text.size() > 6) {
        return false;
    }
    int digits = 0;
    for (const char c : text) {
        if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
            ++digits;
        } else if (std::isalpha(static_cast<unsigned char>(c)) == 0 && c != '-' && c != '.') {
            return false;
        }
    }
    return digits > 0 && digits * 2 >= static_cast<int>(text.size());
}

[[nodiscard]] double widthOf(const PolylineEntity& polyline) {
    double width = polyline.constantWidth;
    for (const PolylineVertex& vertex : polyline.vertices) {
        width = std::max({width, vertex.startWidth, vertex.endWidth});
    }
    return width;
}

[[nodiscard]] double curveLength(const Entity& entity) {
    double length = 0.0;
    for (const CurvePtr& curve : entity.toCurves()) {
        if (curve != nullptr) {
            length += curve->length();
        }
    }
    return length;
}

/// Which layers hold the building shell, from their names.
enum class ShellPart { None, Wall, Door, Window };

[[nodiscard]] ShellPart shellPartOf(std::string_view layerName) {
    const std::vector<std::string> tokens = tokensOf(layerName);
    const std::string name = upper(layerName);
    if (hasToken(tokens, "WALL") || hasToken(tokens, "WALLS") || contains(name, "WALL")) {
        return ShellPart::Wall;
    }
    if (hasToken(tokens, "DOOR") || hasToken(tokens, "DOORS") || hasToken(tokens, "DR")) {
        return ShellPart::Door;
    }
    if (hasToken(tokens, "GLAZ") || hasToken(tokens, "WIND") || hasToken(tokens, "WINDOW") ||
        hasToken(tokens, "WINDOWS")) {
        return ShellPart::Window;
    }
    return ShellPart::None;
}

}  // namespace

std::string shellPartNameOf(std::string_view layerName) {
    switch (shellPartOf(layerName)) {
        case ShellPart::Wall:
            return "wall";
        case ShellPart::Door:
            return "door";
        case ShellPart::Window:
            return "window";
        case ShellPart::None:
            break;
    }
    return {};
}

double SystemReading::totalRunLength() const {
    double total = 0.0;
    for (const RunReading& run : runs) {
        total += run.length;
    }
    return total;
}

std::string systemKindOf(std::string_view layerName) {
    const std::vector<std::string> tokens = tokensOf(layerName);
    const std::string name = upper(layerName);
    const auto any = [&](std::initializer_list<const char*> pieces) {
        for (const char* piece : pieces) {
            if (contains(name, piece)) {
                return true;
            }
        }
        return false;
    };

    if (any({"DUCT", "HVAC"})) {
        // HSDUCT / HRDUCT / HEDUCT are one office's supply, return and
        // exhaust; M-DUCT-SUPP and M-DUCT-RETN are the layer standard's.
        if (any({"SUP", "SDUCT", "-SA"}) || name.rfind("HS", 0) == 0 || name.rfind("MS", 0) == 0) {
            return "supply duct";
        }
        if (any({"RET", "RDUCT", "-RA"}) || name.rfind("HR", 0) == 0 || name.rfind("MR", 0) == 0) {
            return "return duct";
        }
        if (any({"EXH", "EDUCT", "-EA"}) || name.rfind("HE", 0) == 0) {
            return "exhaust duct";
        }
        return "duct";
    }
    if (any({"FITT"}) || hasToken(tokens, "FITTING") || hasToken(tokens, "FITTINGS")) {
        return "fittings";
    }
    if (any({"DIFF", "GRIL", "GRILLE", "REGIS", "TERM"})) {
        return "air terminals";
    }
    if (any({"PIPE", "PIPING", "CHW", "HHW", "CWS", "CWR", "HWS", "HWR", "REFR", "COND",
             "PLUMB"}) ||
        hasToken(tokens, "CW") || hasToken(tokens, "HW")) {
        return "pipe";
    }
    if (any({"EQPM", "EQUIP", "EQT", "MEQ"})) {
        return "equipment";
    }
    return {};
}

std::string systemStatusOf(std::string_view layerName) {
    const std::vector<std::string> tokens = tokensOf(layerName);
    const std::string name = upper(layerName);
    if (tokens.empty()) {
        return {};
    }
    const std::string& last = tokens.back();
    if (last == "D" || last == "DEMO" || contains(name, "DEMO")) {
        return "demolition";
    }
    if (last == "E" || last == "EXIST" || last == "OLD" || last == "EX" || contains(name, "EXIST")) {
        return "existing";
    }
    if (last == "N" || last == "NEW") {
        return "new";
    }
    return {};
}

bool looksLikeRoomName(std::string_view raw) {
    const std::string text = trimmed(raw);
    if (text.size() < 2 || text.size() > 40) {
        return false;
    }
    int letters = 0;
    int digits = 0;
    bool afterHash = false;
    for (const char c : text) {
        const auto uc = static_cast<unsigned char>(c);
        if (std::isalpha(uc) != 0) {
            if (std::islower(uc) != 0) {
                // Room labels are set in capitals; a sentence is a note.
                return false;
            }
            ++letters;
        } else if (std::isdigit(uc) != 0) {
            ++digits;
            if (!afterHash) {
                // "CONF. #1" and "AREA #2" number a room in its name; "AHU-1"
                // and "24x12" are tags and sizes.
                return false;
            }
        } else if (c == '#') {
            afterHash = true;
        } else if (c == ' ' || c == '.' || c == '&' || c == '/' || c == '\'' || c == '-') {
            afterHash = false;
        } else {
            return false;
        }
    }
    if (letters < 2) {
        return false;
    }
    // Units and drafting abbreviations that are set in capitals too.
    static const char* const kNotRooms[] = {
        "CFM", "GPM", "BTU", "MBH", "TYP", "NTS", "MIN", "MAX", "DN", "UP", "SF", "SQ", "FT",
        "IN", "EA", "HP", "KW", "VAV", "AHU", "RTU", "EF", "SF-", "FCU", "UH", "EXH", "SA", "RA",
        "OA", "EA", "REF", "SIM", "OC", "O.C.", "AFF", "A.F.F.", "CLG", "FLR", "BOT", "TOP",
        "NIC", "N.I.C.", "VIF", "V.I.F.", "N", "S", "E", "W", "NORTH", "SCALE", "NOTE",
        "NOTES", "LEGEND", "PLAN", "KEY", "DETAIL", "SECTION", "ELEVATION", "SEE", "PER",
        "AND", "OR", "TO", "BY", "AT", "OF", "THE", "NO", "NEW", "EXIST", "EXISTING", "DEMO",
        "REMOVE", "RELOCATE", "MATCH", "LINE", "DIA", "THRU", "W/", "WITH", "ABOVE", "BELOW",
    };
    const std::string bare = upper(text);
    for (const char* word : kNotRooms) {
        if (bare == word) {
            return false;
        }
    }
    return true;
}

bool looksLikeZoneName(std::string_view raw, std::string* name) {
    const std::string text = upper(trimmed(raw));
    if (text.size() < 2 || text.size() > 16) {
        return false;
    }
    // The word, then the number: "ZONE 3", "ZONE-3", "ZONE:3", "Z-3", "Z3",
    // "VAV-2", "VAV 2". A bare word or a bare number is neither.
    static const char* const kWords[] = {"ZONE", "VAV", "Z"};
    for (const char* word : kWords) {
        const std::string prefix{word};
        if (text.rfind(prefix, 0) != 0) {
            continue;
        }
        std::string rest = text.substr(prefix.size());
        while (!rest.empty() && (rest.front() == ' ' || rest.front() == '-' || rest.front() == ':' ||
                                 rest.front() == '#' || rest.front() == '_')) {
            rest.erase(rest.begin());
        }
        if (rest.empty() || rest.size() > 6) {
            continue;
        }
        bool digits = false;
        for (const char c : rest) {
            if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
                digits = true;
            } else if (std::isalpha(static_cast<unsigned char>(c)) == 0) {
                return false;
            }
        }
        if (!digits) {
            continue;
        }
        if (name != nullptr) {
            *name = prefix == "Z" ? "ZONE " + rest : prefix == "ZONE" ? "ZONE " + rest : prefix + "-" + rest;
        }
        return true;
    }
    return false;
}

std::vector<SpotReading> candidateSpots(const Database& db, const DrawingReading& reading,
                                        const RoomReading& room, std::size_t count) {
    std::vector<SpotReading> spots;
    if (!room.bounded || room.boundaryHandles.empty()) {
        return spots;
    }
    // The room's walls as curves, doors and windows set aside: a device
    // goes on a wall, near the door, not in the opening.
    struct Wall {
        CurvePtr curve;
        bool opening;
    };
    std::vector<Wall> walls;
    std::vector<Vec3> doors;
    for (const Handle handle : room.boundaryHandles) {
        const Entity* entity = db.getEntity(handle);
        if (entity == nullptr || entity->erased) {
            continue;
        }
        const std::string part = shellPartNameOf(layerNameOf(db, *entity));
        const bool opening = part == "door" || part == "window";
        std::vector<CurvePtr> curves;
        if (const auto* insert = dynamic_cast<const InsertEntity*>(entity)) {
            curves = blockReferenceCurves(db, *insert);
            if (part == "door") {
                doors.push_back(insert->position);
            }
        } else {
            curves = entity->toCurves();
        }
        for (CurvePtr& curve : curves) {
            if (curve != nullptr) {
                walls.push_back(Wall{std::move(curve), opening});
            }
        }
    }
    if (walls.empty()) {
        return spots;
    }
    // Air terminals and other symbols already in the room are kept clear of.
    std::vector<Vec3> avoid;
    const Box3 inside = room.bounds.expanded(room.bounds.diagonal() * 0.01);
    for (const Entity* entity : db.entitiesIn(const_cast<Database&>(db).modelSpace())) {
        const auto* insert = dynamic_cast<const InsertEntity*>(entity);
        if (insert == nullptr || insert->erased || !inside.containsPoint(insert->position)) {
            continue;
        }
        const std::string kind = systemKindOf(layerNameOf(db, *entity));
        if (kind == "air terminals" || kind == "supply duct" || kind == "return duct" ||
            kind == "duct") {
            avoid.push_back(insert->position);
        }
    }
    for (const SymbolReading& symbol : reading.symbols) {
        if (inside.containsPoint(symbol.position)) {
            avoid.push_back(symbol.position);
        }
    }

    // Walk each wall at steps of a twentieth of the room's size; score each
    // point by distance to the nearest door, the wall being interior, and
    // clearance from openings and what is to be avoided.
    const double step = std::max(room.bounds.diagonal() / 20.0, 1e-6);
    const double edge = room.bounds.diagonal() * 0.02;
    const Vec3 centre = room.bounds.center();
    for (const Wall& wall : walls) {
        if (wall.opening) {
            continue;
        }
        const double length = wall.curve->length();
        const int steps = std::max(1, static_cast<int>(length / step));
        for (int i = 1; i < steps; ++i) {
            const double t = static_cast<double>(i) / steps;
            const Vec3 p = wall.curve->pointAt(t);
            // Interior walls lie inside the room's box; a point on the box's
            // own edge is an outside wall, which is where the windows are.
            const bool outside = std::abs(p.x - room.bounds.min.x) < edge ||
                                 std::abs(p.x - room.bounds.max.x) < edge ||
                                 std::abs(p.y - room.bounds.min.y) < edge ||
                                 std::abs(p.y - room.bounds.max.y) < edge;
            double nearestOpening = std::numeric_limits<double>::infinity();
            for (const Wall& other : walls) {
                if (other.opening) {
                    nearestOpening = std::min(nearestOpening, other.curve->distanceTo(p));
                }
            }
            double nearestDoor = std::numeric_limits<double>::infinity();
            for (const Vec3& door : doors) {
                nearestDoor = std::min(nearestDoor, door.distanceTo(p));
            }
            double nearestAvoid = std::numeric_limits<double>::infinity();
            for (const Vec3& a : avoid) {
                nearestAvoid = std::min(nearestAvoid, a.distanceTo(p));
            }
            if (nearestOpening < step) {
                continue;
            }
            SpotReading spot;
            spot.position = p;
            spot.along = wall.curve->tangentAt(t);
            spot.cost = (std::isfinite(nearestDoor) ? nearestDoor : room.bounds.diagonal()) +
                        (outside ? room.bounds.diagonal() : 0.0) +
                        (nearestAvoid < 2.0 * step ? room.bounds.diagonal() : 0.0);
            // Which wall, by where it sits relative to the room's centre.
            const Vec3 d = p - centre;
            const char* side = std::abs(d.x) > std::abs(d.y) ? (d.x > 0 ? "east" : "west")
                                                             : (d.y > 0 ? "north" : "south");
            spot.reason = std::string(outside ? "outside " : "inside ") + side + " wall";
            if (std::isfinite(nearestDoor)) {
                spot.reason += ", " + num(nearestDoor) + " from the door";
            }
            if (std::isfinite(nearestAvoid)) {
                spot.reason += ", " + num(nearestAvoid) + " from the nearest terminal or symbol";
            }
            spots.push_back(std::move(spot));
        }
    }
    std::stable_sort(spots.begin(), spots.end(),
                     [](const SpotReading& a, const SpotReading& b) { return a.cost < b.cost; });
    // Spread out: no two offered spots within a step of each other.
    std::vector<SpotReading> chosen;
    for (const SpotReading& spot : spots) {
        bool near = false;
        for (const SpotReading& taken : chosen) {
            if (taken.position.distanceTo(spot.position) < 2.0 * step) {
                near = true;
                break;
            }
        }
        if (!near) {
            chosen.push_back(spot);
        }
        if (chosen.size() == count) {
            break;
        }
    }
    return chosen;
}

DrawingReading readDrawing(const Database& db, const DrawingReadingOptions& options) {
    const auto started = std::chrono::steady_clock::now();
    const auto outOfTime = [&] {
        return std::chrono::steady_clock::now() - started > options.budget;
    };

    DrawingReading reading;
    const std::string units = drawingUnitsName(db.settings.insertionUnits);
    if (units == "inches") {
        reading.areaUnit = "sq ft";
        reading.areaDivisor = 144.0;
    } else if (units == "feet") {
        reading.areaUnit = "sq ft";
    } else if (units == "millimeters") {
        reading.areaUnit = "sq m";
        reading.areaDivisor = 1.0e6;
    } else if (units == "centimeters") {
        reading.areaUnit = "sq m";
        reading.areaDivisor = 1.0e4;
    } else if (units == "meters") {
        reading.areaUnit = "sq m";
    } else {
        reading.areaUnit = "sq units";
    }
    // Smaller than a closet is not a room: the box around an equipment tag,
    // a door swing that closed on itself.
    const double smallestRoom =
        reading.areaUnit == "sq ft" ? 25.0 : reading.areaUnit == "sq m" ? 2.5 : 0.0;

    const auto& model = const_cast<Database&>(db).modelSpace();
    const std::vector<const Entity*> entities = db.entitiesIn(model);

    // ---- Labels, systems and the shell in one pass over model space.
    std::vector<Label> labels;
    std::map<std::string, int> labelUses;
    std::map<std::string, SystemReading> systems;
    struct Circle {
        Handle handle;
        Vec3 center;
        double radius;
        std::string layer;
    };
    std::vector<Circle> circles;
    // Closed outlines on zone layers, and the inserts named for a device,
    // both kept for after the rooms are known.
    struct Outline {
        Handle handle;
        std::vector<Vec3> polygon;
    };
    std::vector<Outline> zoneOutlines;
    std::vector<SymbolReading> namedSymbols;
    std::vector<CurvePtr> boundary;
    std::vector<Handle> boundaryOwners;
    bool tooManyCurves = false;

    // Layer names, to tell a legend entry from a label: the text "MDUCT-D"
    // beside a sample of the linework on layer MDUCT-D is a key, not a room.
    std::vector<std::string> layerNames;
    for (const Handle handle : db.layers().handles()) {
        if (const auto* layer = db.get<Layer>(handle); layer != nullptr && !layer->erased) {
            layerNames.push_back(upper(layer->name));
        }
    }

    // What is in model space, and what a resolved external reference brings
    // into it. A mechanical plan nearly always references the architecture,
    // so the walls, doors and room names are usually in the reference and
    // the ducts in the host; read as one drawing, placed where the reference
    // is placed. Layers of a reference are named "REF|LAYER"; the reference's
    // part is dropped so A-WALL is a wall wherever it came from.
    struct Placed {
        const Entity* entity;
        Mat4 transform;
        bool moved;
    };
    std::vector<Placed> placed;
    placed.reserve(entities.size());
    for (const Entity* entity : entities) {
        if (entity == nullptr || entity->erased) {
            continue;
        }
        placed.push_back(Placed{entity, Mat4::identity(), false});
        const auto* insert = dynamic_cast<const InsertEntity*>(entity);
        if (insert == nullptr) {
            continue;
        }
        const auto* block = db.get<BlockTableRecord>(insert->blockHandle);
        if (block == nullptr || !block->isXref()) {
            continue;
        }
        ReferenceReading reference;
        reference.name = block->name;
        reference.path = block->xrefPath;
        reference.resolved = block->xrefStatus == XrefStatus::Resolved;
        reference.position = insert->position;
        reference.scale = insert->scale.x;
        reference.rotation = insert->rotation;
        reading.references.push_back(std::move(reference));
        if (!reference.resolved) {
            continue;
        }
        const Mat4 transform = insert->blockTransform();
        for (const Entity* inner : db.entitiesIn(*block)) {
            if (inner != nullptr && !inner->erased) {
                placed.push_back(Placed{inner, transform, true});
            }
        }
    }
    const auto ownLayerName = [&](const Entity& entity) {
        std::string name = layerNameOf(db, entity);
        if (const auto bar = name.rfind('|'); bar != std::string::npos) {
            name.erase(0, bar + 1);
        }
        return name;
    };

    // Frozen and switched-off layers are read too. A mechanical sheet keeps
    // the architecture frozen while it is worked on, and the rooms are still
    // where the walls say they are.
    for (const Placed& item : placed) {
        const Entity* entity = item.entity;
        const Box3 bounds = item.moved ? entity->bounds().transformed(item.transform)
                                       : entity->bounds();
        const std::string layerName = ownLayerName(*entity);
        const std::string kind = systemKindOf(layerName);
        const ShellPart shell = shellPartOf(layerName);

        std::string text;
        if (const auto* single = dynamic_cast<const TextEntity*>(entity)) {
            if (dynamic_cast<const AttributeDefinitionEntity*>(entity) == nullptr &&
                dynamic_cast<const AttributeEntity*>(entity) == nullptr) {
                text = decodeTextSymbols(single->contents);
            }
        } else if (const auto* multi = dynamic_cast<const MTextEntity*>(entity)) {
            const std::vector<std::string> lines = multi->lines();
            text = lines.empty() ? std::string{} : lines.front();
        }
        if (!text.empty()) {
            text = trimmed(text);
            if (std::find(layerNames.begin(), layerNames.end(), upper(text)) != layerNames.end()) {
                continue;
            }
            double height = 0.0;
            if (const auto* single = dynamic_cast<const TextEntity*>(entity)) {
                height = single->height;
            } else if (const auto* multi = dynamic_cast<const MTextEntity*>(entity)) {
                height = multi->height;
            }
            labels.push_back(Label{entity->handle, text, bounds, height, false, layerName});
            ++labelUses[text];
            if (!kind.empty()) {
                ++systems[layerName].labels[text];
            }
            continue;
        }

        if (const auto* polyline = dynamic_cast<const PolylineEntity*>(entity)) {
            // A closed outline on a zone layer marks a zone.
            if (polyline->closed && upper(layerName).find("ZONE") != std::string::npos &&
                polyline->vertices.size() >= 3) {
                Outline outline;
                outline.handle = entity->handle;
                for (const PolylineVertex& vertex : polyline->vertices) {
                    outline.polygon.push_back(item.transform.transformPoint(vertex.position));
                }
                zoneOutlines.push_back(std::move(outline));
            }
            const double width = widthOf(*polyline);
            if (width > 0.0) {
                SystemReading& system = systems[layerName];
                RunReading run;
                run.handle = entity->handle;
                run.width = width;
                run.length = curveLength(*entity);
                if (!polyline->vertices.empty()) {
                    run.start = item.transform.transformPoint(polyline->vertices.front().position);
                    run.end = item.transform.transformPoint(polyline->vertices.back().position);
                }
                system.runs.push_back(run);
                // A run is a thing in a room, not a wall of it.
                continue;
            }
        }

        if (const auto* insert = dynamic_cast<const InsertEntity*>(entity)) {
            const auto* block = db.get<BlockTableRecord>(insert->blockHandle);
            const std::string blockName = block == nullptr ? std::string{"?"} : block->name;
            if (!kind.empty()) {
                ++systems[layerName].blocks[blockName];
            }
            // A block named for a device is a symbol wherever it sits.
            {
                const std::string name = upper(blockName);
                static const char* const kDevices[] = {"TSTAT", "T-STAT", "THERMO", "SENSOR",
                                                       "SWITCH", "STAT", "HUMID", "CO2"};
                for (const char* device : kDevices) {
                    if (name.find(device) != std::string::npos) {
                        SymbolReading symbol;
                        symbol.tag = blockName;
                        symbol.position = item.transform.transformPoint(insert->position);
                        symbol.layer = layerName;
                        symbol.kind = "block";
                        symbol.handle = entity->handle;
                        namedSymbols.push_back(std::move(symbol));
                        break;
                    }
                }
            }
            if (shell == ShellPart::Door) {
                ++reading.architecture.doorBlocks[blockName];
                ++reading.architecture.doors;
            } else if (shell == ShellPart::Window) {
                ++reading.architecture.windowBlocks[blockName];
                ++reading.architecture.windows;
            }
        } else if (dynamic_cast<const LineEntity*>(entity) != nullptr) {
            if (!kind.empty()) {
                ++systems[layerName].lines;
            }
            if (shell == ShellPart::Wall) {
                ++reading.architecture.wallLayers[layerName];
            }
        } else if (const auto* circle = dynamic_cast<const CircleEntity*>(entity)) {
            if (!kind.empty()) {
                ++systems[layerName].circles;
            }
            // Small circles are what letters sit in to make a symbol.
            circles.push_back(Circle{entity->handle,
                                     item.transform.transformPoint(circle->center),
                                     circle->radius * (item.moved ? item.transform.transformVector(Vec3::unitX()).length() : 1.0),
                                     layerName});
        }

        // What bounds a room is what is drawn: not fills, notes, dimensions,
        // pictures or the windows of a sheet.
        if (dynamic_cast<const HatchEntity*>(entity) != nullptr ||
            dynamic_cast<const DimensionEntity*>(entity) != nullptr ||
            dynamic_cast<const LeaderEntity*>(entity) != nullptr ||
            dynamic_cast<const MLeaderEntity*>(entity) != nullptr ||
            dynamic_cast<const RasterImageEntity*>(entity) != nullptr ||
            dynamic_cast<const UnderlayEntity*>(entity) != nullptr ||
            dynamic_cast<const ViewportEntity*>(entity) != nullptr ||
            dynamic_cast<const PointEntity*>(entity) != nullptr) {
            continue;
        }
        if (tooManyCurves) {
            continue;
        }
        std::vector<CurvePtr> curves;
        if (const auto* insert = dynamic_cast<const InsertEntity*>(entity)) {
            // Doors and windows sit in the gaps of walls; their leaf and swing
            // are what close a room. A resolved reference's contents are
            // placed one by one above, so the reference itself adds nothing,
            // or every wall in it would be there twice.
            const auto* block = db.get<BlockTableRecord>(insert->blockHandle);
            if (block != nullptr && block->isXref()) {
                continue;
            }
            curves = blockReferenceCurves(db, *insert);
        } else {
            curves = entity->toCurves();
        }
        for (CurvePtr& curve : curves) {
            if (curve == nullptr) {
                continue;
            }
            if (item.moved) {
                curve = curve->transformed(item.transform);
                if (curve == nullptr) {
                    continue;
                }
            }
            boundary.push_back(std::move(curve));
            boundaryOwners.push_back(entity->handle);
        }
        if (boundary.size() > options.maxBoundaryCurves) {
            tooManyCurves = true;
        }
    }

    // ---- Rooms: names, then the number that goes with each, then walls.
    // Stacked lines of one name ("TOOL" / "STOR." / "& JAN.") are joined: a
    // line directly under another, about a line-height apart, is its
    // continuation.
    std::vector<std::size_t> names;
    for (std::size_t i = 0; i < labels.size(); ++i) {
        if (looksLikeRoomName(labels[i].text)) {
            names.push_back(i);
        }
    }
    // Top to bottom, so a continuation is met after its first line.
    std::sort(names.begin(), names.end(), [&](std::size_t a, std::size_t b) {
        return labels[a].bounds.max.y > labels[b].bounds.max.y;
    });
    const auto continuation = [&](const Label& above, const Label& below) {
        if (above.height <= 0.0 || below.height <= 0.0) {
            return false;
        }
        const double gap = above.bounds.min.y - below.bounds.max.y;
        if (gap < -0.2 * above.height || gap > 1.2 * above.height) {
            return false;
        }
        const double overlap = std::min(above.bounds.max.x, below.bounds.max.x) -
                               std::max(above.bounds.min.x, below.bounds.min.x);
        return overlap > 0.3 * std::min(above.bounds.width(), below.bounds.width());
    };

    std::vector<RoomReading> rooms;
    for (const std::size_t index : names) {
        Label& first = labels[index];
        if (first.used) {
            continue;
        }
        RoomReading room;
        room.name = first.text;
        room.label = first.bounds.center();
        room.labelHandles.push_back(first.handle);
        first.used = true;
        Box3 stack = first.bounds;
        const Label* last = &first;
        for (bool grew = true; grew;) {
            grew = false;
            for (const std::size_t other : names) {
                Label& candidate = labels[other];
                if (!candidate.used && continuation(*last, candidate)) {
                    room.name += " " + candidate.text;
                    room.labelHandles.push_back(candidate.handle);
                    candidate.used = true;
                    stack = stack.united(candidate.bounds);
                    last = &candidate;
                    grew = true;
                    break;
                }
            }
        }
        // The seed for the walls is the name itself, not the stack with the
        // number: the number often sits in a bubble, and a seed inside the
        // bubble would trace the bubble.
        room.label = first.bounds.center();

        // The number: the nearest short number-like label within a few line
        // heights, usually directly below.
        const double reach = 4.0 * std::max(first.height, 1e-9);
        std::size_t best = labels.size();
        double bestDistance = reach;
        for (std::size_t i = 0; i < labels.size(); ++i) {
            const Label& candidate = labels[i];
            if (candidate.used || !isNumberLike(candidate.text)) {
                continue;
            }
            const Vec3 at = candidate.bounds.center();
            const double dx = std::max({stack.min.x - at.x, at.x - stack.max.x, 0.0});
            const double dy = std::max({stack.min.y - at.y, at.y - stack.max.y, 0.0});
            const double distance = std::hypot(dx, dy);
            if (distance < bestDistance) {
                bestDistance = distance;
                best = i;
            }
        }
        if (best < labels.size()) {
            room.number = labels[best].text;
            room.labelHandles.push_back(labels[best].handle);
            labels[best].used = true;
        }
        rooms.push_back(std::move(room));
    }

    // A name repeated many times over is a tag or a legend, not that many
    // rooms -- unless it turns out to have walls of its own, below.
    const auto repeated = [&](const RoomReading& room) {
        return room.number.empty() && labelUses[room.name] >= 5;
    };

    reading.boundariesTraced = !tooManyCurves && !boundary.empty();
    if (reading.boundariesTraced) {
        std::vector<const Curve*> curves;
        curves.reserve(boundary.size());
        for (const CurvePtr& curve : boundary) {
            curves.push_back(curve.get());
        }
        std::size_t traced = 0;
        for (RoomReading& room : rooms) {
            if (traced >= options.maxRooms || outOfTime()) {
                reading.boundariesTraced = false;
                break;
            }
            ++traced;
            // A label often sits in a bubble or a box of its own; a seed
            // inside one traces the bubble. When the loop is smaller than
            // a closet, seed again just outside it, once.
            Vec3 seed = room.label;
            std::vector<Vec3> polygon;
            double area = 0.0;
            const BoundaryLoop* outer = nullptr;
            BoundaryTraceResult found;
            for (int attempt = 0; attempt < 2; ++attempt) {
                found = traceBoundary(curves, seed);
                if (!found.ok()) {
                    break;
                }
                outer = &found.loops.front();
                polygon.clear();
                for (const CurvePtr& curve : outer->curves) {
                    for (const Vec3& p : curve->tessellate()) {
                        if (polygon.empty() || polygon.back().distanceTo(p) > 1e-9) {
                            polygon.push_back(p);
                        }
                    }
                }
                area = polygonArea(polygon);
                if (area <= 0.0 || area / reading.areaDivisor >= smallestRoom) {
                    break;
                }
                const Box3 bubble = Box3::fromPoints(polygon);
                seed = Vec3{bubble.max.x + bubble.width() * 0.25, seed.y, seed.z};
                outer = nullptr;
            }
            if (!found.ok() || outer == nullptr) {
                continue;
            }
            // A loop reached through a gap in the walls is the outside, or
            // the whole floor: it has other rooms' labels inside it. The
            // room is open, not enormous. One smaller than a closet is the
            // box drawn around an equipment tag.
            if (area <= 0.0 || area / reading.areaDivisor < smallestRoom) {
                continue;
            }
            bool swallowsAnother = false;
            for (const RoomReading& other : rooms) {
                if (&other != &room && insidePolygon(polygon, other.label)) {
                    swallowsAnother = true;
                    break;
                }
            }
            if (swallowsAnother) {
                continue;
            }
            room.bounded = true;
            room.area = area;
            room.bounds = Box3::fromPoints(polygon);
            for (const std::size_t source : outer->sources) {
                if (source < boundaryOwners.size()) {
                    const Handle owner = boundaryOwners[source];
                    if (std::find(room.boundaryHandles.begin(), room.boundaryHandles.end(),
                                  owner) == room.boundaryHandles.end()) {
                        room.boundaryHandles.push_back(owner);
                    }
                }
            }
        }
    }

    for (RoomReading& room : rooms) {
        if (room.bounded || !repeated(room)) {
            // A short name with no number is a tag ("RF", "OSA") unless it
            // has walls around it and room enough to be a room.
            const bool named = !room.number.empty() || room.name.size() >= 4;
            const bool roomy = room.bounded && room.area / reading.areaDivisor >= 4.0 * smallestRoom;
            if (named || roomy) {
                reading.rooms.push_back(std::move(room));
            }
        }
    }

    // ---- Symbols: a letter or two in a circle no bigger than a couple of
    // text heights, the letter not already a room's number. And the blocks
    // named for a device, found above.
    reading.symbols = std::move(namedSymbols);
    for (const Circle& circle : circles) {
        for (Label& label : labels) {
            if (label.used || label.text.size() > 2 || label.height <= 0.0) {
                continue;
            }
            if (circle.radius > 2.5 * label.height || circle.radius < 0.3 * label.height) {
                continue;
            }
            bool letters = true;
            for (const char c : label.text) {
                if (std::isalpha(static_cast<unsigned char>(c)) == 0) {
                    letters = false;
                }
            }
            if (!letters) {
                continue;
            }
            if (label.bounds.center().distanceTo(circle.center) > circle.radius) {
                continue;
            }
            SymbolReading symbol;
            symbol.tag = upper(label.text);
            symbol.position = circle.center;
            symbol.layer = circle.layer;
            symbol.kind = "circle";
            symbol.handle = circle.handle;
            reading.symbols.push_back(std::move(symbol));
            label.used = true;
            break;
        }
    }

    // ---- Zones: a tag, and the outline that holds it when there is one --
    // a closed polyline on a zone layer, else the walls around the tag.
    // What lies inside is what a zone is for: its rooms, and whether it
    // has its thermostat.
    for (Label& label : labels) {
        std::string name;
        if (label.used || !looksLikeZoneName(label.text, &name)) {
            continue;
        }
        ZoneReading zone;
        zone.name = name;
        zone.label = label.bounds.center();
        zone.labelHandles.push_back(label.handle);
        label.used = true;
        for (const Outline& outline : zoneOutlines) {
            if (insidePolygon(outline.polygon, zone.label)) {
                zone.outline = outline.polygon;
                break;
            }
        }
        if (zone.outline.empty() && reading.boundariesTraced) {
            // The walls around the tag, as for a room; a tag in a corridor
            // that opens onto the whole floor gets no outline.
            std::vector<const Curve*> curves;
            curves.reserve(boundary.size());
            for (const CurvePtr& curve : boundary) {
                curves.push_back(curve.get());
            }
            if (!curves.empty() && !outOfTime()) {
                const BoundaryTraceResult found = traceBoundary(curves, zone.label);
                if (found.ok()) {
                    for (const CurvePtr& curve : found.loops.front().curves) {
                        for (const Vec3& p : curve->tessellate()) {
                            if (zone.outline.empty() || zone.outline.back().distanceTo(p) > 1e-9) {
                                zone.outline.push_back(p);
                            }
                        }
                    }
                }
            }
        }
        if (!zone.outline.empty()) {
            zone.bounded = true;
            zone.bounds = Box3::fromPoints(zone.outline);
            for (const RoomReading& room : reading.rooms) {
                if (insidePolygon(zone.outline, room.label)) {
                    zone.rooms.push_back(room.name + (room.number.empty() ? "" : " " + room.number));
                }
            }
            for (const SymbolReading& symbol : reading.symbols) {
                if (insidePolygon(zone.outline, symbol.position)) {
                    ++zone.symbols[symbol.tag];
                }
            }
        }
        reading.zones.push_back(std::move(zone));
    }
    std::stable_sort(reading.zones.begin(), reading.zones.end(),
                     [](const ZoneReading& a, const ZoneReading& b) {
                         return sheetNumberLess(a.name, b.name);
                     });

    // ---- Systems: the layers named for one, or drawn like one.
    for (auto& [layerName, system] : systems) {
        system.layer = layerName;
        system.kind = systemKindOf(layerName);
        system.status = systemStatusOf(layerName);
        if (system.kind.empty() && system.runs.empty()) {
            continue;
        }
        reading.systems.push_back(std::move(system));
    }
    std::stable_sort(reading.systems.begin(), reading.systems.end(),
                     [](const SystemReading& a, const SystemReading& b) {
                         return a.runs.size() + a.lines > b.runs.size() + b.lines;
                     });

    // ---- Title blocks: on each sheet, the placed block with the most
    // attributes; in model space too, for the offices that draw the sheet
    // there.
    const auto titleBlockIn = [&](const BlockTableRecord& space, std::string sheet,
                                  std::size_t fewestFields) {
        const InsertEntity* best = nullptr;
        for (const Entity* entity : db.entitiesIn(space)) {
            const auto* insert = dynamic_cast<const InsertEntity*>(entity);
            if (insert == nullptr || insert->erased) {
                continue;
            }
            if (best == nullptr || insert->attributeHandles.size() > best->attributeHandles.size()) {
                best = insert;
            }
        }
        if (best == nullptr) {
            return;
        }
        // A block that names a sheet is a title block however few fields it
        // has; anything else needs the full complement to count as one.
        bool namesASheet = false;
        for (const Handle handle : best->attributeHandles) {
            const auto* attribute = db.get<AttributeEntity>(handle);
            if (attribute == nullptr) {
                continue;
            }
            const std::string tag = upper(attribute->tag);
            if (tag.find("SHEET") != std::string::npos || tag.find("TITLE") != std::string::npos ||
                tag == "DWGNO" || tag == "SHT") {
                namesASheet = true;
            }
        }
        if (best->attributeHandles.size() < (namesASheet ? std::size_t{1} : fewestFields)) {
            return;
        }
        TitleBlockReading title;
        title.sheet = std::move(sheet);
        if (const auto* block = db.get<BlockTableRecord>(best->blockHandle)) {
            title.block = block->name;
        }
        for (const Handle handle : best->attributeHandles) {
            const auto* attribute = db.get<AttributeEntity>(handle);
            if (attribute == nullptr || attribute->erased) {
                continue;
            }
            const std::string value = trimmed(decodeTextSymbols(attribute->value()));
            if (!value.empty()) {
                title.fields.emplace_back(attribute->tag, value);
            }
        }
        if (!title.fields.empty()) {
            reading.titleBlocks.push_back(std::move(title));
        }
    };
    for (const Layout* layout : db.layouts()) {
        const auto* block = db.get<BlockTableRecord>(layout->blockRecordHandle);
        if (block != nullptr && !block->isModelSpace()) {
            titleBlockIn(*block, layout->name, 2);
        }
    }
    // Only the offices that draw the sheet in model space have a title block
    // there, and it has many fields; a tag block with two is a tag.
    if (reading.titleBlocks.empty()) {
        titleBlockIn(model, "model space", 4);
    }

    return reading;
}

std::string DrawingReading::describe() const {
    if (empty()) {
        return {};
    }
    std::ostringstream out;
    out << "What the drawing shows:\n";

    if (!rooms.empty()) {
        std::size_t bounded = 0;
        for (const RoomReading& room : rooms) {
            bounded += room.bounded ? 1 : 0;
        }
        out << "Rooms (" << rooms.size();
        if (boundariesTraced) {
            out << ", " << bounded << " with walls traced";
        } else if (bounded > 0) {
            out << ", " << bounded << " with walls traced before time ran out";
        } else {
            out << ", walls not traced";
        }
        out << "):\n";
        constexpr std::size_t kMaxRooms = 60;
        for (std::size_t i = 0; i < rooms.size() && i < kMaxRooms; ++i) {
            const RoomReading& room = rooms[i];
            out << "  " << room.name;
            if (!room.number.empty()) {
                out << " " << room.number;
            }
            if (room.bounded) {
                out << ": " << num(room.area / areaDivisor) << " " << areaUnit << ", "
                    << num(room.bounds.width()) << " x " << num(room.bounds.height())
                    << ", centre " << point(room.bounds.center());
            } else {
                out << ": label at " << point(room.label) << ", not enclosed";
            }
            out << "\n";
        }
        if (rooms.size() > kMaxRooms) {
            out << "  and " << (rooms.size() - kMaxRooms) << " more\n";
        }
    }

    if (!architecture.empty()) {
        out << "Shell:";
        bool first = true;
        for (const auto& [layer, count] : architecture.wallLayers) {
            out << (first ? " walls on " : ", ") << layer << " (" << count << " lines)";
            first = false;
        }
        if (architecture.doors > 0) {
            out << (first ? " " : "; ") << architecture.doors << " door"
                << (architecture.doors == 1 ? "" : "s");
            bool firstBlock = true;
            for (const auto& [block, count] : architecture.doorBlocks) {
                out << (firstBlock ? " (" : ", ") << block << " x" << count;
                firstBlock = false;
            }
            out << ")";
            first = false;
        }
        if (architecture.windows > 0) {
            out << (first ? " " : "; ") << architecture.windows << " window"
                << (architecture.windows == 1 ? "" : "s");
            bool firstBlock = true;
            for (const auto& [block, count] : architecture.windowBlocks) {
                out << (firstBlock ? " (" : ", ") << block << " x" << count;
                firstBlock = false;
            }
            out << ")";
        }
        out << "\n";
    }

    if (!systems.empty()) {
        out << "Systems by layer:\n";
        constexpr std::size_t kMaxSystems = 20;
        for (std::size_t i = 0; i < systems.size() && i < kMaxSystems; ++i) {
            const SystemReading& system = systems[i];
            out << "  " << system.layer;
            if (!system.kind.empty() || !system.status.empty()) {
                out << " (" << system.kind;
                if (!system.status.empty()) {
                    out << (system.kind.empty() ? "" : ", ") << system.status;
                }
                out << ")";
            }
            out << ":";
            bool first = true;
            const auto item = [&](const std::string& text) {
                out << (first ? " " : "; ") << text;
                first = false;
            };
            if (!system.runs.empty()) {
                std::map<double, int> widths;
                for (const RunReading& run : system.runs) {
                    ++widths[std::round(run.width * 100.0) / 100.0];
                }
                std::string text = std::to_string(system.runs.size()) + " run" +
                                   (system.runs.size() == 1 ? "" : "s") + " of width ";
                bool firstWidth = true;
                for (const auto& [width, count] : widths) {
                    text += (firstWidth ? "" : "/") + num(width);
                    firstWidth = false;
                }
                text += ", total length " + num(system.totalRunLength());
                item(text);
            }
            if (system.lines > 0) {
                item(std::to_string(system.lines) + " line" + (system.lines == 1 ? "" : "s"));
            }
            if (system.circles > 0) {
                item(std::to_string(system.circles) + " circle" + (system.circles == 1 ? "" : "s"));
            }
            if (!system.blocks.empty()) {
                std::string text = "blocks";
                bool firstBlock = true;
                for (const auto& [block, count] : system.blocks) {
                    text += (firstBlock ? " " : ", ") + block + " x" + std::to_string(count);
                    firstBlock = false;
                }
                item(text);
            }
            if (!system.labels.empty()) {
                std::vector<std::pair<std::string, int>> top(system.labels.begin(),
                                                             system.labels.end());
                std::stable_sort(top.begin(), top.end(),
                                 [](const auto& a, const auto& b) { return a.second > b.second; });
                std::string text = "labels";
                for (std::size_t k = 0; k < top.size() && k < 5; ++k) {
                    text += (k == 0 ? " " : ", ") + top[k].first + " x" + std::to_string(top[k].second);
                }
                item(text);
            }
            out << "\n";
        }
        if (systems.size() > kMaxSystems) {
            out << "  and " << (systems.size() - kMaxSystems) << " more\n";
        }
    }

    if (!symbols.empty()) {
        // Counted by tag: "T x19" says how many thermostats without listing
        // where; ASK SYMBOLS T gives the positions.
        std::map<std::string, int> byTag;
        for (const SymbolReading& symbol : symbols) {
            ++byTag[symbol.tag];
        }
        out << "Symbols (letters in circles, device blocks):";
        bool first = true;
        for (const auto& [tag, count] : byTag) {
            out << (first ? " " : ", ") << tag << " x" << count;
            first = false;
        }
        out << "\n";
    }

    if (!zones.empty()) {
        out << "Zones (" << zones.size() << "):\n";
        constexpr std::size_t kMaxZones = 40;
        for (std::size_t i = 0; i < zones.size() && i < kMaxZones; ++i) {
            const ZoneReading& zone = zones[i];
            out << "  " << zone.name;
            if (zone.bounded) {
                out << ": " << num(zone.bounds.width()) << " x " << num(zone.bounds.height())
                    << ", centre " << point(zone.bounds.center());
                if (!zone.rooms.empty()) {
                    out << ", rooms";
                    for (std::size_t r = 0; r < zone.rooms.size() && r < 8; ++r) {
                        out << (r == 0 ? " " : "; ") << zone.rooms[r];
                    }
                    if (zone.rooms.size() > 8) {
                        out << "; and " << (zone.rooms.size() - 8) << " more";
                    }
                }
                if (zone.symbols.empty()) {
                    out << ", no symbols inside";
                } else {
                    out << ", symbols";
                    for (const auto& [tag, count] : zone.symbols) {
                        out << " " << tag << " x" << count;
                    }
                }
            } else {
                out << ": tag at " << point(zone.label) << ", no outline";
            }
            out << "\n";
        }
        if (zones.size() > kMaxZones) {
            out << "  and " << (zones.size() - kMaxZones) << " more\n";
        }
    }

    if (!references.empty()) {
        out << "References:";
        for (std::size_t i = 0; i < references.size() && i < 12; ++i) {
            const ReferenceReading& reference = references[i];
            out << (i == 0 ? " " : "; ") << reference.name << " at " << point(reference.position)
                << " scale " << num(reference.scale) << " rotation "
                << num(reference.rotation * 180.0 / 3.14159265358979323846)
                << (reference.resolved ? "" : " (not found)");
        }
        out << "\n";
    }

    for (const TitleBlockReading& title : titleBlocks) {
        out << "Title block on " << (title.sheet == "model space" ? "" : "sheet ")
            << (title.sheet == "model space" ? title.sheet : "\"" + title.sheet + "\"");
        if (!title.block.empty()) {
            out << " (block " << title.block << ")";
        }
        out << ":";
        constexpr std::size_t kMaxFields = 24;
        for (std::size_t i = 0; i < title.fields.size() && i < kMaxFields; ++i) {
            out << (i == 0 ? " " : "; ") << title.fields[i].first << " = " << title.fields[i].second;
        }
        if (title.fields.size() > kMaxFields) {
            out << "; and " << (title.fields.size() - kMaxFields) << " more";
        }
        out << "\n";
    }

    return out.str();
}

}  // namespace cad
