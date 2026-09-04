/// Tests for fields: text whose value is derived rather than typed.
///
/// The numbers throughout are from the sample drawing, because a field is only
/// worth anything if it agrees with what the application that wrote it computed.
/// Inventing simpler ones would test the code against itself.
#include <doctest/doctest.h>

#include <algorithm>

#include "cad/db/CurveEntities.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/Dictionary.hpp"
#include "cad/db/FieldEvaluator.hpp"
#include "cad/db/FieldObject.hpp"
#include "cad/db/Layout.hpp"
#include "cad/db/MTextFormat.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/db/TableEntity.hpp"
#include "test_helpers.hpp"

using namespace cad;

namespace {

/// Add a field to `db` and hand back a reference to it.
FieldObject& addField(Database& db, std::string evaluator, std::string code,
                      std::string cached = {}) {
    auto field = std::make_unique<FieldObject>();
    field->evaluatorId = std::move(evaluator);
    field->fieldCode = std::move(code);
    field->cachedValue = std::move(cached);
    auto& reference = *field;
    db.addObject(std::move(field));
    return reference;
}

/// The area boundary of the master bedroom in the sample drawing: nine
/// vertices, *not* flagged closed, drawn back onto its own start point.
PolylineEntity masterBedroomBoundary() {
    PolylineEntity polyline;
    for (const Vec3 point : {Vec3{342.226, 325.617, 0.0}, Vec3{342.226, 149.617, 0.0},
                             Vec3{590.226, 149.617, 0.0}, Vec3{590.226, 513.617, 0.0},
                             Vec3{518.226, 513.617, 0.0}, Vec3{518.226, 435.617, 0.0},
                             Vec3{484.226, 435.617, 0.0}, Vec3{484.226, 325.617, 0.0},
                             Vec3{342.226, 325.617, 0.0}}) {
        polyline.vertices.push_back(PolylineVertex{point});
    }
    polyline.closed = false;
    return polyline;
}

}  // namespace

TEST_SUITE("Field formats") {
    TEST_CASE("a conversion factor turns square inches into square feet") {
        // The format on every area label in the sample drawing.
        const FieldFormat format =
            FieldFormat::parse(R"(\AcObjProp Object(x).Area \f "%lu2%pr2%ct8[0.0069444444444444]")");
        CHECK(format.precision.has_value());
        CHECK(*format.precision == 2);
        CHECK(format.conversion == doctest::Approx(1.0 / 144.0));
        CHECK(format.applyToNumber(60924.0) == "423.08");
    }

    TEST_CASE("text case is applied") {
        const FieldFormat format = FieldFormat::parse(R"(\AcObjProp Object(x).TextString \f "%tc1")");
        CHECK(format.textCase == 1);
        CHECK(format.applyToText("Master Bedroom") == "MASTER BEDROOM");
        CHECK(FieldFormat::parse(R"(\f "%tc2")").applyToText("SHUT") == "shut");
    }

    /// A format with no `%pr` does not mean "as few digits as possible". It
    /// means the drawing's own precision, and guessing produced `1912.96` where
    /// the drawing said `1912.958333`.
    TEST_CASE("a format with no precision uses the one it is given") {
        const FieldFormat format = FieldFormat::parse(R"(\f "%lu2")");
        CHECK_FALSE(format.precision.has_value());
        CHECK(format.applyToNumber(1912.9583333333, 6) == "1912.958333");
        CHECK(format.applyToNumber(1912.9583333333, 2) == "1912.96");
    }

    TEST_CASE("a code with no format clause formats plainly") {
        const FieldFormat format = FieldFormat::parse(R"(\AcExpr ((1277.10+2073.48)))");
        CHECK(format.conversion == doctest::Approx(1.0));
        CHECK(format.applyToNumber(3350.58, 6) == "3350.580000");
    }
}

