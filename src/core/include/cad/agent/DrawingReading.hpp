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
    /// False when room boundaries were not traced: too many curves, or no
    /// budget. The rooms are then labels only.
    bool boundariesTraced = false;
    std::string areaUnit;
    /// Divides square drawing units into `areaUnit`.
    double areaDivisor = 1.0;

    [[nodiscard]] bool empty() const noexcept {
        return rooms.empty() && systems.empty() && architecture.empty() && titleBlocks.empty();
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

/// Whether a piece of text reads as a room name rather than a note, a tag or
/// a number: "OFFICE", "CONF. #1", "STAFF WORK AREA" yes; "T", "CFM", "150",
/// "AHU-1" no.
[[nodiscard]] bool looksLikeRoomName(std::string_view text);

}  // namespace cad
