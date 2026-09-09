#pragma once

#include <chrono>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "cad/db/Database.hpp"
#include "cad/math/Box3.hpp"
#include "cad/math/Vec3.hpp"

namespace cad {

/// A room: a name label, the number that goes with it, and the space it sits
/// in when the walls around it close.
struct RoomReading {
    std::string name;
    std::string number;
    /// Where the name label sits.
    Vec3 label;
    /// True when the walls around the label were traced into a closed loop.
    bool bounded = false;
    /// Area of that loop in square drawing units; zero when not bounded.
    double area = 0.0;
    Box3 bounds;
    std::vector<Handle> labelHandles;
    /// The entities whose curves form the boundary, when bounded.
    std::vector<Handle> boundaryHandles;
};

/// One drawn run: a polyline given a width, which is how ducts and pipes are
/// drawn single-line.
struct RunReading {
    Handle handle;
    double width = 0.0;
    double length = 0.0;
    Vec3 start;
    Vec3 end;
};

/// What one layer carries, for the layers that hold a building system.
struct SystemReading {
    std::string layer;
    /// "supply duct", "return duct", "exhaust duct", "duct", "pipe",
    /// "equipment", "fittings", or "" for a layer named for nothing in
    /// particular but drawn like a system.
    std::string kind;
    /// "existing", "demolition", "new", or "".
    std::string status;
    std::vector<RunReading> runs;
    long lines = 0;
    long circles = 0;
    std::map<std::string, int> blocks;
    std::map<std::string, int> labels;

    [[nodiscard]] double totalRunLength() const;
};

/// The building shell as drawn: which layers hold walls, doors and windows.
struct ArchitectureReading {
    std::map<std::string, long> wallLayers;
    std::map<std::string, int> doorBlocks;
    std::map<std::string, int> windowBlocks;
    int doors = 0;
    int windows = 0;

    [[nodiscard]] bool empty() const noexcept {
        return wallLayers.empty() && doors == 0 && windows == 0;
    }
};

/// The attributes of the block that serves as a sheet's title block.
struct TitleBlockReading {
    std::string sheet;
    std::string block;
    std::vector<std::pair<std::string, std::string>> fields;
};

/// A small symbol: a letter in a circle, or a block named for what it is.
/// Thermostats, sensors, switches and the like are drawn this way.
struct SymbolReading {
    /// "T", "S", or the block's name.
    std::string tag;
    Vec3 position;
    std::string layer;
    /// "circle" for a letter in a circle, "block" for a named block.
    std::string kind;
    Handle handle;
};

/// A zone: a tag such as "ZONE 3" or "VAV-2", the area it marks when a
/// closed outline holds it, and what lies inside.
struct ZoneReading {
    std::string name;
    Vec3 label;
    bool bounded = false;
    Box3 bounds;
    std::vector<Vec3> outline;
    /// Rooms whose labels lie inside the outline, by name and number.
    std::vector<std::string> rooms;
    /// Symbol tags inside the outline, with counts: "T" -> 1.
    std::map<std::string, int> symbols;
    std::vector<Handle> labelHandles;
};

/// An external reference as placed in model space: which file, where, how
/// big, how turned. What a sibling sheet needs to place the same background
/// the same way.
struct ReferenceReading {
    std::string name;
    std::string path;
    bool resolved = false;
    Vec3 position;
    double scale = 1.0;
    /// Radians.
    double rotation = 0.0;
};

struct DrawingReadingOptions {
    /// Tracing room boundaries stops when this runs out; the rooms left are
    /// reported by their labels only. A model waiting on a request should
    /// not wait on a floor plan with three hundred rooms.
    std::chrono::milliseconds budget{2000};
    std::size_t maxRooms = 200;
    /// Above this many boundary curves no tracing is attempted at all.
    std::size_t maxBoundaryCurves = 20000;
};

/// The drawing read as things a person would name -- rooms, systems, walls
/// and doors, the title block -- rather than as entities.
///
/// A model asked to "lay out the supply duct for this floor" needs to know
/// which rooms there are, how big, where, and what already runs through them.
/// A digest of entity counts cannot tell it; this can, and it is built by
/// ordinary geometry from what the drafter drew, so it can be shown to the
/// user and checked before anything is built on it.
struct DrawingReading {
    std::vector<RoomReading> rooms;
    std::vector<SystemReading> systems;
    ArchitectureReading architecture;
    std::vector<TitleBlockReading> titleBlocks;
    std::vector<ReferenceReading> references;
    std::vector<SymbolReading> symbols;
    std::vector<ZoneReading> zones;
    /// False when room boundaries were not traced: too many curves, or no
    /// budget. The rooms are then labels only.
    bool boundariesTraced = false;
    std::string areaUnit;
    /// Divides square drawing units into `areaUnit`.
    double areaDivisor = 1.0;

    [[nodiscard]] bool empty() const noexcept {
        return rooms.empty() && systems.empty() && architecture.empty() && titleBlocks.empty() &&
               references.empty() && symbols.empty() && zones.empty();
    }

    /// The reading in prose: what DESCRIBE prints and what the assistant is
    /// told. Capped so a large plan does not crowd out the request.
    [[nodiscard]] std::string describe() const;
};

[[nodiscard]] DrawingReading readDrawing(const Database& db,
                                         const DrawingReadingOptions& options = {});

/// What a layer named like this holds, by the conventions offices actually
/// use: "supply duct" for HSDUCT or M-DUCT-SUPP, "pipe" for M-PIPE-CHW, and
/// so on. Empty for a layer that names no system.
[[nodiscard]] std::string systemKindOf(std::string_view layerName);

/// "existing", "demolition" or "new" from the suffixes drafters put on layer
/// names, or "" when the name says nothing about it.
[[nodiscard]] std::string systemStatusOf(std::string_view layerName);

/// "wall", "door" or "window" for a layer named for one of them, else "".
[[nodiscard]] std::string shellPartNameOf(std::string_view layerName);

/// Whether text is a zone tag: "ZONE 3", "ZONE-3", "Z-3", "VAV-2", "VAV 2".
/// The zone's name comes back through `name` in a settled form: "ZONE 3",
/// "VAV-2".
[[nodiscard]] bool looksLikeZoneName(std::string_view text, std::string* name = nullptr);

/// A place a wall-mounted device could go, with the reason it is a good
/// one, for the assistant to choose among.
struct SpotReading {
    Vec3 position;
    /// The wall's direction at the spot, so a symbol can face into the room.
    Vec3 along;
    std::string reason;
    /// Lower is better: near the door, on an inside wall, away from
    /// diffusers and windows.
    double cost = 0.0;
};

/// Candidate spots on the walls of a bounded room: on interior walls, clear
/// of doors, windows and air terminals, nearest the door first. At most
/// `count`. Empty for a room without walls.
[[nodiscard]] std::vector<SpotReading> candidateSpots(const Database& db,
                                                      const DrawingReading& reading,
                                                      const RoomReading& room,
                                                      std::size_t count = 5);

/// Whether a piece of text reads as a room name rather than a note, a tag or
/// a number: "OFFICE", "CONF. #1", "STAFF WORK AREA" yes; "T", "CFM", "150",
/// "AHU-1" no.
[[nodiscard]] bool looksLikeRoomName(std::string_view text);

}  // namespace cad