TEST_SUITE("Field arithmetic") {
    TEST_CASE("precedence and parentheses") {
        CHECK(*evaluateArithmetic("1+2*3") == doctest::Approx(7.0));
        CHECK(*evaluateArithmetic("(1+2)*3") == doctest::Approx(9.0));
        CHECK(*evaluateArithmetic("((1277.10+2073.48))") == doctest::Approx(3350.58));
        CHECK(*evaluateArithmetic(" 60924 / 144 ") == doctest::Approx(423.0833333));
        CHECK(*evaluateArithmetic("-4+1") == doctest::Approx(-3.0));
    }

    /// Table formulas reach the same evaluator and must be *refused*, not
    /// half-parsed: a field showing a number it made up is worse than one
    /// showing the number the drawing last knew.
    TEST_CASE("anything that is not arithmetic is refused") {
        CHECK_FALSE(evaluateArithmetic("SUM(B2:B11)/144").has_value());
        CHECK_FALSE(evaluateArithmetic("1+").has_value());
        CHECK_FALSE(evaluateArithmetic("").has_value());
        CHECK_FALSE(evaluateArithmetic("1 2").has_value());
        // Division by zero has no value, so the field keeps its cached one.
        CHECK_FALSE(evaluateArithmetic("1/0").has_value());
    }
}

