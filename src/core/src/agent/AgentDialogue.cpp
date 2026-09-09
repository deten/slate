#include "cad/agent/AgentDialogue.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <sstream>
#include <stdexcept>

#include "cad/agent/DrawingReading.hpp"
#include "cad/agent/DrawingStandards.hpp"
#include "cad/db/BlockTableRecord.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/TextEntities.hpp"

namespace cad {
namespace {

[[nodiscard]] std::string trimmed(std::string_view text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(begin, end - begin + 1));
}

[[nodiscard]] std::string upper(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

[[nodiscard]] bool isFence(const std::string& line) { return line.rfind("```", 0) == 0; }

[[nodiscard]] std::string num(double value) {
    std::ostringstream out;
    out.precision(value >= 100.0 || value <= -100.0 ? 0 : 2);
    out << std::fixed << value;
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

[[nodiscard]] std::string roomLine(const RoomReading& room, const DrawingReading& reading) {
    std::string line = room.name;
    if (!room.number.empty()) {
        line += " " + room.number;
    }
    if (room.bounded) {
        line += ": " + num(room.area / reading.areaDivisor) + " " + reading.areaUnit + ", " +
                num(room.bounds.width()) + " x " + num(room.bounds.height()) + ", from " +
                point(room.bounds.min) + " to " + point(room.bounds.max) + ", centre " +
                point(room.bounds.center());
    } else {
        line += ": label at " + point(room.label) + ", not enclosed";
    }
    return line;
}

[[nodiscard]] bool matchesRoom(const RoomReading& room, std::string_view wanted) {
    const std::string want = upper(trimmed(wanted));
    if (want.empty()) {
        return false;
    }
    const std::string full = upper(room.name + (room.number.empty() ? "" : " " + room.number));
    return full == want || upper(room.name) == want || upper(room.number) == want ||
           full.find(want) != std::string::npos;
}

/// Lines describing entities, capped, with a count of what was left out.
void listEntities(std::ostringstream& out, const Database& db,
                  const std::vector<const Entity*>& entities, std::size_t cap) {
    std::size_t shown = 0;
    for (const Entity* entity : entities) {
        if (shown == cap) {
            out << "  and " << (entities.size() - shown) << " more\n";
            return;
        }
        out << "  " << describeEntity(db, *entity) << "\n";
        ++shown;
    }
}

[[nodiscard]] std::map<std::string, int> countByType(const std::vector<const Entity*>& entities) {
    std::map<std::string, int> counts;
    for (const Entity* entity : entities) {
        ++counts[entity->objectType()];
    }
    return counts;
}

void writeCounts(std::ostringstream& out, const std::map<std::string, int>& counts) {
    bool first = true;
    for (const auto& [type, count] : counts) {
        out << (first ? "" : ", ") << count << " x " << type;
        first = false;
    }
}

[[nodiscard]] std::vector<const Entity*> liveModelEntities(const Database& db) {
    std::vector<const Entity*> live;
    for (const Entity* entity : db.entitiesIn(const_cast<Database&>(db).modelSpace())) {
        if (entity != nullptr && !entity->erased) {
            live.push_back(entity);
        }
    }
    return live;
}

[[nodiscard]] std::string answerRoom(const Database& db, const DrawingReading& reading,
                                     std::string_view wanted) {
    std::ostringstream out;
    std::vector<const RoomReading*> found;
    for (const RoomReading& room : reading.rooms) {
        if (matchesRoom(room, wanted)) {
            found.push_back(&room);
        }
    }
    if (found.empty()) {
        out << "No room called \"" << trimmed(wanted) << "\". Rooms are:";
        for (const RoomReading& room : reading.rooms) {
            out << " " << room.name << (room.number.empty() ? "" : " " + room.number) << ";";
        }
        out << "\n";
        return out.str();
    }
    const std::vector<const Entity*> entities = liveModelEntities(db);
    constexpr std::size_t kMaxRooms = 3;
    for (std::size_t r = 0; r < found.size() && r < kMaxRooms; ++r) {
        const RoomReading& room = *found[r];
        out << roomLine(room, reading) << "\n";
        if (!room.bounded) {
            continue;
        }
        std::vector<const Entity*> walls;
        for (const Handle handle : room.boundaryHandles) {
            if (const Entity* entity = db.getEntity(handle); entity != nullptr && !entity->erased) {
                walls.push_back(entity);
            }
        }
        out << "Bounded by " << walls.size() << " object" << (walls.size() == 1 ? "" : "s") << ":\n";
        listEntities(out, db, walls, 24);

        // What sits inside: doors on its edge count, runs that start or end
        // in it, blocks and labels placed in it.
        const Box3 inside = room.bounds.expanded(room.bounds.diagonal() * 0.01);
        std::vector<const Entity*> doors;
        std::vector<const Entity*> contents;
        for (const Entity* entity : entities) {
            if (std::find(room.boundaryHandles.begin(), room.boundaryHandles.end(),
                          entity->handle) != room.boundaryHandles.end()) {
                if (shellPartNameOf(layerNameOf(db, *entity)) == "door") {
                    doors.push_back(entity);
                }
                continue;
            }
            const Box3 bounds = entity->bounds();
            if (bounds.isEmpty() || !inside.containsBox(bounds)) {
                continue;
            }
            if (std::find(room.labelHandles.begin(), room.labelHandles.end(), entity->handle) !=
                room.labelHandles.end()) {
                continue;
            }
            contents.push_back(entity);
        }
        if (!doors.empty()) {
            out << doors.size() << " door" << (doors.size() == 1 ? "" : "s") << ":\n";
            listEntities(out, db, doors, 8);
        }
        if (!contents.empty()) {
            out << "Inside (" << contents.size() << "): ";
            writeCounts(out, countByType(contents));
            out << "\n";
            listEntities(out, db, contents, 20);
        } else {
            out << "Nothing else is drawn inside it.\n";
        }
    }
    if (found.size() > kMaxRooms) {
        out << "and " << (found.size() - kMaxRooms) << " more rooms match; ask by number\n";
    }
    return out.str();
}

[[nodiscard]] std::string answerRooms(const DrawingReading& reading) {
    if (reading.rooms.empty()) {
        return "No rooms were read from the drawing.\n";
    }
    std::ostringstream out;
    out << "Rooms (" << reading.rooms.size() << "):\n";
    for (const RoomReading& room : reading.rooms) {
        out << "  " << roomLine(room, reading) << "\n";
    }
    return out.str();
}

[[nodiscard]] std::string answerLayer(const Database& db, std::string_view wanted) {
    const std::string name = upper(trimmed(wanted));
    std::vector<const Entity*> on;
    for (const Entity* entity : liveModelEntities(db)) {
        if (upper(layerNameOf(db, *entity)) == name) {
            on.push_back(entity);
        }
    }
    std::ostringstream out;
    if (on.empty()) {
        bool exists = false;
        for (const Handle handle : db.layers().handles()) {
            const auto* layer = db.get<Layer>(handle);
            if (layer != nullptr && upper(layer->name) == name) {
                exists = true;
            }
        }
        out << (exists ? "Nothing is on layer " : "There is no layer ") << trimmed(wanted) << "\n";
        return out.str();
    }
    out << "On layer " << trimmed(wanted) << " (" << on.size() << "): ";
    writeCounts(out, countByType(on));
    out << "\n";
    listEntities(out, db, on, 40);
    return out.str();
}

[[nodiscard]] std::string answerNear(const Database& db, std::string_view argument) {
    // "x,y" or "x,y radius".
    std::istringstream in{trimmed(argument)};
    std::string where;
    double radius = 0.0;
    in >> where >> radius;
    const auto comma = where.find(',');
    double x = 0.0;
    double y = 0.0;
    try {
        if (comma == std::string::npos) {
            throw std::invalid_argument("no comma");
        }
        x = std::stod(where.substr(0, comma));
        y = std::stod(where.substr(comma + 1));
    } catch (const std::exception&) {
        return "NEAR needs a point as x,y and an optional radius, e.g. ASK NEAR 1200,1400 100\n";
    }
    const std::vector<const Entity*> entities = liveModelEntities(db);
    if (radius <= 0.0) {
        Box3 all;
        for (const Entity* entity : entities) {
            all = all.united(entity->bounds());
        }
        radius = all.isEmpty() ? 100.0 : all.diagonal() * 0.02;
    }
    const Box3 around = Box3::fromCenterSize(Vec3{x, y, 0.0}, Vec3{2 * radius, 2 * radius, 0.0});
    std::vector<std::pair<double, const Entity*>> near;
    for (const Entity* entity : entities) {
        if (entity->bounds().intersects(around)) {
            near.emplace_back(entity->distanceToPoint(Vec3{x, y, 0.0}), entity);
        }
    }
    std::sort(near.begin(), near.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::ostringstream out;
    if (near.empty()) {
        out << "Nothing within " << num(radius) << " of " << num(x) << "," << num(y) << "\n";
        return out.str();
    }
    out << "Within " << num(radius) << " of " << num(x) << "," << num(y) << " (" << near.size()
        << "), nearest first:\n";
    std::vector<const Entity*> ordered;
    for (const auto& [distance, entity] : near) {
        ordered.push_back(entity);
    }
    listEntities(out, db, ordered, 40);
    return out.str();
}

[[nodiscard]] std::string answerBlock(const Database& db, std::string_view wanted) {
    const std::string name = upper(trimmed(wanted));
    const BlockTableRecord* block = nullptr;
    for (const Handle handle : db.blocks().handles()) {
        const auto* candidate = db.get<BlockTableRecord>(handle);
        if (candidate != nullptr && !candidate->erased && upper(candidate->name) == name) {
            block = candidate;
        }
    }
    std::ostringstream out;
    if (block == nullptr) {
        out << "There is no block called " << trimmed(wanted) << "\n";
        return out.str();
    }
    std::vector<const Entity*> contents;
    std::vector<std::string> tags;
    for (const Entity* entity : db.entitiesIn(*block)) {
        if (entity == nullptr || entity->erased) {
            continue;
        }
        if (const auto* definition = dynamic_cast<const AttributeDefinitionEntity*>(entity)) {
            tags.push_back(definition->tag);
        }
        contents.push_back(entity);
    }
    out << "Block " << block->name << " holds " << contents.size() << " object"
        << (contents.size() == 1 ? "" : "s");
    if (!contents.empty()) {
        out << ": ";
        writeCounts(out, countByType(contents));
    }
    out << ", spanning " << num(const_cast<Database&>(db).blockExtents(*block).width()) << " x "
        << num(const_cast<Database&>(db).blockExtents(*block).height()) << "\n";
    if (!tags.empty()) {
        out << "Attributes:";
        for (const std::string& tag : tags) {
            out << " " << tag;
        }
        out << "\n";
    }
    std::vector<const Entity*> placements;
    for (const Entity* entity : liveModelEntities(db)) {
        if (const auto* insert = dynamic_cast<const InsertEntity*>(entity);
            insert != nullptr && insert->blockHandle == block->handle) {
            placements.push_back(entity);
        }
    }
    out << "Placed " << placements.size() << " time" << (placements.size() == 1 ? "" : "s")
        << (placements.empty() ? "" : ":") << "\n";
    listEntities(out, db, placements, 12);
    return out.str();
}

[[nodiscard]] std::string answerSymbols(const DrawingReading& reading, std::string_view wanted) {
    const std::string tag = upper(trimmed(wanted));
    std::vector<const SymbolReading*> found;
    for (const SymbolReading& symbol : reading.symbols) {
        if (tag.empty() || upper(symbol.tag) == tag) {
            found.push_back(&symbol);
        }
    }
    std::ostringstream out;
    if (found.empty()) {
        out << (tag.empty() ? "No symbols were read." : "No symbol tagged " + tag + ".");
        if (!reading.symbols.empty()) {
            std::map<std::string, int> byTag;
            for (const SymbolReading& symbol : reading.symbols) {
                ++byTag[symbol.tag];
            }
            out << " Symbols are:";
            for (const auto& [name, count] : byTag) {
                out << " " << name << " x" << count << ";";
            }
        }
        out << "\n";
        return out.str();
    }
    out << "Symbols" << (tag.empty() ? "" : " tagged " + tag) << " (" << found.size() << "):\n";
    constexpr std::size_t kMax = 60;
    for (std::size_t i = 0; i < found.size() && i < kMax; ++i) {
        const SymbolReading& symbol = *found[i];
        out << "  " << symbol.tag << " at " << point(symbol.position) << " on layer " << symbol.layer
            << " (" << symbol.kind << ")";
        // Which room it is in, when one holds it.
        for (const RoomReading& room : reading.rooms) {
            if (room.bounded && room.bounds.containsPoint(symbol.position)) {
                out << ", in " << room.name << (room.number.empty() ? "" : " " + room.number);
                break;
            }
        }
        out << "\n";
    }
    if (found.size() > kMax) {
        out << "  and " << (found.size() - kMax) << " more\n";
    }
    return out.str();
}

[[nodiscard]] std::string zoneLine(const ZoneReading& zone) {
    std::string line = zone.name;
    if (!zone.bounded) {
        return line + ": tag at " + point(zone.label) + ", no outline";
    }
    line += ": " + num(zone.bounds.width()) + " x " + num(zone.bounds.height()) + ", from " +
            point(zone.bounds.min) + " to " + point(zone.bounds.max) + ", centre " +
            point(zone.bounds.center());
    if (!zone.rooms.empty()) {
        line += ", rooms";
        for (std::size_t i = 0; i < zone.rooms.size(); ++i) {
            line += (i == 0 ? " " : "; ") + zone.rooms[i];
        }
    }
    if (zone.symbols.empty()) {
        line += ", no symbols inside";
    } else {
        line += ", symbols";
        for (const auto& [tag, count] : zone.symbols) {
            line += " " + tag + " x" + std::to_string(count);
        }
    }
    return line;
}

[[nodiscard]] std::string answerZones(const DrawingReading& reading) {
    if (reading.zones.empty()) {
        return "No zones were read: no tag like ZONE 3 or VAV-2, and no closed outline on a zone "
               "layer.\n";
    }
    std::ostringstream out;
    out << "Zones (" << reading.zones.size() << "):\n";
    for (const ZoneReading& zone : reading.zones) {
        out << "  " << zoneLine(zone) << "\n";
    }
    return out.str();
}

[[nodiscard]] const ZoneReading* findZone(const DrawingReading& reading, std::string_view wanted) {
    std::string name;
    const std::string want = looksLikeZoneName(wanted, &name) ? name : upper(trimmed(wanted));
    for (const ZoneReading& zone : reading.zones) {
        if (upper(zone.name) == want) {
            return &zone;
        }
    }
    for (const ZoneReading& zone : reading.zones) {
        if (upper(zone.name).find(want) != std::string::npos) {
            return &zone;
        }
    }
    return nullptr;
}

[[nodiscard]] std::string answerZone(const DrawingReading& reading, std::string_view wanted) {
    const ZoneReading* zone = findZone(reading, wanted);
    if (zone == nullptr) {
        std::ostringstream out;
        out << "No zone called \"" << trimmed(wanted) << "\". Zones are:";
        for (const ZoneReading& candidate : reading.zones) {
            out << " " << candidate.name << ";";
        }
        out << "\n";
        return out.str();
    }
    std::ostringstream out;
    out << zoneLine(*zone) << "\n";
    if (zone->bounded) {
        // Each room of the zone with its own line, so a device can be put
        // in a particular one.
        for (const RoomReading& room : reading.rooms) {
            const std::string full = room.name + (room.number.empty() ? "" : " " + room.number);
            if (std::find(zone->rooms.begin(), zone->rooms.end(), full) != zone->rooms.end()) {
                out << "  " << roomLine(room, reading) << "\n";
            }
        }
    }
    return out.str();
}

[[nodiscard]] std::string answerSpots(const Database& db, const DrawingReading& reading,
                                      std::string_view wanted) {
    // A room by name or number, or a zone: for a zone, the spots of each of
    // its rooms that has none of the wanted device yet.
    std::vector<const RoomReading*> rooms;
    if (const ZoneReading* zone = findZone(reading, wanted); zone != nullptr && zone->bounded) {
        for (const RoomReading& room : reading.rooms) {
            const std::string full = room.name + (room.number.empty() ? "" : " " + room.number);
            if (std::find(zone->rooms.begin(), zone->rooms.end(), full) != zone->rooms.end()) {
                rooms.push_back(&room);
            }
        }
    } else {
        for (const RoomReading& room : reading.rooms) {
            if (matchesRoom(room, wanted)) {
                rooms.push_back(&room);
            }
        }
    }
    if (rooms.empty()) {
        return "No room or zone called \"" + trimmed(wanted) + "\".\n";
    }
    std::ostringstream out;
    constexpr std::size_t kMaxRooms = 6;
    for (std::size_t r = 0; r < rooms.size() && r < kMaxRooms; ++r) {
        const RoomReading& room = *rooms[r];
        out << room.name << (room.number.empty() ? "" : " " + room.number);
        if (!room.bounded) {
            out << ": no walls traced, so no wall spots\n";
            continue;
        }
        const std::vector<SpotReading> spots = candidateSpots(db, reading, room);
        if (spots.empty()) {
            out << ": no clear wall found\n";
            continue;
        }
        out << ", wall spots best first:\n";
        for (std::size_t i = 0; i < spots.size(); ++i) {
            out << "  " << (i + 1) << ". " << point(spots[i].position) << " on " << spots[i].reason
                << ", wall runs " << num(spots[i].along.x) << "," << num(spots[i].along.y) << "\n";
        }
    }
    if (rooms.size() > kMaxRooms) {
        out << "and " << (rooms.size() - kMaxRooms) << " more rooms; ask one at a time\n";
    }
    return out.str();
}

}  // namespace

AgentTurnReading readAgentTurn(std::string_view reply) {
    AgentTurnReading reading;
    std::string text{reply};
    std::istringstream stream{text};
    std::string raw;
    while (std::getline(stream, raw)) {
        const std::string line = trimmed(raw);
        if (line.empty() || isFence(line)) {
            continue;
        }
        if (line.front() == '?') {
            AgentQuestion question;
            std::string body = trimmed(line.substr(1));
            if (!body.empty() && body.back() == ']') {
                if (const auto open = body.rfind('['); open != std::string::npos) {
                    question.suggested = trimmed(body.substr(open + 1, body.size() - open - 2));
                    body = trimmed(body.substr(0, open));
                }
            }
            question.text = body;
            if (!question.text.empty()) {
                reading.questions.push_back(std::move(question));
            }
            continue;
        }
        const std::string head = upper(line.substr(0, 4));
        if (head == "ASK " || upper(line) == "ASK") {
            AgentQuery query;
            query.source = line;
            const std::string rest = trimmed(line.substr(3));
            const auto space = rest.find(' ');
            query.kind = upper(space == std::string::npos ? rest : rest.substr(0, space));
            query.argument = space == std::string::npos ? std::string{} : trimmed(rest.substr(space + 1));
            reading.queries.push_back(std::move(query));
        }
    }
    return reading;
}

std::string answerAgentQuery(const Database& db, const DrawingReading& reading,
                             const AgentQuery& query) {
    if (query.kind == "ROOM") {
        return answerRoom(db, reading, query.argument);
    }
    if (query.kind == "ROOMS") {
        return answerRooms(reading);
    }
    if (query.kind == "LAYER") {
        return answerLayer(db, query.argument);
    }
    if (query.kind == "NEAR") {
        return answerNear(db, query.argument);
    }
    if (query.kind == "BLOCK") {
        return answerBlock(db, query.argument);
    }
    if (query.kind == "SYMBOLS") {
        return answerSymbols(reading, query.argument);
    }
    if (query.kind == "ZONES") {
        return answerZones(reading);
    }
    if (query.kind == "ZONE") {
        return answerZone(reading, query.argument);
    }
    if (query.kind == "SPOTS") {
        return answerSpots(db, reading, query.argument);
    }
    return "Unknown question \"" + query.source +
           "\". Ask ROOM <name or number>, ROOMS, LAYER <name>, NEAR <x,y> [radius], BLOCK <name>, "
           "SYMBOLS [tag], ZONES, ZONE <name>, SPOTS <room or zone>, SHEET <number> or PROJECT.\n";
}

}  // namespace cad
