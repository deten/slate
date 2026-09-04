#pragma once

#include <string>
#include <vector>

namespace cad::app {

/// Ribbon metrics, shared so that a control contributed from elsewhere — the
/// layer strip — lines up with the buttons beside it rather than guessing.
///
/// The guessing is what produced "Layer Pro...ies" beside three state switches
/// so short their labels ran into each other.
inline constexpr int kRibbonSmallButtonHeight = 22;
inline constexpr int kRibbonSmallRows = 3;
inline constexpr int kRibbonColumnSpacing = 2;
inline constexpr int kRibbonContentHeight =
    kRibbonSmallButtonHeight * kRibbonSmallRows + kRibbonColumnSpacing * 2;

/// How prominent a command's button is.
enum class RibbonSize : std::uint8_t {
    /// A tall button with room for two lines. For the two or three commands in
    /// a group that are reached for far more often than the rest.
    Large,
    /// One of three stacked in a column.
    Small,
};

/// One command's place in the ribbon.
struct RibbonSlot {
    /// Command name, as the registry knows it.
    std::string command;
    RibbonSize size = RibbonSize::Small;
};

/// A captioned group of buttons within a tab.
struct RibbonGroup {
    std::string title;
    std::vector<RibbonSlot> slots;
};

/// One tab.
struct RibbonTab {
    std::string title;
    std::vector<RibbonGroup> groups;
};

/// Where every built-in command sits in the ribbon.
///
/// A curated table rather than a grouping derived from the command registry's
/// categories, and the difference matters. A category answers "what kind of
/// command is this" — it is a taxonomy, and it is the right thing for a menu.
/// A ribbon answers "what am I doing next", which is a different question with
/// a different answer: MOVE, COPY and ROTATE belong together because a drafter
/// reaches for them in the same breath, and they sit in the same category as
/// TRIM and FILLET, which they have almost nothing to do with in use.
///
/// Deriving the ribbon from categories produced one flat run of every Draw
/// command in registration order, three deep, with no grouping the eye could
/// use. This table is what "like tools near each other" costs: somebody has to
/// say which tools are alike.
///
/// A command *not* in this table still appears — under its category, in the
/// tab that category maps to. That is what keeps a plugin's command reachable
/// without this file having to know it exists.
[[nodiscard]] const std::vector<RibbonTab>& ribbonLayout();

/// The tab an uncurated category belongs in, for plugins and anything added
/// later. Categories with no home land in Tools rather than vanishing.
[[nodiscard]] std::string tabForCategory(const std::string& category);

/// The tab that appears while objects of a given type are selected.
///
/// A contextual tab is the answer to "I have selected this — now what?", and it
/// is the only place in a ribbon where the arrangement can depend on the
/// drawing rather than on the application. Selecting a schedule and being
/// offered ARC is not wrong exactly, it is just no help.
///
/// Empty when there is nothing worth offering for that type — which is most of
/// them today. A contextual tab that appears with one irrelevant button in it
/// is worse than no tab: it trains the user to ignore the place where the
/// relevant thing will eventually be.
///
/// @param objectType an entity's `objectType()`, e.g. "MText" or "Table".
[[nodiscard]] std::vector<RibbonGroup> contextualGroupsFor(const std::string& objectType);

/// The caption of the contextual tab for a type, empty when it has none.
[[nodiscard]] std::string contextualTitleFor(const std::string& objectType);

}  // namespace cad::app