TEST_SUITE("Field evaluation") {
    TEST_CASE("an object property field reads the object it names") {
        const std::unique_ptr<Database> owned = Database::createEmpty();
        Database& db = *owned;
        db.settings.linearPrecision = 6;

        auto boundary = std::make_unique<PolylineEntity>(masterBedroomBoundary());
        const Handle polyline = db.addEntity(std::move(boundary), db.modelSpace()).handle;

        FieldObject& area = addField(
            db, "AcObjProp.16.2",
            R"(\AcObjProp.16.2 Object(%<\_ObjIdx 0>%).Area \f "%lu2%pr2%ct8[0.0069444444444444]")");
        area.referencedObjects.push_back(polyline);

        const FieldEvaluator evaluator{db, FieldContext{}};
        const FieldResult result = evaluator.evaluate(area);
        CHECK(result.evaluated);
        CHECK(result.text == "423.08");
        // The number carried out is unconverted, for an enclosing expression.
        REQUIRE(result.number.has_value());
        CHECK(*result.number == doctest::Approx(60924.0));
    }

    /// The failure this was found by: the boundary is not flagged closed, and an
    /// open polyline reported an area of zero. Every area label on the sheet
    /// read 0.00.
    TEST_CASE("an area boundary drawn without Close still has an area") {
        const PolylineEntity polyline = masterBedroomBoundary();
        CHECK_FALSE(polyline.closed);
        CHECK(std::abs(polyline.geometry().signedArea()) == doctest::Approx(60924.0));
    }

    TEST_CASE("a text field takes another entity's text and formats it") {
        const std::unique_ptr<Database> owned = Database::createEmpty();
        Database& db = *owned;
        auto label = std::make_unique<MTextEntity>();
        label->contents = R"(Master\PBedroom)";
        const Handle source = db.addEntity(std::move(label), db.modelSpace()).handle;

        FieldObject& upper = addField(
            db, "AcObjProp", R"(\AcObjProp Object(%<\_ObjIdx 0>%).TextString \f "%tc1")");
        upper.referencedObjects.push_back(source);

        FieldObject& wrapper = addField(db, "_text", R"(2nd %<\_FldIdx 0>%)");
        wrapper.childFields.push_back(upper.handle);

        const FieldEvaluator evaluator{db, FieldContext{}};
        const FieldResult result = evaluator.evaluate(wrapper);
        CHECK(result.evaluated);
        // The child's value is data, so its backslash is escaped: the label says
        // "MASTER\PBEDROOM" and does not silently become two lines.
        CHECK(result.text == R"(2nd MASTER\\PBEDROOM)");
    }

    /// An expression works on values, not on what the values were going to look
    /// like. Summing two children that each report square feet and then dividing
    /// by 144 again is wrong by a factor of 144 and looks entirely plausible.
    TEST_CASE("an expression uses its children's numbers, not their text") {
        const std::unique_ptr<Database> owned = Database::createEmpty();
        Database& db = *owned;
        db.settings.linearPrecision = 6;

        auto first = std::make_unique<PolylineEntity>(masterBedroomBoundary());
        const Handle a = db.addEntity(std::move(first), db.modelSpace()).handle;
        auto second = std::make_unique<PolylineEntity>(masterBedroomBoundary());
        const Handle b = db.addEntity(std::move(second), db.modelSpace()).handle;

        const char* areaCode =
            R"(\AcObjProp.16.2 Object(%<\_ObjIdx 0>%).Area \f "%lu2%ct8[0.0069444444444444]")";
        FieldObject& areaA = addField(db, "AcObjProp.16.2", areaCode);
        areaA.referencedObjects.push_back(a);
        FieldObject& areaB = addField(db, "AcObjProp.16.2", areaCode);
        areaB.referencedObjects.push_back(b);

        FieldObject& total = addField(
            db, "AcExpr",
            R"(\AcExpr ((%<\_FldIdx 0>%+%<\_FldIdx 1>%)) \f "%lu2%ct8[0.0069444444444444]")");
        total.childFields.push_back(areaA.handle);
        total.childFields.push_back(areaB.handle);

        const FieldEvaluator evaluator{db, FieldContext{}};
        // Two whole boundaries, in square feet: 2 * 60924 / 144.
        CHECK(evaluator.evaluate(total).text == "846.166667");
    }

    /// A field naming something this application does not implement keeps the
    /// value the authoring application last computed. A stale number is worth
    /// far more than a blank or an error on a construction drawing.
    TEST_CASE("an unevaluable field falls back to its cached value") {
        const std::unique_ptr<Database> owned = Database::createEmpty();
        Database& db = *owned;
        FieldObject& sum =
            addField(db, "AcExpr", R"(\AcExpr (SUM(B2:B11)/144) \f "%lu2")", "1142.30");

        const FieldEvaluator evaluator{db, FieldContext{}};
        const FieldResult result = evaluator.evaluate(sum);
        CHECK_FALSE(result.evaluated);
        CHECK(result.text == "1142.30");
        // This formula *is* implemented now, but only inside the table whose
        // cells it names. Evaluated without one it still falls back, and says
        // which of the two reasons it is falling back for.
        CHECK(result.problem.find("table") != std::string::npos);
    }

    /// And a template around such a field keeps its own literal text: the label
    /// still reads "TOTAL AREA 1142.30" rather than losing the number and the
    /// words with it.
    TEST_CASE("a template survives a child it cannot evaluate") {
        const std::unique_ptr<Database> owned = Database::createEmpty();
        Database& db = *owned;
        FieldObject& sum =
            addField(db, "AcExpr", R"(\AcExpr (SUM(B2:B11)/144) \f "%lu2")", "1142.30");
        FieldObject& label = addField(db, "_text", R"(TOTAL AREA %<\_FldIdx 0>%)");
        label.childFields.push_back(sum.handle);

        const FieldEvaluator evaluator{db, FieldContext{}};
        const FieldResult result = evaluator.evaluate(label);
        CHECK_FALSE(result.evaluated);
        CHECK(result.text == "TOTAL AREA 1142.30");
    }

    TEST_CASE("a field naming an erased object says so and keeps its cache") {
        const std::unique_ptr<Database> owned = Database::createEmpty();
        Database& db = *owned;
        auto label = std::make_unique<MTextEntity>();
        label->contents = "Kitchen";
        Entity& source = db.addEntity(std::move(label), db.modelSpace());
        const Handle handle = source.handle;
        source.erased = true;

        FieldObject& field = addField(
            db, "AcObjProp", R"(\AcObjProp Object(%<\_ObjIdx 0>%).TextString)", "Kitchen");
        field.referencedObjects.push_back(handle);

        const FieldEvaluator evaluator{db, FieldContext{}};
        const FieldResult result = evaluator.evaluate(field);
        CHECK_FALSE(result.evaluated);
        CHECK(result.text == "Kitchen");
        CHECK(result.problem == "field refers to an object that is gone");
    }

    /// A file that names a field from inside itself is malformed. The only
    /// wrong answer is to hang.
    TEST_CASE("a field that refers to itself terminates") {
        const std::unique_ptr<Database> owned = Database::createEmpty();
        Database& db = *owned;
        FieldObject& loop = addField(db, "_text", R"(%<\_FldIdx 0>%)", "cached");
        loop.childFields.push_back(loop.handle);

        const FieldEvaluator evaluator{db, FieldContext{}};
        const FieldResult result = evaluator.evaluate(loop);
        CHECK_FALSE(result.evaluated);
    }

    TEST_CASE("drawing variables are available to fields") {
        const std::unique_ptr<Database> owned = Database::createEmpty();
        Database& db = *owned;
        FieldObject& name = addField(db, "AcVar", R"(\AcVar Filename \f "%tc1")");

        FieldContext context;
        context.fileName = "2nd Floor Remo Sketch.dwg";
        const FieldEvaluator evaluator{db, context};
        CHECK(evaluator.evaluate(name).text == "2ND FLOOR REMO SKETCH.DWG");
    }

    TEST_CASE("an unsupported variable keeps its cache and reports why") {
        const std::unique_ptr<Database> owned = Database::createEmpty();
        Database& db = *owned;
        FieldObject& unknown = addField(db, "AcVar", R"(\AcVar HyperlinkBase)", "K:\\Projects");

        const FieldEvaluator evaluator{db, FieldContext{}};
        const FieldResult result = evaluator.evaluate(unknown);
        CHECK_FALSE(result.evaluated);
        CHECK(result.text == "K:\\Projects");
        CHECK(result.problem == "unsupported field variable HyperlinkBase");
    }

    TEST_CASE("a field survives a clone, which undo depends on") {
        const std::unique_ptr<Database> owned = Database::createEmpty();
        Database& db = *owned;
        FieldObject& field = addField(db, "AcObjProp", R"(\AcObjProp Object(x).Area)", "423.08");
        field.childFields.push_back(Handle{0x10});
        field.referencedObjects.push_back(Handle{0x20});

        const DbObjectPtr copy = field.clone();
        const auto* typed = dynamic_cast<const FieldObject*>(copy.get());
        REQUIRE(typed != nullptr);
        CHECK(typed->evaluatorId == "AcObjProp");
        CHECK(typed->cachedValue == "423.08");
        CHECK(typed->childFields.size() == 1);
        CHECK(typed->referencedObjects.size() == 1);
    }

    TEST_CASE("the evaluator name drops the version suffix") {
        FieldObject field;
        field.evaluatorId = "AcObjProp.16.2";
        CHECK(field.evaluatorName() == "AcObjProp");
        field.evaluatorId = "_text";
        CHECK(field.evaluatorName() == "_text");
    }
}

