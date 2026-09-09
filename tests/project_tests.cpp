// The project file: a set of sheets and their backgrounds, kept beside the
// drawings, read and written by Slate.
#include "test_helpers.hpp"

#include <string>

#include "cad/project/Project.hpp"

using namespace cad;

TEST_CASE("a project survives the trip through its text form") {
    Project project;
    project.name = "0804 Admin Building HVAC";
    project.discipline = "M";
    project.sheetTemplate = "template.dwg";
    project.notes = "Client wants no exposed duct.\nAll sizes in inches.";
    project.backgrounds.push_back(ProjectBackground{"A1.1", "../Arch/A1.1 Floor Plan.dwg"});
    project.sheets.push_back(ProjectSheet{"M2.1", "Duct Plan L1", "M2.1.dwg", "", "A1.1", "draft"});
    project.sheets.push_back(ProjectSheet{"M0.1", "General Notes", "M0.1.dwg", "Layout1", "", ""});
    project.sheets.push_back(ProjectSheet{"M10.1", "Schedules", "M10.1.dwg", "", "", "issued"});
    project.sortSheets();

    const std::string text = formatProject(project);
    const Project back = parseProject(text);

    CHECK(back.name == project.name);
    CHECK(back.discipline == "M");
    CHECK(back.sheetTemplate == "template.dwg");
    CHECK(back.notes == project.notes);
    REQUIRE(back.backgrounds.size() == 1);
    CHECK(back.backgrounds[0].name == "A1.1");
    CHECK(back.backgrounds[0].file == "../Arch/A1.1 Floor Plan.dwg");
    REQUIRE(back.sheets.size() == 3);
    // In set order: M0.1, M2.1, M10.1 -- the numbers compared as numbers.
    CHECK(back.sheets[0].number == "M0.1");
    CHECK(back.sheets[0].layout == "Layout1");
    CHECK(back.sheets[1].number == "M2.1");
    CHECK(back.sheets[1].background == "A1.1");
    CHECK(back.sheets[1].status == "draft");
    CHECK(back.sheets[2].number == "M10.1");
    CHECK(back.sheets[2].title == "Schedules");
    CHECK(back.findSheet("M2.1") != nullptr);
    CHECK(back.findSheet("M9.9") == nullptr);
    CHECK(back.findBackground("A1.1") != nullptr);
}

TEST_CASE("sheet numbers order like a drawing index, not like text") {
    CHECK(sheetNumberLess("M2.1", "M10.1"));
    CHECK_FALSE(sheetNumberLess("M10.1", "M2.1"));
    CHECK(sheetNumberLess("M1.1", "M1.2"));
    CHECK(sheetNumberLess("A-101", "M-101"));
    CHECK(sheetNumberLess("M0.1", "M0.10"));
    CHECK(sheetNumberLess("m1.1", "M1.2"));
    CHECK_FALSE(sheetNumberLess("M1.1", "M1.1"));
}

TEST_CASE("unknown keys and comments are passed over, and a bar cannot break a field") {
    const Project project = parseProject(
        "# a comment\n"
        "project: Test\n"
        "colour: blue\n"
        "sheet: M1.1 | Plan | M1.1.dwg\n"
        "sheet:  | no number, dropped\n"
        "background: A1.1\n");
    CHECK(project.name == "Test");
    REQUIRE(project.sheets.size() == 1);
    CHECK(project.sheets[0].file == "M1.1.dwg");
    CHECK(project.sheets[0].layout.empty());
    REQUIRE(project.backgrounds.size() == 1);
    CHECK(project.backgrounds[0].file.empty());

    Project barred;
    barred.name = "Bars | in | names";
    barred.sheets.push_back(ProjectSheet{"M1.1", "Plan | Level 1", "M1.1.dwg", "", "", ""});
    const Project back = parseProject(formatProject(barred));
    CHECK(back.name == "Bars   in   names");
    REQUIRE(back.sheets.size() == 1);
    CHECK(back.sheets[0].title == "Plan   Level 1");
    CHECK(back.sheets[0].file == "M1.1.dwg");
}

TEST_CASE("layer patterns match the way layer filters do") {
    CHECK(layerPatternMatches("A-ANNO*", "A-ANNO-TEXT"));
    CHECK(layerPatternMatches("a-anno*", "A-ANNO"));
    CHECK_FALSE(layerPatternMatches("A-ANNO*", "A-WALL"));
    CHECK(layerPatternMatches("*DIM*", "A-DIMS-01"));
    CHECK(layerPatternMatches("A-WALL-?", "A-WALL-N"));
    CHECK_FALSE(layerPatternMatches("A-WALL-?", "A-WALL-NEW"));
    CHECK(layerPatternMatches("*", ""));
    CHECK_FALSE(layerPatternMatches("", "A"));
}

TEST_CASE("the layers to freeze in backgrounds go through the file") {
    Project project;
    project.name = "Set";
    project.freezeInBackgrounds = {"A-ANNO*", "A-DIMS*"};
    const Project back = parseProject(formatProject(project));
    REQUIRE(back.freezeInBackgrounds.size() == 2);
    CHECK(back.freezeInBackgrounds[0] == "A-ANNO*");
    CHECK(back.freezeInBackgrounds[1] == "A-DIMS*");
}

TEST_CASE("an empty project reads back empty") {
    CHECK(parseProject("").empty());
    CHECK(parseProject(formatProject(Project{})).empty());
}
