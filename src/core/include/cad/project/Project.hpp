#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cad {

/// One sheet of a set: what it is called, what it shows, where it lives.
struct ProjectSheet {
    /// "M1.1", "A-101": the number as it appears in the title block.
    std::string number;
    std::string title;
    /// The drawing file, relative to the project folder when it is inside it.
    std::string file;
    /// The layout in that file, when the file holds more than one sheet.
    std::string layout;
    /// The name of a background from the project's list, or empty.
    std::string background;
    /// Free: "draft", "issued", "for review"...
    std::string status;
};

/// A drawing pulled in behind sheets: usually another discipline's plan.
struct ProjectBackground {
    /// "A1.1": how the sheets refer to it.
    std::string name;
    /// Where it is, relative to the project folder when it is inside it.
    std::string file;
};

/// A set of sheets and what stands behind them.
///
/// Sits beside the drawings, never inside them, and is never required: a
/// drawing opens the same with or without one. What it adds is the things
/// the drawings cannot say for themselves -- which files make up a set, in
/// what order, what each is for, and where its backgrounds come from -- so
/// a person can see the set at once and an assistant can be told about it.
struct Project {
    std::string name;
    /// The discipline letter or word the sheet numbers start with: "M".
    std::string discipline;
    /// The template a new sheet is made from, when there is one.
    std::string sheetTemplate;
    /// Anything the drawings cannot say: conventions, client rules.
    std::string notes;
    std::vector<ProjectSheet> sheets;
    std::vector<ProjectBackground> backgrounds;

    [[nodiscard]] bool empty() const noexcept {
        return name.empty() && sheets.empty() && backgrounds.empty();
    }
    [[nodiscard]] const ProjectSheet* findSheet(std::string_view number) const;
    [[nodiscard]] ProjectSheet* findSheet(std::string_view number);
    [[nodiscard]] const ProjectBackground* findBackground(std::string_view wanted) const;

    /// Sheets in drawing-set order: by number, the way an index sheet lists
    /// them, with the numeric parts compared as numbers so M10.1 follows M9.1.
    void sortSheets();
};

/// The file name a project is kept under, beside its drawings.
inline constexpr const char* kProjectFileName = "project.slate";

/// Read a project from its text form. Lines are `key: value`; a sheet or
/// background line holds its fields separated by ` | `. Unknown keys are
/// kept out rather than failed on, so a newer Slate can add fields without
/// an older one refusing the file.
[[nodiscard]] Project parseProject(std::string_view text);

/// The text form, which `parseProject` reads back unchanged.
[[nodiscard]] std::string formatProject(const Project& project);

/// Read the project at `path`; an empty project with `error` set on failure.
[[nodiscard]] Project loadProject(const std::filesystem::path& path, std::string* error = nullptr);

/// Write the project to `path`. False with `error` set on failure.
bool saveProject(const Project& project, const std::filesystem::path& path,
                 std::string* error = nullptr);

/// Compare sheet numbers the way a set is ordered: "M2.1" before "M10.1".
[[nodiscard]] bool sheetNumberLess(std::string_view a, std::string_view b);

}  // namespace cad