TEST_SUITE("Dictionaries kept verbatim") {
    TEST_CASE("entries pair each name with the handle that follows it") {
        RawObject dictionary;
        dictionary.dxfType = "DICTIONARY";
        dictionary.preserved = std::make_unique<PreservedData>();
        dictionary.preserved->unknownCodes = {
            {100, std::string{"AcDbDictionary"}},
            {3, std::string{"ACAD_LAYERFILTERS"}}, {360, std::string{"9195"}},
            {3, std::string{"ACAD_FIELD"}},        {360, std::string{"18F"}},
            {3, std::string{"ACLYDICTIONARY"}},    {360, std::string{"9194"}},
        };

        const auto entries = dictionaryEntries(dictionary);
        REQUIRE(entries.size() == 3);
        CHECK(entries[1].first == "ACAD_FIELD");
        CHECK(entries[1].second == Handle{0x18F});

        CHECK(dictionaryEntry(dictionary, "ACAD_FIELD") == Handle{0x18F});
        // Names are matched as DXF matches them.
        CHECK(dictionaryEntry(dictionary, "acad_field") == Handle{0x18F});
        CHECK(dictionaryEntry(dictionary, "ACAD_GROUP").isNull());
    }

    TEST_CASE("an object with no preserved groups has no entries") {
        RawObject empty;
        CHECK(dictionaryEntries(empty).empty());
        CHECK(dictionaryEntry(empty, "ACAD_FIELD").isNull());
    }
}

TEST_SUITE("MText escaping") {
    TEST_CASE("markup characters survive as content") {
        CHECK(MTextEntity::escapeForContents(R"(MASTER\PBEDROOM)") == R"(MASTER\\PBEDROOM)");
        CHECK(MTextEntity::escapeForContents("{braced}") == R"(\{braced\})");
        CHECK(MTextEntity::escapeForContents("plain") == "plain");
    }

    TEST_CASE("escaping and stripping are inverses") {
        MTextEntity text;
        for (const std::string original : {R"(MASTER\PBEDROOM)", "{braced}", "a\\b{c}d"}) {
            text.contents = MTextEntity::escapeForContents(original);
            CHECK(text.plainText() == original);
        }
    }
}

