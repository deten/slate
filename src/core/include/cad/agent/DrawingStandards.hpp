#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "cad/db/Color.hpp"
#include "cad/db/Database.hpp"

namespace cad {

class Entity;

/// What a set of drawings has in common: the office's conventions, learned
/// from the drawings themselves rather than written down anywhere.
///
/// An assistant asked to "add the retaining wall" cannot know which layer a
/// wall goes on, what the wall block is called, or what the dimensions should
/// look like, from the one drawing in front of it. It can from the last dozen
/// drawings the office produced. This collects the parts of those drawings
/// that carry intent -- layer names and their colours and linetypes, block
/// names and their attribute tags, the styles in use, the units, the sheet
/// sizes -- and writes them out short enough to go into every request.
struct DrawingStandards {
    struct LayerUse {
        /// How many drawings define the layer, and how many objects sit on it
        /// across all of them. A layer every drawing has with thousands of
        /// objects is a convention; one drawing's empty layer is noise.
        int drawings = 0;
        long objects = 0;
        /// Votes for each colour and linetype, so the description reports the
        /// usual one rather than whichever drawing was read first.
        std::map<std::string, int> colors;
        std::map<std::string, int> linetypes;
        std::map<std::string, long> contents;
        std::string description;
    };
    struct BlockUse {
        int drawings = 0;
        long inserts = 0;
        std::vector<std::string> attributeTags;
    };
    struct StyleUse {
        int drawings = 0;
        std::string detail;
    };

    int drawingCount = 0;
    std::vector<std::string> sources;
    std::map<std::string, LayerUse> layers;
    std::map<std::string, BlockUse> blocks;
    std::map<std::string, StyleUse> textStyles;
    std::map<std::string, StyleUse> dimStyles;
    std::map<std::string, int> units;
    std::map<std::string, int> sheets;

    [[nodiscard]] bool empty() const noexcept { return drawingCount == 0; }

    /// Add what one drawing shows.
    void learnFrom(const Database& db, std::string_view sourceName);

    /// The conventions in prose a model can use, most used first, capped.
    [[nodiscard]] std::string describe() const;
};

/// The name of a layer, or what stands in for one when the handle is dead.
[[nodiscard]] std::string layerNameOf(const Database& db, const Entity& entity);

/// The name of a linetype record, or "ByLayer"/"Continuous" for the defaults.
[[nodiscard]] std::string linetypeNameOf(const Database& db, Handle linetype);

/// A colour the way a drafter says it: the index, with a name for the seven
/// everybody knows, or ByLayer, or the hex of a true colour.
[[nodiscard]] std::string colorNameOf(const Color& color);

/// Drawing units as a word.
[[nodiscard]] std::string drawingUnitsName(DrawingUnits units);

/// One line saying what an object is, where, and on which layer.
///
/// The level of detail a person would give over the phone: a line from here
/// to there, a circle of this radius at that centre, the text and its height,
/// which block and where. Enough to reason about, short enough that a
/// selection of twenty fits in a request.
[[nodiscard]] std::string describeEntity(const Database& db, const Entity& entity);

}  // namespace cad
