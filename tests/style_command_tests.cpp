// STYLE and DIMSTYLE: making and changing styles from the command line.
#include "test_helpers.hpp"

#include <memory>

#include "cad/command/CommandExecutor.hpp"
#include "cad/command/StandardCommands.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/DimStyle.hpp"
#include "cad/db/TextStyle.hpp"
#include "cad/db/Transaction.hpp"

using namespace cad;

namespace {

struct StyleHarness {
    std::unique_ptr<Database> db = Database::createEmpty();
    TransactionManager tm{*db};
    std::unique_ptr<CommandRegistry> registry = createStandardRegistry();
    SimpleSelection selection;
    CommandExecutor executor{*db, tm, *registry, selection};

    void begin(const char* command) {
        REQUIRE(executor.execute(command).status == CommandOutcome::Status::Running);
    }
    CommandOutcome text(const char* t) { return executor.supply(InputValue::makeText(t)); }
    CommandOutcome number(double n) { return executor.supply(InputValue::makeNumber(n)); }
    CommandOutcome enter() { return executor.supply(InputValue::makeEmpty()); }
};

}  // namespace

TEST_CASE("STYLE makes a text style with a face, height, width and angle, and makes it current") {
    StyleHarness h;
    h.begin("STYLE");
    h.text("NOTES");
    h.text("Arial");
    h.number(3.5);
    h.number(0.9);
    const CommandOutcome outcome = h.number(0.0);
    CHECK(outcome.status == CommandOutcome::Status::Completed);
    const TextStyle* style = h.db->findTextStyle("NOTES");
    REQUIRE(style != nullptr);
    CHECK(style->typeface == "Arial");
    CHECK(style->height == doctest::Approx(3.5));
    CHECK(style->widthFactor == doctest::Approx(0.9));
    CHECK(h.db->settings.currentTextStyleName == "NOTES");

    // A file name goes to the font file instead.
    h.begin("STYLE");
    h.text("PLAIN");
    h.text("simplex.shx");
    h.enter();
    h.enter();
    h.enter();
    const TextStyle* plain = h.db->findTextStyle("PLAIN");
    REQUIRE(plain != nullptr);
    CHECK(plain->fontFile == "simplex.shx");
    CHECK(plain->typeface.empty());

    // Undo takes the style away again.
    REQUIRE(h.tm.undo());
    CHECK(h.db->findTextStyle("PLAIN") == nullptr);
}

TEST_CASE("STYLE on an existing style offers its values and Enter keeps them") {
    StyleHarness h;
    TextStyle& standard = h.db->ensureTextStyle("Standard");
    standard.widthFactor = 0.8;
    h.begin("STYLE");
    h.enter();   // the current style
    h.enter();   // font
    h.enter();   // height
    h.enter();   // width
    CHECK(h.enter().status == CommandOutcome::Status::Completed);
    CHECK(h.db->findTextStyle("Standard")->widthFactor == doctest::Approx(0.8));
}

TEST_CASE("DIMSTYLE makes a style from the current one, with the values given, and makes it current") {
    StyleHarness h;
    h.begin("DIMSTYLE");
    h.text("ARCH");
    h.number(3.0);      // arrows
    h.number(2.5);      // text
    h.enter();          // gap kept
    h.enter();          // extension offset kept
    h.enter();          // extension beyond kept
    h.number(8.0);      // baseline spacing
    h.number(50.0);     // overall scale
    h.number(0.0);      // decimals
    const CommandOutcome outcome = h.text("C");
    CHECK(outcome.status == CommandOutcome::Status::Completed);
    const DimStyle* style = h.db->findDimStyle("ARCH");
    REQUIRE(style != nullptr);
    CHECK(style->arrowSize == doctest::Approx(3.0));
    CHECK(style->textHeight == doctest::Approx(2.5));
    CHECK(style->baselineSpacing == doctest::Approx(8.0));
    CHECK(style->overallScale == doctest::Approx(50.0));
    CHECK(style->linearPrecision == 0);
    CHECK(style->textVerticalPosition == TextVerticalPosition::Centered);
    CHECK(h.db->settings.currentDimStyleName == "ARCH");

    // Changing it again keeps the same record.
    const Handle handle = style->handle;
    h.begin("DIMSTYLE");
    h.enter();
    h.number(4.0);
    for (int i = 0; i < 8; ++i) {
        h.enter();
    }
    CHECK(h.db->findDimStyle("ARCH")->handle == handle);
    CHECK(h.db->findDimStyle("ARCH")->arrowSize == doctest::Approx(4.0));
    REQUIRE(h.tm.undo());
    CHECK(h.db->findDimStyle("ARCH")->arrowSize == doctest::Approx(3.0));
}