TEST_SUITE("MText paragraphs") {
    /// Paragraph breaks become lines an editor can show, and only those.
    ///
    /// This is what an in-place editor hands the user. Turning a formatting code
    /// it cannot render into something else would reformat a note by the act of
    /// reading it, so everything but `\P` is left exactly as written.
    TEST_CASE("only the paragraph code becomes a line break") {
        CHECK(MTextEntity::paragraphsToLines(R"(FIRST\PSECOND)") == "FIRST\nSECOND");
        CHECK(MTextEntity::paragraphsToLines(R"(\pxi-4;NOTE\PMORE)") == "\\pxi-4;NOTE\nMORE");
        // Height, colour and stacking codes pass through untouched.
        CHECK(MTextEntity::paragraphsToLines(R"({\H2x;BIG} \C1;RED)") == R"({\H2x;BIG} \C1;RED)");
        CHECK(MTextEntity::paragraphsToLines("plain") == "plain");
    }

    TEST_CASE("an escaped backslash is not a paragraph break") {
        // `\\P` is a literal backslash followed by a P — a Windows path in a
        // note, most often. Reading it as a break would split the line and,
        // worse, would not put it back.
        CHECK(MTextEntity::paragraphsToLines(R"(C:\\PROJECTS)") == R"(C:\\PROJECTS)");
        CHECK(MTextEntity::paragraphsToLines(R"(A\\P\PB)") == "A\\\\P\nB");
    }

    TEST_CASE("lines become paragraph breaks again") {
        CHECK(MTextEntity::linesToParagraphs("FIRST\nSECOND") == R"(FIRST\PSECOND)");
        // A carriage return would land in the drawing as a literal character,
        // which renders as a box in most fonts.
        CHECK(MTextEntity::linesToParagraphs("FIRST\r\nSECOND") == R"(FIRST\PSECOND)");
        CHECK(MTextEntity::linesToParagraphs("plain") == "plain");
    }

    TEST_CASE("a note survives being opened in an editor and closed again") {
        // The property that matters: editing a note without changing it must
        // not change it. Anything else silently rewrites a drawing's annotation
        // the first time somebody double-clicks a label to read it.
        for (const std::string original :
             {R"(GENERAL NOTES\PALL WORK TO COMPLY)", R"(\pxi-4;1. FIRST\P2. SECOND)",
              R"({\H0.8x;SMALL}\PNEXT)", R"(C:\\PROJECTS\PSEE PLAN)", "one line", ""}) {
            const std::string shown = MTextEntity::paragraphsToLines(original);
            CHECK(MTextEntity::linesToParagraphs(shown) == original);
        }
    }
}

