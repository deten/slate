#include "RibbonLayout.hpp"

#include <map>

namespace cad::app {
namespace {

constexpr RibbonSize kLarge = RibbonSize::Large;
constexpr RibbonSize kSmall = RibbonSize::Small;

}  // namespace

const std::vector<RibbonTab>& ribbonLayout() {
    static const std::vector<RibbonTab> layout{
        // Home is the tab a drafter stays on, so it is not a category — it is
        // the frequently-reached subset of several, which no single category
        // can express. Everything here is also somewhere else; that repetition
        // is the point.
        {"Home",
         {
             // First, as it is in every application that has one, and three
             // small buttons wide rather than a large Paste: Home is the widest
             // tab and the one that runs out of window first, so a group that
             // is reached by Ctrl+V nine times in ten pays for one column, not
             // three.
             {"Clipboard",
              {{"PASTECLIP", kSmall}, {"COPYCLIP", kSmall}, {"CUTCLIP", kSmall}}},
             // The layer tools that act on what is pointed at. Beside the
             // layer strip, which acts on the current layer by name.
             {"Layer Tools", {{"LAYISO", kSmall}, {"LAYOFF", kSmall}, {"LAYFRZ", kSmall}}},
             {"Draw",
              {{"LINE", kLarge},
               {"PLINE", kLarge},
               {"CIRCLE", kLarge},
               {"ARC", kSmall},
               {"RECTANG", kSmall},
               {"HATCH", kSmall}}},
             // Quick Select sits with the tools that act on a selection,
             // because it is used *while* selecting -- reaching objects that
             // share a property but are nowhere near each other. On a tab of
             // its own a drafter has to go looking for it at exactly the moment
             // they are working somewhere else.
             {"Modify",
              {{"MOVE", kLarge},
               {"COPY", kLarge},
               {"TRIM", kLarge},
               {"ERASE", kSmall},
               {"ROTATE", kSmall},
               {"OFFSET", kSmall},
               {"QSELECT", kSmall}}},
             // Three, not five. Home is the tab a drafter works from all day,
             // so what earns a place here is what they reach for constantly --
             // and Home is also the widest tab, the one that runs out of window
             // first. Edit Text and Update Fields are a double-click and a
             // once-a-session command respectively, and both keep their place
             // on the Annotate tab.
             {"Annotate", {{"MTEXT", kLarge}, {"DIMLINEAR", kLarge}, {"TEXT", kSmall}}},
         }},

        // Drawing: grouped by what the tool *makes*, not by which file the
        // command lives in. Straight edges, curves, filled regions.
        {"Draw",
         {
             {"Lines",
              {{"LINE", kLarge},
               {"XLINE", kSmall},
               {"RAY", kSmall},
               {"PLINE", kLarge},
               {"RECTANG", kSmall},
               {"POLYGON", kSmall},
               {"POINT", kSmall}}},
             {"Curves", {{"CIRCLE", kLarge}, {"ARC", kLarge}, {"ELLIPSE", kSmall}, {"SPLINE", kSmall}}},
             {"Regions", {{"HATCH", kLarge}, {"HATCHEDIT", kSmall}}},
             // Building systems drawn as a run: the system, the size, the
             // route, and the office's conventions do the rest.
             {"Systems", {{"LIKE", kLarge}, {"DUCT", kSmall}}},
         }},

        // Annotation: what a sheet says. Text, then the measurements, then the
        // derived values — which is the order they are added in.
        {"Annotate",
         {
             {"Text", {{"MTEXT", kLarge}, {"TEXT", kLarge}, {"TEXTEDIT", kSmall}, {"STYLE", kSmall}}},
             {"Dimensions",
              {{"DIMLINEAR", kLarge},
               {"DIMALIGNED", kLarge},
               {"DIMRADIUS", kSmall},
               {"DIMDIAMETER", kSmall},
               {"DIMANGULAR", kSmall},
               {"DIMORDINATE", kSmall},
               {"DIMCONTINUE", kSmall},
               {"DIMBASELINE", kSmall},
               {"DIMTEDIT", kSmall},
               {"DIMEDIT", kSmall},
               {"DIMSTYLE", kSmall}}},
             // Its own group rather than filed under Dimensions: a leader
             // measures nothing. It points at something and says what it is,
             // which is a different job from reporting a distance, and other CAD software
             // gives it its own panel for the same reason.
             {"Leaders", {{"LEADER", kLarge}}},
             {"Fields", {{"UPDATEFIELD", kSmall}}},
         }},

        // Modifying: moving things, reshaping things, and getting rid of
        // things. The split that matters is *does this change where it is* or
        // *does this change what it is*.
        {"Modify",
         {
             {"Position",
              {{"MOVE", kLarge},
               {"COPY", kLarge},
               {"ROTATE", kSmall},
               {"MIRROR", kSmall},
               {"SCALE", kSmall},
               {"STRETCH", kSmall},
               {"ALIGN", kSmall},
               {"DRAWORDER", kSmall}}},
             {"Reshape",
              {{"TRIM", kLarge},
               {"EXTEND", kLarge},
               {"FILLET", kSmall},
               {"CHAMFER", kSmall},
               {"OFFSET", kSmall},
               {"BREAK", kSmall},
               {"JOIN", kSmall},
               {"PEDIT", kSmall},
               {"LENGTHEN", kSmall}}},
             {"Objects", {{"ERASE", kLarge}, {"EXPLODE", kSmall}}},
             {"History", {{"UNDO", kLarge}, {"REDO", kLarge}}},
         }},

        {"Insert",
         {
             {"Block",
              {{"INSERT", kLarge},
               {"BLOCK", kLarge},
               {"ATTDEF", kSmall},
               {"ATTEDIT", kSmall},
               {"ATTSYNC", kSmall}}},
             {"External", {{"XATTACH", kLarge}, {"PDFATTACH", kSmall}, {"IMAGEATTACH", kSmall}}},
             {"Project", {{"SHEETINDEX", kSmall}}},
             {"Reference", {{"REFERENCES", kLarge}}},
         }},

        {"View",
         {
             {"Navigate",
              {{"ZOOMEXTENTS", kLarge},
               {"ZOOM", kLarge},
               {"PAN", kSmall},
               {"ZOOMCENTER", kSmall}}},
             {"Sheets",
              {{"LAYOUT", kLarge}, {"MVIEW", kLarge}, {"PAGESETUP", kSmall}, {"VPLAYER", kSmall}}},
             {"Display", {{"LWDISPLAY", kSmall}}},
         }},

        // Tools: what is done *to* the drawing rather than drawn in it.
        {"Tools",
         {
             {"Selection", {{"QSELECT", kLarge}}},
             // Measuring is what a drafter does between drawing things, and
             // other CAD software keeps these together for that reason.
             {"Measure",
              {{"DIST", kLarge},
               {"AREA", kLarge},
               {"ID", kSmall},
               {"LIST", kSmall},
               {"DESCRIBE", kSmall},
               {"MEASURE", kSmall},
               {"DIVIDE", kSmall}}},
             {"Drawing", {{"PURGE", kLarge}}},
         }},
    };
    return layout;
}

namespace {

/// What each kind of selected object is offered, and under what heading.
///
/// Deliberately sparse. Only four types have anything to say yet, and the rest
/// get no tab at all rather than a tab full of commands that happen to exist.
struct ContextualEntry {
    const char* title;
    std::vector<RibbonGroup> groups;
};

[[nodiscard]] const std::map<std::string, ContextualEntry>& contextualEntries() {
    static const std::map<std::string, ContextualEntry> entries{
        {"Text",
         {"Text", {{"Text", {{"TEXTEDIT", kLarge}, {"TEXT", kSmall}, {"MTEXT", kSmall}}}}}},
        {"MText",
         {"Text", {{"Text", {{"TEXTEDIT", kLarge}, {"MTEXT", kSmall}, {"TEXT", kSmall}}}}}},
        {"Attribute",
         {"Attribute", {{"Value", {{"TEXTEDIT", kLarge}}}}}},
        {"AttributeDefinition",
         {"Attribute", {{"Value", {{"TEXTEDIT", kLarge}}}}}},
        // A schedule's cells are fields, so recomputing them is the one thing
        // that genuinely applies to a selected table.
        {"Table", {"Table", {{"Contents", {{"UPDATEFIELD", kLarge}}}}}},
        {"Dimension",
         {"Dimension",
          {{"Text", {{"TEXTEDIT", kLarge}}},
           {"New", {{"DIMLINEAR", kSmall}, {"DIMALIGNED", kSmall}, {"DIMRADIUS", kSmall}}}}}},
        {"Insert",
         {"Block", {{"Block", {{"EXPLODE", kLarge}, {"INSERT", kSmall}, {"BLOCK", kSmall}}}}}},
    };
    return entries;
}

}  // namespace

std::vector<RibbonGroup> contextualGroupsFor(const std::string& objectType) {
    const auto entry = contextualEntries().find(objectType);
    return entry == contextualEntries().end() ? std::vector<RibbonGroup>{} : entry->second.groups;
}

std::string contextualTitleFor(const std::string& objectType) {
    const auto entry = contextualEntries().find(objectType);
    return entry == contextualEntries().end() ? std::string{} : entry->second.title;
}

std::string tabForCategory(const std::string& category) {
    if (category == "Draw") {
        return "Draw";
    }
    if (category == "Modify") {
        return "Modify";
    }
    if (category == "Dimension" || category == "Annotate") {
        return "Annotate";
    }
    if (category == "Block") {
        return "Insert";
    }
    if (category == "View") {
        return "View";
    }
    return "Tools";
}

}  // namespace cad::app