TEST_SUITE("MText formatting") {
    /// The whole note as plain text, which must agree with `plainText`.
    [[nodiscard]] std::string flatten(const std::vector<MTextRun>& runs) {
        return runsToPlainText(runs);
    }

    TEST_CASE("unformatted text is one run") {
        const std::vector<MTextRun> runs = parseMText("GENERAL NOTES");
        REQUIRE(runs.size() == 1);
        CHECK(runs[0].text == "GENERAL NOTES");
        CHECK_FALSE(runs[0].bold);
        CHECK(runs[0].heightFactor == doctest::Approx(1.0));
        CHECK_FALSE(runs[0].color.has_value());
    }

    TEST_CASE("a brace scopes its formatting and closes it again") {
        // The reason a stack is needed rather than a running set of flags: the
        // second word must come out unaffected.
        const std::vector<MTextRun> runs = parseMText(R"({\C1;red} black)");
        REQUIRE(runs.size() == 2);
        CHECK(runs[0].text == "red");
        REQUIRE(runs[0].color.has_value());
        CHECK(runs[0].color->index() == 1);
        CHECK(runs[1].text == " black");
        CHECK_FALSE(runs[1].color.has_value());
    }

    TEST_CASE("underline is a toggle, and its text survives") {
        // `\L` carries no argument and no semicolon. Treating it as one of the
        // semicolon-terminated codes swallowed everything up to the next
        // semicolon — usually the rest of the note — so an underlined run
        // vanished entirely.
        const std::vector<MTextRun> runs = parseMText(R"(see \Lsheet A-2.2\l for detail; ok)");
        CHECK(flatten(runs) == "see sheet A-2.2 for detail; ok");

        const auto underlined =
            std::find_if(runs.begin(), runs.end(), [](const MTextRun& r) { return r.underline; });
        REQUIRE(underlined != runs.end());
        CHECK(underlined->text == "sheet A-2.2");
    }

    TEST_CASE("a relative height is a factor and an absolute one is not") {
        const std::vector<MTextRun> relative = parseMText(R"({\H2x;BIG}small)");
        REQUIRE(relative.size() == 2);
        CHECK(relative[0].heightFactor == doctest::Approx(2.0));
        CHECK(relative[1].heightFactor == doctest::Approx(1.0));

        // `\H2;` is two drawing units, which a factor cannot express — taken as
        // the entity's own height rather than as two and a half times it, which
        // is what reading it as a factor would do to a 2.5-unit note.
        const std::vector<MTextRun> absolute = parseMText(R"({\H2;text})");
        REQUIRE(absolute.size() == 1);
        CHECK(absolute[0].heightFactor == doctest::Approx(1.0));
    }

    TEST_CASE("the font code carries bold and italic") {
        const std::vector<MTextRun> runs = parseMText(R"({\fArial|b1|i1|c0|p34;both})");
        REQUIRE(runs.size() == 1);
        CHECK(runs[0].font == "Arial");
        CHECK(runs[0].bold);
        CHECK(runs[0].italic);
    }

    TEST_CASE("a true colour is unpacked from the format's own byte order") {
        // The format stores BGR. Read as RGB, a red note comes out blue.
        const std::vector<MTextRun> runs = parseMText(R"({\c255;red})");
        REQUIRE(runs.size() == 1);
        REQUIRE(runs[0].color.has_value());
        CHECK(runs[0].color->rgb() == 0xFF0000u);
    }

    TEST_CASE("ByLayer and ByBlock leave the run's colour unset") {
        // 256 is ByLayer and 0 is ByBlock; neither is a colour this run is
        // choosing, which is exactly what an unset colour says.
        CHECK_FALSE(parseMText(R"({\C256;x})")[0].color.has_value());
        CHECK_FALSE(parseMText(R"({\C0;x})")[0].color.has_value());
    }

    TEST_CASE("a paragraph break ends a run and starts a line") {
        const std::vector<MTextRun> runs = parseMText(R"(GENERAL NOTES\PALL WORK)");
        REQUIRE(runs.size() == 2);
        CHECK(runs[0].text == "GENERAL NOTES");
        CHECK(runs[0].endsParagraph);
        CHECK(runs[1].text == "ALL WORK");
        CHECK(flatten(runs) == "GENERAL NOTES\nALL WORK");
    }

    TEST_CASE("runs that read the same way are merged") {
        // Two scopes, one appearance. The difference is invisible on screen and
        // the renderer would pay for it on every frame.
        const std::vector<MTextRun> runs = parseMText(R"({\C1;a}{\C1;b})");
        REQUIRE(runs.size() == 1);
        CHECK(runs[0].text == "ab");
    }

    TEST_CASE("escaped markup is content, not formatting") {
        const std::vector<MTextRun> runs = parseMText(R"(C:\\PROJECTS \{note\})");
        CHECK(flatten(runs) == R"(C:\PROJECTS {note})");
    }

    TEST_CASE("a stacked fraction reads as one") {
        const std::vector<MTextRun> runs = parseMText(R"(1\S1^2; inch)");
        CHECK(flatten(runs) == "11/2 inch");
    }

    TEST_CASE("the runs agree with plainText on the sample drawing's shapes") {
        // The two must not disagree: one is what is drawn and the other is what
        // is picked, measured and searched.
        MTextEntity text;
        for (const std::string contents :
             {R"(GENERAL NOTES\PALL WORK TO COMPLY)", R"({\H0.8x;SMALL} normal)",
              R"(\pxi-4;1. FIRST\P2. SECOND)", R"(plain)", R"({\C1;red}\Lunder\l)"}) {
            text.contents = contents;
            CHECK(flatten(text.formattedRuns()) == text.plainText());
        }
    }
}

TEST_SUITE("MText round trip") {
    /// Compare two run lists on everything that decides how they read.
    void checkSameRuns(const std::vector<MTextRun>& a, const std::vector<MTextRun>& b) {
        REQUIRE(a.size() == b.size());
        for (std::size_t i = 0; i < a.size(); ++i) {
            CHECK(a[i].text == b[i].text);
            CHECK(a[i].endsParagraph == b[i].endsParagraph);
            CHECK(a[i].sameFormatting(b[i]));
        }
    }

    /// The property an editor depends on: reading a note apart and putting it
    /// back together must not change what it says or how it looks. Everything
    /// else in a rich editor is built on top of this being true.
    TEST_CASE("parsing and serialising are inverses") {
        for (const std::string contents : {
                 R"(GENERAL NOTES)",
                 R"(GENERAL NOTES\PALL WORK TO COMPLY)",
                 R"({\fArial|b1|i0|c0|p0;BOLD} normal)",
                 R"({\H0.7x;small} normal)",
                 R"({\C1;red} black)",
                 R"(see \Lsheet A-2.2\l for detail)",
                 R"(C:\\PROJECTS \{braced\})",
                 R"({\W1.2;wide})",
             }) {
            const std::vector<MTextRun> first = parseMText(contents);
            const std::vector<MTextRun> second = parseMText(formatRuns(first, "Arial"));
            checkSameRuns(first, second);
        }
    }

    TEST_CASE("a code this does not model survives the round trip") {
        // `\pxi-4;` is a paragraph indent — nothing here can show it, and an
        // editor that dropped it would silently reformat a numbered note the
        // first time somebody corrected a typo in it.
        const std::string contents = R"(\pxi-4;1. FIRST\P2. SECOND)";
        const std::string again = formatRuns(parseMText(contents), "Arial");
        CHECK(again.find(R"(\pxi-4;)") != std::string::npos);
        CHECK(runsToPlainText(parseMText(again)) == "1. FIRST\n2. SECOND");
    }

    TEST_CASE("formatting applied to part of a note stays on that part") {
        // What the editor does when a user selects two words and presses Bold:
        // the run is split, and the formatting must not leak into its
        // neighbours — which is what the brace scope is for.
        std::vector<MTextRun> runs = parseMText("one two three");
        REQUIRE(runs.size() == 1);

        MTextRun before{}, middle{}, after{};
        before.text = "one ";
        middle.text = "two";
        middle.bold = true;
        after.text = " three";
        runs = {before, middle, after};

        const std::vector<MTextRun> again = parseMText(formatRuns(runs, "Arial"));
        REQUIRE(again.size() == 3);
        CHECK_FALSE(again[0].bold);
        CHECK(again[1].bold);
        CHECK(again[1].text == "two");
        CHECK_FALSE(again[2].bold);
        CHECK(runsToPlainText(again) == "one two three");
    }

    TEST_CASE("weight needs a font to be a weight of") {
        // MText has no standalone bold code, so with no family available the
        // weight cannot be expressed. Dropped rather than written as something
        // that will not render.
        MTextRun run;
        run.text = "bold";
        run.bold = true;
        const std::string withFont = formatRuns({run}, "Arial");
        CHECK(withFont.find("b1") != std::string::npos);
        CHECK(parseMText(withFont)[0].bold);

        const std::string without = formatRuns({run}, "");
        CHECK(without.find("b1") == std::string::npos);
        CHECK(runsToPlainText(parseMText(without)) == "bold");
    }

    TEST_CASE("markup typed into an editor is escaped, not obeyed") {
        // A user who types a brace into a note means a brace. Unescaped it
        // opens a formatting scope and takes the rest of the note with it.
        MTextRun run;
        run.text = R"(width {W} of 1\2)";
        const std::vector<MTextRun> again = parseMText(formatRuns({run}, "Arial"));
        REQUIRE(again.size() == 1);
        CHECK(again[0].text == R"(width {W} of 1\2)");
    }
}

TEST_SUITE("Schedule formulas") {
    /// A schedule whose column B holds raw values and a total that sums them.
    ///
    /// Shaped like the sample drawing's area charts: a title row, data rows, and
    /// a total whose formula divides by 144 because the cells hold square
    /// inches and the schedule reads in square feet.
    struct Schedule {
        std::unique_ptr<Database> db = Database::createEmpty();
        TableEntity table;

        Schedule() {
            table.rowHeights = {10.0, 10.0, 10.0, 10.0, 10.0};
            table.columnWidths = {40.0, 20.0};
            table.cells.resize(table.rowHeights.size() * table.columnWidths.size());
        }

        void setLiteral(std::size_t row, std::size_t column, std::string text) {
            table.cells[row * table.columnWidths.size() + column].text = std::move(text);
        }

        /// A cell holding `value`, displayed through a format that scales it.
        void setNumber(std::size_t row, std::size_t column, double value) {
            FieldObject& field = addField(*db, "AcExpr",
                                          "\\AcExpr (" + std::to_string(value) + ")");
            table.cells[row * table.columnWidths.size() + column].fieldHandle = field.handle;
        }

        [[nodiscard]] std::string evaluate(const std::string& code) {
            FieldObject& total = addField(*db, "AcExpr", code);
            const FieldEvaluator evaluator{*db, FieldContext{}};
            return evaluator.evaluate(total, table).text;
        }
    };

    TEST_CASE("SUM adds a column and the formula around it converts") {
        Schedule s;
        s.setLiteral(0, 0, "AREA CHART");
        s.setNumber(1, 1, 14400.0);
        s.setNumber(2, 1, 7200.0);
        s.setNumber(3, 1, 2880.0);

        // Rows are numbered from one over the whole grid, title included, which
        // is what the sample drawing's own cached totals confirm.
        CHECK(s.evaluate("\\AcExpr (SUM(B2:B4)/144) \\f \"%pr2%lu2\"") == "170.00");
    }

    TEST_CASE("a blank row inside the range counts as nothing") {
        Schedule s;
        s.setNumber(1, 1, 14400.0);
        s.setNumber(2, 1, 7200.0);
        // Row 4 left empty, as a spacer above a total is in a real schedule.
        CHECK(s.evaluate("\\AcExpr (SUM(B2:B5)/144) \\f \"%pr2%lu2\"") == "150.00");
    }

    TEST_CASE("a cell's formatting codes are not read as its value") {
        Schedule s;
        s.setNumber(1, 1, 14400.0);
        // What other CAD software writes for a field it could not resolve, wrapped in the
        // cell's colour code. Scanning it for the first number finds the 256,
        // which is how a schedule total came out 3.56 square feet heavy.
        s.setLiteral(2, 1, "{\\C256;#####}");

        CHECK(s.evaluate("\\AcExpr (SUM(B2:B3)/144) \\f \"%pr2%lu2\"") == "100.00");
    }

    TEST_CASE("COUNT, MIN, MAX and AVERAGE") {
        Schedule s;
        s.setNumber(1, 1, 10.0);
        s.setNumber(2, 1, 20.0);
        s.setNumber(3, 1, 60.0);

        CHECK(s.evaluate("\\AcExpr (COUNT(B2:B4)) \\f \"%pr0%lu2\"") == "3");
        CHECK(s.evaluate("\\AcExpr (MIN(B2:B4)) \\f \"%pr0%lu2\"") == "10");
        CHECK(s.evaluate("\\AcExpr (MAX(B2:B4)) \\f \"%pr0%lu2\"") == "60");
        CHECK(s.evaluate("\\AcExpr (AVERAGE(B2:B4)) \\f \"%pr0%lu2\"") == "30");
    }

    TEST_CASE("a single cell reference is a value like any other") {
        Schedule s;
        s.setNumber(1, 1, 12.0);
        CHECK(s.evaluate("\\AcExpr ((B2*2)) \\f \"%pr0%lu2\"") == "24");
    }

    TEST_CASE("a formula with no table to stand in says so") {
        auto db = Database::createEmpty();
        FieldObject& field = addField(*db, "AcExpr", "\\AcExpr (SUM(B2:B4)/144)", "999.99");
        const FieldEvaluator evaluator{*db, FieldContext{}};

        // Not zero, and not a guess: the cached value is what the drawing last
        // knew, and the complaint says why it is being used.
        const FieldResult result = evaluator.evaluate(field);
        CHECK_FALSE(result.evaluated);
        CHECK(result.text == "999.99");
        CHECK(result.problem.find("table") != std::string::npos);
    }

    TEST_CASE("the format the file applied wins over the drawing's precision") {
        auto db = Database::createEmpty();
        db->settings.linearPrecision = 6;
        FieldObject& field = addField(*db, "AcExpr", "\\AcExpr ((1.0/3.0)) \\f \"%lu2\"");
        // What the value block recorded, which is not what the field code says.
        field.appliedFormat = "%pr2%lu2";

        const FieldEvaluator evaluator{*db, FieldContext{}};
        CHECK(evaluator.evaluate(field).text == "0.33");
    }
}
