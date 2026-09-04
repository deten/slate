/// Tests for dimensions and hatches.
///
/// These two are where a CAD file stops being geometry and starts being a
/// *drawing*, and they are also where the conventions are densest: a dimension
/// that measures the right distance but writes it in the wrong format is still
/// wrong, and a hatch whose islands fill instead of hollowing is a different
/// drawing. Most of what is pinned here is convention rather than mathematics.

#include <doctest/doctest.h>

#include <algorithm>
#include <string>

#include "cad/command/CommandExecutor.hpp"
#include "cad/command/StandardCommands.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/DimensionEntity.hpp"
#include "cad/db/HatchEntity.hpp"
#include "cad/io/DxfReader.hpp"
#include "cad/io/DxfWriter.hpp"
#include "test_helpers.hpp"

using namespace cad;

namespace {

[[nodiscard]] DimStyle metricStyle() {
    DimStyle style;
    style.name = "Metric";
    return style;
}

/// A rotated linear dimension measuring 100 along X, with its line 20 above.
[[nodiscard]] DimensionEntity linearDimension() {
    DimensionEntity dimension;
    dimension.type = DimensionType::Rotated;
    dimension.point13 = Vec3{0.0, 0.0, 0.0};
    dimension.point14 = Vec3{100.0, 0.0, 0.0};
    dimension.definitionPoint = Vec3{0.0, 20.0, 0.0};
    dimension.rotation = 0.0;
    return dimension;
}

/// A document with the built-in commands registered, for driving them.
struct CommandHarness {
    std::unique_ptr<Database> db = Database::createEmpty();
    TransactionManager transactions{*db};
    std::unique_ptr<CommandRegistry> registry = createStandardRegistry();
    SimpleSelection selection;
    CommandExecutor executor{*db, transactions, *registry, selection};

    [[nodiscard]] std::vector<Entity*> entities() { return db->entitiesIn(db->modelSpace()); }

    template <typename T>
    [[nodiscard]] T* only() {
        for (Entity* entity : entities()) {
            if (auto* typed = dynamic_cast<T*>(entity)) {
                return typed;
            }
        }
        return nullptr;
    }
};

[[nodiscard]] InputValue point(double x, double y) {
    return InputValue::makePoint(Vec3{x, y, 0.0});
}

/// A square boundary path, counter-clockwise.
[[nodiscard]] HatchBoundaryPath squarePath(double x0, double y0, double x1, double y1,
                                           bool external) {
    HatchBoundaryPath path;
    path.flags.isPolyline = true;
    path.flags.isExternal = external;
    path.flags.isOutermost = external;
    path.closed = true;
    path.vertices.push_back(PolylineVertex{Vec3{x0, y0, 0.0}});
    path.vertices.push_back(PolylineVertex{Vec3{x1, y0, 0.0}});
    path.vertices.push_back(PolylineVertex{Vec3{x1, y1, 0.0}});
    path.vertices.push_back(PolylineVertex{Vec3{x0, y1, 0.0}});
    return path;
}

}  // namespace

TEST_SUITE("Dimension formatting") {
    TEST_CASE("decimal measurements honour precision and zero suppression") {
        DimStyle style = metricStyle();
        CHECK(formatLinearMeasurement(style, 42.5) == "42.50");

        style.linearPrecision = 0;
        CHECK(formatLinearMeasurement(style, 42.5) == "43");

        style.linearPrecision = 4;
        style.linearZeroSuppression.suppressTrailing = true;
        CHECK(formatLinearMeasurement(style, 42.5) == "42.5");
        // A whole number loses its decimal point entirely, not just its zeros.
        CHECK(formatLinearMeasurement(style, 42.0) == "42");

        style.linearZeroSuppression.suppressLeading = true;
        CHECK(formatLinearMeasurement(style, 0.5) == ".5");
        // Only a *lone* leading zero goes; 10.5 must not become 1.5.
        CHECK(formatLinearMeasurement(style, 10.5) == "10.5");
    }

    TEST_CASE("the decimal separator is a style setting, not the machine's locale") {
        DimStyle style = metricStyle();
        style.decimalSeparator = ',';
        CHECK(formatLinearMeasurement(style, 42.5) == "42,50");
    }

    TEST_CASE("architectural measurements are feet and fractional inches") {
        DimStyle style = metricStyle();
        style.linearFormat = LinearUnitsFormat::Architectural;
        style.linearPrecision = 4;  // sixteenths

        CHECK(formatLinearMeasurement(style, 42.5) == "3'-6 1/2\"");
        // The fraction reduces: 8/16 is written 1/2.
        CHECK(formatLinearMeasurement(style, 12.0) == "1'");
        CHECK(formatLinearMeasurement(style, 6.25) == "6 1/4\"");

        // Rounding must carry into the feet rather than producing 0'-12".
        CHECK(formatLinearMeasurement(style, 11.99999) == "1'");
    }

    TEST_CASE("engineering measurements are feet and decimal inches") {
        DimStyle style = metricStyle();
        style.linearFormat = LinearUnitsFormat::Engineering;
        style.linearPrecision = 2;
        CHECK(formatLinearMeasurement(style, 42.5) == "3'-6.50\"");
    }

    TEST_CASE("DIMLFAC scales the written value without moving the geometry") {
        // The reason it exists: a detail drawn at 1:5 is dimensioned in
        // real-world units.
        DimStyle style = metricStyle();
        style.linearScale = 5.0;
        style.linearPrecision = 1;
        CHECK(formatLinearMeasurement(style, 20.0) == "100.0");
    }

    TEST_CASE("DIMRND rounds to an increment") {
        DimStyle style = metricStyle();
        style.roundOff = 0.25;
        style.linearPrecision = 2;
        CHECK(formatLinearMeasurement(style, 42.6) == "42.50");
        CHECK(formatLinearMeasurement(style, 42.7) == "42.75");
    }

    TEST_CASE("DIMPOST is a suffix, or a template when it contains <>") {
        // The asymmetry is other CAD software's, and files depend on it.
        DimStyle style = metricStyle();
        style.linearPrecision = 0;

        style.linearPostfix = "mm";
        CHECK(formatLinearMeasurement(style, 42.0) == "42mm");

        style.linearPostfix = "<> mm";
        CHECK(formatLinearMeasurement(style, 42.0) == "42 mm");

        style.linearPostfix = "approx <>";
        CHECK(formatLinearMeasurement(style, 42.0) == "approx 42");
    }

    TEST_CASE("angular measurements follow their own format") {
        DimStyle style = metricStyle();
        style.angularPrecision = 0;

        CHECK(formatAngularMeasurement(style, degToRad(90.0)) == "90\xC2\xB0");

        style.angularFormat = AngularUnitsFormat::DegreesMinutesSeconds;
        CHECK(formatAngularMeasurement(style, degToRad(45.5)) == "45\xC2\xB0" "30'0\"");

        style.angularFormat = AngularUnitsFormat::Radians;
        style.angularPrecision = 3;
        CHECK(formatAngularMeasurement(style, 1.5) == "1.500r");

        style.angularFormat = AngularUnitsFormat::Gradians;
        style.angularPrecision = 0;
        CHECK(formatAngularMeasurement(style, degToRad(90.0)) == "100g");
    }

    TEST_CASE("surveyor bearings are quadrant-relative") {
        DimStyle style = metricStyle();
        style.angularFormat = AngularUnitsFormat::SurveyorUnits;
        style.angularPrecision = 0;

        // Measured away from the north-south axis, never past 90 degrees.
        CHECK(formatAngularMeasurement(style, degToRad(45.0)) == "N 45\xC2\xB0 E");
        CHECK(formatAngularMeasurement(style, degToRad(135.0)) == "N 45\xC2\xB0 W");
        CHECK(formatAngularMeasurement(style, degToRad(225.0)) == "S 45\xC2\xB0 W");
        CHECK(formatAngularMeasurement(style, degToRad(315.0)) == "S 45\xC2\xB0 E");

        // The cardinal directions carry no angle at all.
        CHECK(formatAngularMeasurement(style, 0.0) == "E");
        CHECK(formatAngularMeasurement(style, degToRad(90.0)) == "N");
    }

    TEST_CASE("DIMZIN packs feet/inches modes and decimal flags into one field") {
        // The low two bits are an enumeration, not two independent flags — the
        // trap when reading the DXF reference.
        for (const int dimzin : {0, 1, 2, 3, 4, 8, 12, 5, 9, 13, 6, 10, 14}) {
            const ZeroSuppression zeros = ZeroSuppression::fromDimzin(dimzin);
            CHECK(zeros.toDimzin() == dimzin);
        }
    }
}

TEST_SUITE("Dimension measurement") {
    TEST_CASE("a rotated dimension measures along its axis, not between its points") {
        // The single most common way to get a rotated dimension wrong.
        DimensionEntity dimension = linearDimension();
        dimension.point14 = Vec3{100.0, 60.0, 0.0};
        dimension.rotation = 0.0;
        CHECK(dimension.measure() == doctest::Approx(100.0));

        dimension.rotation = kPi / 2.0;
        CHECK(dimension.measure() == doctest::Approx(60.0));
    }

    TEST_CASE("an aligned dimension measures point to point") {
        DimensionEntity dimension = linearDimension();
        dimension.type = DimensionType::Aligned;
        dimension.point14 = Vec3{30.0, 40.0, 0.0};
        CHECK(dimension.measure() == doctest::Approx(50.0));
    }

    TEST_CASE("radius and diameter read their own definition points") {
        DimensionEntity radius;
        radius.type = DimensionType::Radius;
        radius.definitionPoint = Vec3{10.0, 10.0, 0.0};  // centre
        radius.point15 = Vec3{35.0, 10.0, 0.0};          // on the circumference
        CHECK(radius.measure() == doctest::Approx(25.0));
        CHECK_POINT(radius.arcCenter(), (Vec3{10.0, 10.0, 0.0}));

        DimensionEntity diameter;
        diameter.type = DimensionType::Diameter;
        // Both points are on the circumference; the centre is not stored.
        diameter.definitionPoint = Vec3{-25.0, 0.0, 0.0};
        diameter.point15 = Vec3{25.0, 0.0, 0.0};
        CHECK(diameter.measure() == doctest::Approx(50.0));
        CHECK_POINT(diameter.arcCenter(), (Vec3{0.0, 0.0, 0.0}));
    }

    TEST_CASE("a three-point angular dimension measures the counter-clockwise sweep") {
        DimensionEntity dimension;
        dimension.type = DimensionType::Angular3Point;
        dimension.point15 = Vec3::zero();          // vertex
        dimension.point13 = Vec3{10.0, 0.0, 0.0};  // first arm
        dimension.point14 = Vec3{0.0, 10.0, 0.0};  // second arm
        dimension.definitionPoint = Vec3{5.0, 0.0, 0.0};
        CHECK(dimension.measure() == doctest::Approx(kPi / 2.0));

        // Swapping the arms measures the reflex angle, not the same angle.
        std::swap(dimension.point13, dimension.point14);
        CHECK(dimension.measure() == doctest::Approx(3.0 * kPi / 2.0));
    }

    TEST_CASE("a two-line angular dimension finds its own vertex") {
        DimensionEntity dimension;
        dimension.type = DimensionType::Angular2Line;
        // First line along X, second along Y; they meet at the origin.
        dimension.point13 = Vec3{-10.0, 0.0, 0.0};
        dimension.point14 = Vec3{10.0, 0.0, 0.0};
        dimension.point15 = Vec3{0.0, -10.0, 0.0};
        dimension.definitionPoint = Vec3{0.0, 10.0, 0.0};
        dimension.point16 = Vec3{5.0, 5.0, 0.0};

        CHECK_POINT(dimension.arcCenter(), (Vec3{0.0, 0.0, 0.0}));
        CHECK(dimension.measure() == doctest::Approx(kPi / 2.0));
    }

    TEST_CASE("an ordinate dimension measures one axis from the datum, signed") {
        DimensionEntity dimension;
        dimension.type = DimensionType::Ordinate;
        dimension.definitionPoint = Vec3{100.0, 200.0, 0.0};  // the datum
        dimension.point13 = Vec3{130.0, 180.0, 0.0};
        dimension.point14 = Vec3{130.0, 250.0, 0.0};

        dimension.isXDatum = true;
        CHECK(dimension.measure() == doctest::Approx(30.0));
        dimension.isXDatum = false;
        // Negative is a real answer here, not an error.
        CHECK(dimension.measure() == doctest::Approx(-20.0));
    }
}

TEST_SUITE("Dimension text") {
    TEST_CASE("the three override states are distinct") {
        const DimStyle style = metricStyle();
        DimensionEntity dimension = linearDimension();

        // Absent means "write the measurement".
        CHECK(dimension.measurementText(style) == "100.00");

        // A single space means "write nothing", and is not the same as absent.
        dimension.textOverride = " ";
        CHECK(dimension.measurementText(style).empty());

        // Anything else replaces, with <> standing in for the measurement.
        dimension.textOverride = "TYP.";
        CHECK(dimension.measurementText(style) == "TYP.");

        dimension.textOverride = "<> TYP.";
        CHECK(dimension.measurementText(style) == "100.00 TYP.");

        dimension.textOverride = "<> x <>";
        CHECK(dimension.measurementText(style) == "100.00 x 100.00");
    }

    TEST_CASE("radial dimensions carry their conventional prefix") {
        DimStyle style = metricStyle();
        style.linearPrecision = 0;

        DimensionEntity radius;
        radius.type = DimensionType::Radius;
        radius.definitionPoint = Vec3::zero();
        radius.point15 = Vec3{25.0, 0.0, 0.0};
        // Without the R a radius is indistinguishable from a diameter.
        CHECK(radius.measurementText(style) == "R25");

        DimensionEntity diameter;
        diameter.type = DimensionType::Diameter;
        diameter.definitionPoint = Vec3{-25.0, 0.0, 0.0};
        diameter.point15 = Vec3{25.0, 0.0, 0.0};
        CHECK(diameter.measurementText(style) == "\xE2\x8C\x80" "50");
    }

    TEST_CASE("text is never upside down") {
        const DimStyle style = metricStyle();
        DimensionEntity dimension = linearDimension();

        dimension.rotation = degToRad(200.0);
        dimension.point14 = Vec3{-100.0, 0.0, 0.0};
        const DimensionGeometry geometry = dimension.generate(style);

        // Past the vertical the text flips rather than inverting; otherwise
        // half the dimensions in a rotated drawing are unreadable.
        const double angle = normalizeAngle(geometry.textRotation);
        CHECK((angle <= kPi / 2.0 + 1e-9 || angle >= 3.0 * kPi / 2.0 - 1e-9));
    }
}

TEST_SUITE("Dimension geometry") {
    TEST_CASE("a linear dimension produces extension lines, a dimension line and two arrows") {
        const DimStyle style = metricStyle();
        const DimensionEntity dimension = linearDimension();
        const DimensionGeometry geometry = dimension.generate(style);

        REQUIRE(geometry.lines.size() == 3);
        CHECK(geometry.arrowheads.size() == 2);
        CHECK(geometry.text == "100.00");

        // Extension lines start clear of the measured geometry by DIMEXO and
        // run past the dimension line by DIMEXE.
        CHECK_POINT(geometry.lines[0].start, (Vec3{0.0, style.extensionLineOffset, 0.0}));
        CHECK_POINT(geometry.lines[0].end,
                    (Vec3{0.0, 20.0 + style.extensionLineExtend, 0.0}));
        CHECK_POINT(geometry.lines[1].start, (Vec3{100.0, style.extensionLineOffset, 0.0}));

        // The dimension line spans the projections of the measured points.
        CHECK_POINT(geometry.lines[2].start, (Vec3{0.0, 20.0, 0.0}));
        CHECK_POINT(geometry.lines[2].end, (Vec3{100.0, 20.0, 0.0}));
    }

    TEST_CASE("a rotated dimension projects its points onto its own axis") {
        const DimStyle style = metricStyle();
        DimensionEntity dimension = linearDimension();
        // The second point is off-axis; the dimension line must still be
        // horizontal and must span only the X difference.
        dimension.point14 = Vec3{100.0, 60.0, 0.0};

        const DimensionGeometry geometry = dimension.generate(style);
        const LineSegment& dimLine = geometry.lines.back();
        CHECK(dimLine.start.y == doctest::Approx(dimLine.end.y));
        CHECK(std::abs(dimLine.end.x - dimLine.start.x) == doctest::Approx(100.0));
    }

    TEST_CASE("arrows flip outside when the dimension is too small to hold them") {
        const DimStyle style = metricStyle();
        DimensionEntity dimension = linearDimension();
        dimension.point14 = Vec3{2.0, 0.0, 0.0};  // 2 units, arrows are 2.5

        const DimensionGeometry geometry = dimension.generate(style);
        const LineSegment& dimLine = geometry.lines.back();
        // The line grows past the extension lines to carry the arrows, rather
        // than the arrows overlapping into a blob.
        CHECK(dimLine.start.x < 0.0);
        CHECK(dimLine.end.x > 2.0);
    }

    TEST_CASE("default text sits above the dimension line, on the far side from the geometry") {
        const DimStyle style = metricStyle();
        DimensionEntity dimension = linearDimension();

        Vec3 above = dimension.defaultTextPosition(style);
        CHECK(above.x == doctest::Approx(50.0));
        CHECK(above.y > 20.0);

        // With the dimension line *below* the geometry, "above" must follow it
        // rather than staying on a fixed side.
        dimension.definitionPoint = Vec3{0.0, -20.0, 0.0};
        above = dimension.defaultTextPosition(style);
        CHECK(above.y < -20.0);
    }

    TEST_CASE("a dragged text position wins over the default") {
        const DimStyle style = metricStyle();
        DimensionEntity dimension = linearDimension();
        dimension.textPositionIsUserDefined = true;
        dimension.textMidPoint = Vec3{500.0, 500.0, 0.0};

        CHECK_POINT(dimension.generate(style).textPosition, (Vec3{500.0, 500.0, 0.0}));
    }

    TEST_CASE("an angular dimension draws an arc between its arms") {
        const DimStyle style = metricStyle();
        DimensionEntity dimension;
        dimension.type = DimensionType::Angular3Point;
        dimension.point15 = Vec3::zero();
        dimension.point13 = Vec3{10.0, 0.0, 0.0};
        dimension.point14 = Vec3{0.0, 10.0, 0.0};
        dimension.definitionPoint = Vec3{15.0, 0.0, 0.0};

        const DimensionGeometry geometry = dimension.generate(style);
        REQUIRE(geometry.arcs.size() == 1);
        CHECK(geometry.arcs[0].radius == doctest::Approx(15.0));
        CHECK(geometry.arrowheads.size() == 2);
        CHECK(geometry.text == "90\xC2\xB0");
    }

    TEST_CASE("a radius dimension draws a leader and a centre mark") {
        DimStyle style = metricStyle();
        style.centerMarkSize = 2.5;

        DimensionEntity dimension;
        dimension.type = DimensionType::Radius;
        dimension.definitionPoint = Vec3::zero();
        dimension.point15 = Vec3{25.0, 0.0, 0.0};

        const DimensionGeometry geometry = dimension.generate(style);
        // The leader plus the two centre-mark strokes.
        CHECK(geometry.lines.size() == 3);
        CHECK(geometry.arrowheads.size() == 1);

        // A zero centre mark draws none, which is what the sign convention
        // means and not merely a very small one.
        style.centerMarkSize = 0.0;
        CHECK(dimension.generate(style).lines.size() == 1);
    }

    TEST_CASE("bounds cover the drawn geometry, not just the definition points") {
        const DimensionEntity dimension = linearDimension();
        const Box3 bounds = dimension.bounds();
        REQUIRE_FALSE(bounds.isEmpty());
        // The extension lines reach past the dimension line.
        CHECK(bounds.max.y > 20.0);
        CHECK(bounds.min.x <= 0.0);
        CHECK(bounds.max.x >= 100.0);
    }

    TEST_CASE("a dimension is pickable through its generated curves") {
        const DimensionEntity dimension = linearDimension();
        const std::vector<CurvePtr> curves = dimension.toCurves();
        CHECK(curves.size() == 3);
        // Clicking the dimension line selects it.
        CHECK(dimension.distanceToPoint(Vec3{50.0, 20.0, 0.0}) == doctest::Approx(0.0));
    }
}

TEST_SUITE("Dimension editing") {
    TEST_CASE("rotating a dimension rotates its measurement axis with it") {
        DimensionEntity dimension = linearDimension();
        dimension.point14 = Vec3{100.0, 60.0, 0.0};
        REQUIRE(dimension.measure() == doctest::Approx(100.0));

        dimension.transformBy(Mat4::rotationZ(kPi / 2.0));
        // An axis left behind would start measuring the wrong distance.
        CHECK(dimension.measure() == doctest::Approx(100.0));
        CHECK(dimension.rotation == doctest::Approx(kPi / 2.0));
    }

    TEST_CASE("scaling a dimension scales what it reads") {
        DimensionEntity dimension = linearDimension();
        dimension.transformBy(Mat4::scaling(2.0));
        CHECK(dimension.measure() == doctest::Approx(200.0));
    }

    TEST_CASE("dragging the text grip marks the position as user-defined") {
        DimensionEntity dimension = linearDimension();
        const std::vector<GripPoint> grips = dimension.gripPoints();
        REQUIRE_FALSE(grips.empty());

        const GripPoint& textGrip = grips.back();
        REQUIRE(textGrip.index == 100);
        dimension.moveGrip(textGrip, Vec3{60.0, 40.0, 0.0});

        CHECK(dimension.textPositionIsUserDefined);
        CHECK_POINT(dimension.textMidPoint, (Vec3{60.0, 40.0, 0.0}));
    }

    TEST_CASE("clone and restore round-trip every field") {
        DimensionEntity dimension = linearDimension();
        dimension.type = DimensionType::Aligned;
        dimension.textOverride = "<> TYP.";
        dimension.obliqueAngle = 0.2;
        dimension.leaderLength = 7.0;
        dimension.isXDatum = true;
        dimension.textPositionIsUserDefined = true;
        dimension.attachmentPoint = MTextAttachment::BottomRight;

        const DbObjectPtr snapshot = dimension.clone();
        DimensionEntity restored;
        restored.restoreFrom(*snapshot);

        CHECK(restored.type == DimensionType::Aligned);
        CHECK(restored.textOverride == "<> TYP.");
        CHECK(restored.obliqueAngle == doctest::Approx(0.2));
        CHECK(restored.leaderLength == doctest::Approx(7.0));
        CHECK(restored.isXDatum);
        CHECK(restored.textPositionIsUserDefined);
        CHECK(restored.attachmentPoint == MTextAttachment::BottomRight);
        CHECK_POINT(restored.point14, (Vec3{100.0, 0.0, 0.0}));
    }

    TEST_CASE("a dimension resolves its style through the database") {
        auto db = Database::createEmpty();
        // Every drawing has a Standard dimension style, as it has layer 0.
        REQUIRE(db->findDimStyle(kDimStyleStandard) != nullptr);

        DimStyle& fine = db->ensureDimStyle("Fine");
        fine.linearPrecision = 4;

        auto dimension = std::make_unique<DimensionEntity>();
        dimension->styleHandle = fine.handle;
        DimensionEntity& added =
            static_cast<DimensionEntity&>(db->addToModelSpace(std::move(dimension)));

        CHECK(db->dimStyleFor(added).name == "Fine");

        // A dangling style handle falls back rather than failing: a drawing
        // with a damaged style table must still display.
        added.styleHandle = Handle{999999};
        CHECK(db->dimStyleFor(added).name == kDimStyleStandard);
    }
}

TEST_SUITE("Dimension style overrides") {
    /// The XData other CAD software writes for a per-dimension override.
    [[nodiscard]] std::vector<XDataItem> dstyle(
        const std::vector<std::pair<int, XDataValue>>& overrides) {
        std::vector<XDataItem> items;
        items.push_back({1000, std::string{"DSTYLE"}});
        items.push_back({1002, std::string{"{"}});
        for (const auto& [variable, value] : overrides) {
            items.push_back({1070, static_cast<std::int64_t>(variable)});
            // Reals go in 1040 and integers in 1070, as the format requires.
            items.push_back(
                {std::holds_alternative<double>(value) ? 1040 : 1070, value});
        }
        items.push_back({1002, std::string{"}"}});
        return items;
    }

    TEST_CASE("a dimension with no overrides is left alone") {
        DimensionEntity dimension = linearDimension();
        CHECK_FALSE(hasDimStyleOverrides(dimension));

        DimStyle style = metricStyle();
        CHECK(applyDimStyleOverrides(style, dimension) == 0);
        CHECK(style.arrowSize == doctest::Approx(2.5));
    }

    TEST_CASE("overrides replace the style's values") {
        // Nudging one dimension's arrow size or text height is routine, and a
        // reader that ignores it draws that dimension visibly wrong while
        // insisting it read the file correctly.
        DimensionEntity dimension = linearDimension();
        dimension.setXData("ACAD", dstyle({
                                       {41, 5.0},                              // DIMASZ
                                       {140, 7.5},                             // DIMTXT
                                       {271, static_cast<std::int64_t>(4)},     // DIMDEC
                                       {75, static_cast<std::int64_t>(1)},      // DIMSE1
                                   }));
        REQUIRE(hasDimStyleOverrides(dimension));

        DimStyle style = metricStyle();
        CHECK(applyDimStyleOverrides(style, dimension) == 4);
        CHECK(style.arrowSize == doctest::Approx(5.0));
        CHECK(style.textHeight == doctest::Approx(7.5));
        CHECK(style.linearPrecision == 4);
        CHECK(style.suppressExtensionLine1);
        // Untouched variables keep the style's value.
        CHECK(style.textGap == doctest::Approx(0.625));
    }

    TEST_CASE("an override changes what the dimension draws") {
        DimensionEntity dimension = linearDimension();
        const DimStyle base = metricStyle();
        const DimensionGeometry plain = dimension.generate(base);

        // DIMSE1 suppresses the first extension line, so one line disappears.
        dimension.setXData("ACAD", dstyle({{75, static_cast<std::int64_t>(1)}}));
        DimStyle overridden = metricStyle();
        applyDimStyleOverrides(overridden, dimension);
        const DimensionGeometry suppressed = dimension.generate(overridden);

        CHECK(suppressed.lines.size() == plain.lines.size() - 1);
    }

    TEST_CASE("an unmodelled variable is skipped, not guessed at") {
        DimensionEntity dimension = linearDimension();
        dimension.setXData("ACAD", dstyle({
                                       {41, 5.0},                            // known
                                       {47, 0.5},                            // DIMTP, not modelled
                                       {140, 3.0},                           // known
                                   }));

        DimStyle style = metricStyle();
        // Two applied, one skipped — and the XData is untouched, so the
        // unmodelled one still round-trips to the file.
        CHECK(applyDimStyleOverrides(style, dimension) == 2);
        CHECK(style.arrowSize == doctest::Approx(5.0));
        CHECK(style.textHeight == doctest::Approx(3.0));
        REQUIRE(dimension.getXData("ACAD") != nullptr);
        CHECK(dimension.getXData("ACAD")->size() == 2 + 3 * 2 + 1);
    }

    TEST_CASE("XData that is not a DSTYLE block is ignored") {
        DimensionEntity dimension = linearDimension();
        dimension.setXData("ACME", dstyle({{41, 9.0}}));
        dimension.setXData("ACAD", {{1000, std::string{"SOMETHING_ELSE"}},
                                    {1070, static_cast<std::int64_t>(41)},
                                    {1040, 9.0}});

        // Only ACAD's DSTYLE block means style overrides. Reading any 1070/1040
        // pair as one would corrupt every third-party annotation in a drawing.
        CHECK_FALSE(hasDimStyleOverrides(dimension));
        DimStyle style = metricStyle();
        CHECK(applyDimStyleOverrides(style, dimension) == 0);
        CHECK(style.arrowSize == doctest::Approx(2.5));
    }

    TEST_CASE("the database resolves the effective style") {
        auto db = Database::createEmpty();
        DimStyle& named = db->ensureDimStyle("Coarse");
        named.arrowSize = 4.0;
        named.textHeight = 4.0;

        auto owned = std::make_unique<DimensionEntity>();
        owned->styleHandle = named.handle;
        owned->setXData("ACAD", dstyle({{41, 1.5}}));
        const auto& dimension =
            static_cast<DimensionEntity&>(db->addToModelSpace(std::move(owned)));

        // The named style is unchanged; the effective one carries the override.
        CHECK(db->dimStyleFor(dimension).arrowSize == doctest::Approx(4.0));
        const DimStyle effective = db->effectiveDimStyle(dimension);
        CHECK(effective.arrowSize == doctest::Approx(1.5));
        CHECK(effective.textHeight == doctest::Approx(4.0));
        CHECK(effective.name == "Coarse");
    }

    TEST_CASE("overrides survive a DXF round trip") {
        auto db = Database::createEmpty();
        auto owned = std::make_unique<DimensionEntity>();
        owned->point13 = Vec3::zero();
        owned->point14 = Vec3{100.0, 0.0, 0.0};
        owned->definitionPoint = Vec3{0.0, 20.0, 0.0};
        owned->setXData("ACAD", dstyle({{41, 5.0}, {140, 7.5}}));
        db->addToModelSpace(std::move(owned));

        io::DxfWriter writer;
        const io::DxfReadResult result = io::readDxf(writer.write(*db));
        const auto entities = result.database->entitiesIn(result.database->modelSpace());
        REQUIRE(entities.size() == 1);
        const auto* read = dynamic_cast<const DimensionEntity*>(entities[0]);
        REQUIRE(read != nullptr);

        const DimStyle effective = result.database->effectiveDimStyle(*read);
        CHECK(effective.arrowSize == doctest::Approx(5.0));
        CHECK(effective.textHeight == doctest::Approx(7.5));
    }
}

TEST_SUITE("Hatch boundaries") {
    TEST_CASE("a solid hatch reports what is inside it") {
        HatchEntity hatch;
        hatch.paths.push_back(squarePath(0.0, 0.0, 10.0, 10.0, true));

        CHECK(hatch.contains(Vec3{5.0, 5.0, 0.0}));
        CHECK_FALSE(hatch.contains(Vec3{15.0, 5.0, 0.0}));
        CHECK_FALSE(hatch.contains(Vec3{-1.0, 5.0, 0.0}));

        // Picked by area, so clicking the middle of a fill selects it.
        CHECK(hatch.distanceToPoint(Vec3{5.0, 5.0, 0.0}) == doctest::Approx(0.0));
        CHECK(hatch.distanceToPoint(Vec3{13.0, 5.0, 0.0}) == doctest::Approx(3.0));
    }

    TEST_CASE("an island hollows the fill rather than filling twice") {
        HatchEntity hatch;
        hatch.paths.push_back(squarePath(0.0, 0.0, 20.0, 20.0, true));
        hatch.paths.push_back(squarePath(5.0, 5.0, 15.0, 15.0, false));

        CHECK(hatch.contains(Vec3{2.0, 2.0, 0.0}));
        CHECK_FALSE(hatch.contains(Vec3{10.0, 10.0, 0.0}));

        // Ignore style fills straight over the island.
        hatch.style = HatchStyle::Ignore;
        CHECK(hatch.contains(Vec3{10.0, 10.0, 0.0}));
    }

    TEST_CASE("a bulged polyline boundary stays curved") {
        HatchBoundaryPath path;
        path.flags.isPolyline = true;
        path.closed = true;
        // A half-circle span: bulge 1 is a 180 degree arc.
        path.vertices.push_back(PolylineVertex{Vec3{0.0, 0.0, 0.0}, 1.0});
        path.vertices.push_back(PolylineVertex{Vec3{10.0, 0.0, 0.0}});

        HatchEntity hatch;
        hatch.paths.push_back(path);

        const Box3 bounds = hatch.bounds();
        REQUIRE_FALSE(bounds.isEmpty());
        // A positive bulge sweeps counter-clockwise, so running left-to-right
        // the arc goes *below* the chord. Reading it as "bulges upward" is the
        // classic bulge-sign mistake.
        CHECK(bounds.min.y == doctest::Approx(-5.0));
        CHECK(bounds.max.y == doctest::Approx(0.0));
    }

    TEST_CASE("an edge-list boundary accepts lines and arcs") {
        HatchBoundaryPath path;
        path.edges.push_back(HatchEdge::line(Vec3{0.0, 0.0, 0.0}, Vec3{10.0, 0.0, 0.0}));
        path.edges.push_back(HatchEdge::arc(Vec3{5.0, 0.0, 0.0}, 5.0, 0.0, kPi, true));

        HatchEntity hatch;
        hatch.paths.push_back(path);

        CHECK(hatch.toCurves().size() == 2);
        const Box3 bounds = hatch.bounds();
        CHECK(bounds.max.y == doctest::Approx(5.0));
    }

    TEST_CASE("the counter-clockwise flag reverses traversal, not geometry") {
        // Group 73 says which way the loop runs through the edge. The shape is
        // the same quarter arc either way; only its start and end swap. An
        // implementation that mirrored the arc instead would close the loop in
        // the wrong place.
        const HatchEdge forward = HatchEdge::arc(Vec3::zero(), 5.0, 0.0, kPi / 2.0, true);
        const CurvePtr forwardCurve = forward.toCurve();
        REQUIRE(forwardCurve != nullptr);
        CHECK_POINT_EPS(forwardCurve->startPoint(), (Vec3{5.0, 0.0, 0.0}), 1e-9);
        CHECK_POINT_EPS(forwardCurve->endPoint(), (Vec3{0.0, 5.0, 0.0}), 1e-9);

        const HatchEdge backward = HatchEdge::arc(Vec3::zero(), 5.0, 0.0, kPi / 2.0, false);
        const CurvePtr backwardCurve = backward.toCurve();
        REQUIRE(backwardCurve != nullptr);
        CHECK_POINT_EPS(backwardCurve->startPoint(), (Vec3{0.0, 5.0, 0.0}), 1e-9);
        CHECK_POINT_EPS(backwardCurve->endPoint(), (Vec3{5.0, 0.0, 0.0}), 1e-9);

        // Same shape, so the same bounds.
        CHECK(forwardCurve->bounds().max.y == doctest::Approx(backwardCurve->bounds().max.y));
    }

    TEST_CASE("a spline edge is approximated from its fit points, not dropped") {
        HatchEdge edge;
        edge.kind = HatchEdge::Kind::Spline;
        edge.degree = 3;
        edge.fitPoints = {Vec3{0.0, 0.0, 0.0}, Vec3{5.0, 5.0, 0.0}, Vec3{10.0, 0.0, 0.0}};

        const CurvePtr curve = edge.toCurve();
        REQUIRE(curve != nullptr);
        CHECK_POINT(curve->startPoint(), (Vec3{0.0, 0.0, 0.0}));
        CHECK_POINT(curve->endPoint(), (Vec3{10.0, 0.0, 0.0}));
    }

    TEST_CASE("path flags round-trip through their DXF bit field") {
        for (int flags = 0; flags < 32; ++flags) {
            CHECK(HatchPathFlags::fromDxf(flags).toDxf() == flags);
        }
    }
}

TEST_SUITE("Hatch patterns") {
    TEST_CASE("SOLID is a fill, not a very fine pattern") {
        const auto solid = standardHatchPattern("SOLID");
        REQUIRE(solid.has_value());
        CHECK(solid->isSolid);
        CHECK(solid->lines.empty());
    }

    TEST_CASE("a known pattern is found case-insensitively") {
        const auto pattern = standardHatchPattern("ansi31");
        REQUIRE(pattern.has_value());
        CHECK_FALSE(pattern->isSolid);
        REQUIRE(pattern->lines.size() == 1);
        CHECK(pattern->lines[0].angle == doctest::Approx(kPi / 4.0));

        CHECK_FALSE(standardHatchPattern("NOT_A_PATTERN").has_value());
    }

    TEST_CASE("pattern lines are generated inside the boundary and nowhere else") {
        HatchEntity hatch;
        hatch.paths.push_back(squarePath(0.0, 0.0, 100.0, 100.0, true));
        hatch.pattern = *standardHatchPattern("ANSI31");
        hatch.pattern.scale = 10.0;

        const std::vector<LineSegment> lines = hatch.generateLines(hatch.bounds());
        REQUIRE_FALSE(lines.empty());

        for (const LineSegment& line : lines) {
            const Vec3 middle = line.start.midpoint(line.end);
            INFO("segment midpoint ", middle.x, ",", middle.y);
            CHECK(hatch.contains(middle));
        }
    }

    TEST_CASE("an island is left unhatched") {
        HatchEntity hatch;
        hatch.paths.push_back(squarePath(0.0, 0.0, 100.0, 100.0, true));
        hatch.paths.push_back(squarePath(30.0, 30.0, 70.0, 70.0, false));
        hatch.pattern = *standardHatchPattern("ANSI31");
        hatch.pattern.scale = 5.0;

        for (const LineSegment& line : hatch.generateLines(hatch.bounds())) {
            const Vec3 middle = line.start.midpoint(line.end);
            const bool insideIsland = middle.x > 30.0 && middle.x < 70.0 && middle.y > 30.0 &&
                                      middle.y < 70.0;
            CHECK_FALSE(insideIsland);
        }
    }

    TEST_CASE("clipping to the view bounds what is generated") {
        HatchEntity hatch;
        hatch.paths.push_back(squarePath(0.0, 0.0, 1000.0, 1000.0, true));
        hatch.pattern = *standardHatchPattern("ANSI31");
        hatch.pattern.scale = 10.0;

        const std::size_t whole = hatch.generateLines(hatch.bounds()).size();
        const std::size_t corner =
            hatch.generateLines(Box3::fromCorners(Vec3::zero(), Vec3{100.0, 100.0, 0.0})).size();

        // This is what keeps a site-plan-sized hatch affordable when only a
        // doorway is on screen.
        REQUIRE(whole > 0);
        CHECK(corner < whole);
    }

    TEST_CASE("a dashed pattern leaves gaps") {
        HatchEntity hatch;
        hatch.paths.push_back(squarePath(0.0, 0.0, 100.0, 100.0, true));
        hatch.pattern.isSolid = false;
        hatch.pattern.name = "DASHED";

        HatchPatternLine dashed;
        dashed.angle = 0.0;
        dashed.offset = Vec3{0.0, 20.0, 0.0};
        dashed.dashes = {5.0, -5.0};
        hatch.pattern.lines.push_back(dashed);

        const std::vector<LineSegment> lines = hatch.generateLines(hatch.bounds());
        REQUIRE_FALSE(lines.empty());
        // Every drawn piece is one dash long, not a whole crossing.
        for (const LineSegment& line : lines) {
            CHECK(line.length() <= doctest::Approx(5.0).epsilon(1e-6));
        }
    }

    TEST_CASE("a staggered pattern still lands on the region") {
        // The offset's x component slides successive lines *along* themselves,
        // which is what makes a brick pattern stagger. A line is unchanged by
        // sliding along itself, so it must not move the drawn span — but a
        // finite ray anchored to the shifted origin walks away from the hatch a
        // little further with every line, and the pattern silently draws
        // nothing. Real drawings are full of staggered patterns; ANSI31, which
        // the other tests use, has a zero x offset and never catches it.
        HatchEntity hatch;
        hatch.paths.push_back(squarePath(0.0, 0.0, 40.0, 40.0, true));
        hatch.pattern.isSolid = false;
        hatch.pattern.name = "STAGGERED";

        HatchPatternLine staggered;
        staggered.angle = 0.0;
        // Far outside the region, and offset by a half step so no line lands
        // exactly on an edge — a line collinear with the boundary is a separate
        // question from the one this test is asking.
        staggered.basePoint = Vec3{1.0, 100.5, 0.0};
        staggered.offset = Vec3{15.0, 3.0, 0.0};  // large shift along the line
        hatch.pattern.lines.push_back(staggered);

        const std::vector<LineSegment> lines = hatch.generateLines(hatch.bounds());
        REQUIRE_FALSE(lines.empty());
        for (const LineSegment& line : lines) {
            CHECK(hatch.contains(line.start.midpoint(line.end)));
        }
    }

    TEST_CASE("a staggered dashed pattern phases from the shifted origin") {
        // The shift's *only* effect is on where the dashes fall, so two
        // adjacent lines must not have their dashes in the same places.
        HatchEntity hatch;
        hatch.paths.push_back(squarePath(0.0, 0.0, 100.0, 20.0, true));
        hatch.pattern.isSolid = false;

        HatchPatternLine brick;
        brick.angle = 0.0;
        // Rows land at y = 5, 15 inside the region, clear of both edges.
        brick.basePoint = Vec3{0.0, 5.0, 0.0};
        brick.offset = Vec3{5.0, 10.0, 0.0};
        brick.dashes = {10.0, -10.0};
        hatch.pattern.lines.push_back(brick);

        const std::vector<LineSegment> lines = hatch.generateLines(hatch.bounds());
        REQUIRE(lines.size() >= 4);

        std::vector<double> startsOnFirstRow;
        std::vector<double> startsOnSecondRow;
        for (const LineSegment& line : lines) {
            const double y = line.start.y;
            if (std::abs(y - 5.0) < 1e-6) { startsOnFirstRow.push_back(line.start.x); }
            if (std::abs(y - 15.0) < 1e-6) { startsOnSecondRow.push_back(line.start.x); }
        }
        REQUIRE_FALSE(startsOnFirstRow.empty());
        REQUIRE_FALSE(startsOnSecondRow.empty());
        std::sort(startsOnFirstRow.begin(), startsOnFirstRow.end());
        std::sort(startsOnSecondRow.begin(), startsOnSecondRow.end());
        // Offset by the shift, modulo the pattern length — the stagger.
        CHECK(std::abs(startsOnFirstRow.front() - startsOnSecondRow.front()) >
              doctest::Approx(1.0));
    }

    TEST_CASE("a degenerate pattern produces nothing rather than hanging") {
        HatchEntity hatch;
        hatch.paths.push_back(squarePath(0.0, 0.0, 100.0, 100.0, true));
        hatch.pattern.isSolid = false;

        HatchPatternLine coincident;
        // Zero perpendicular spacing describes infinitely many coincident
        // lines. Real files really do contain this.
        coincident.offset = Vec3{1.0, 0.0, 0.0};
        hatch.pattern.lines.push_back(coincident);

        CHECK(hatch.generateLines(hatch.bounds()).empty());
    }

    TEST_CASE("generation is capped so a nonsense scale cannot exhaust memory") {
        HatchEntity hatch;
        hatch.paths.push_back(squarePath(0.0, 0.0, 100.0, 100.0, true));
        hatch.pattern = *standardHatchPattern("ANSI31");
        hatch.pattern.scale = 0.001;

        const std::vector<LineSegment> lines = hatch.generateLines(hatch.bounds(), 500);
        CHECK(lines.size() <= 500);
        CHECK_FALSE(lines.empty());
    }

    TEST_CASE("double hatching adds a perpendicular family") {
        HatchEntity hatch;
        hatch.paths.push_back(squarePath(0.0, 0.0, 100.0, 100.0, true));
        hatch.pattern = *standardHatchPattern("ANSI31");
        hatch.pattern.scale = 10.0;

        const std::size_t single = hatch.generateLines(hatch.bounds()).size();
        hatch.pattern.isDouble = true;
        CHECK(hatch.generateLines(hatch.bounds()).size() > single);
    }
}

TEST_SUITE("Hatch editing") {
    TEST_CASE("transforming a hatch carries the pattern with the region") {
        HatchEntity hatch;
        hatch.paths.push_back(squarePath(0.0, 0.0, 10.0, 10.0, true));
        hatch.pattern = *standardHatchPattern("ANSI31");
        hatch.pattern.scale = 1.0;

        hatch.transformBy(Mat4::scaling(3.0));

        // Hatching that did not scale would slide across the geometry.
        CHECK(hatch.pattern.scale == doctest::Approx(3.0));
        CHECK(hatch.bounds().max.x == doctest::Approx(30.0));
    }

    TEST_CASE("clone and restore round-trip the boundary and the pattern") {
        HatchEntity hatch;
        hatch.paths.push_back(squarePath(0.0, 0.0, 10.0, 10.0, true));
        hatch.pattern = *standardHatchPattern("ANSI31");
        hatch.style = HatchStyle::Outer;
        hatch.isAssociative = true;
        hatch.seedPoints.push_back(Vec3{5.0, 5.0, 0.0});

        const DbObjectPtr snapshot = hatch.clone();
        HatchEntity restored;
        restored.restoreFrom(*snapshot);

        REQUIRE(restored.paths.size() == 1);
        CHECK(restored.paths[0].vertices.size() == 4);
        CHECK(restored.pattern.name == "ANSI31");
        CHECK(restored.style == HatchStyle::Outer);
        CHECK(restored.isAssociative);
        REQUIRE(restored.seedPoints.size() == 1);
        CHECK_POINT(restored.seedPoints[0], (Vec3{5.0, 5.0, 0.0}));
    }
}

TEST_SUITE("Annotation commands") {
    TEST_CASE("the annotation commands are registered like any other") {
        const auto registry = createStandardRegistry();
        for (const char* name : {"DIMLINEAR", "DIMALIGNED", "DIMRADIUS", "DIMDIAMETER",
                                 "DIMANGULAR", "DIMORDINATE", "HATCH"}) {
            INFO("looking up ", name);
            CHECK(registry->lookup(name) != nullptr);
        }
        CHECK(registry->lookup("DLI")->name == "DIMLINEAR");
        CHECK(registry->lookup("H")->name == "HATCH");
        CHECK(registry->byCategory().count("Dimension") == 1);
    }

    TEST_CASE("DIMLINEAR chooses its axis from the drag direction") {
        CommandHarness harness;
        REQUIRE(harness.executor.execute("DIMLINEAR").status ==
                CommandOutcome::Status::Running);
        harness.executor.supply(point(0.0, 0.0));
        harness.executor.supply(point(100.0, 40.0));
        // Dragged upwards: measures the horizontal separation.
        harness.executor.supply(point(50.0, 80.0));

        const auto* dimension = harness.only<DimensionEntity>();
        REQUIRE(dimension != nullptr);
        CHECK(dimension->type == DimensionType::Rotated);
        CHECK(dimension->rotation == doctest::Approx(0.0));
        CHECK(dimension->measure() == doctest::Approx(100.0));
    }

    TEST_CASE("DIMLINEAR dragged sideways measures the vertical separation") {
        CommandHarness harness;
        harness.executor.execute("DIMLINEAR");
        harness.executor.supply(point(0.0, 0.0));
        harness.executor.supply(point(100.0, 40.0));
        harness.executor.supply(point(150.0, 20.0));

        const auto* dimension = harness.only<DimensionEntity>();
        REQUIRE(dimension != nullptr);
        CHECK(dimension->rotation == doctest::Approx(kPi / 2.0));
        CHECK(dimension->measure() == doctest::Approx(40.0));
    }

    TEST_CASE("DIMALIGNED measures point to point whatever the drag") {
        CommandHarness harness;
        harness.executor.execute("DIMALIGNED");
        harness.executor.supply(point(0.0, 0.0));
        harness.executor.supply(point(30.0, 40.0));
        harness.executor.supply(point(0.0, 50.0));

        const auto* dimension = harness.only<DimensionEntity>();
        REQUIRE(dimension != nullptr);
        CHECK(dimension->type == DimensionType::Aligned);
        CHECK(dimension->measure() == doctest::Approx(50.0));
    }

    TEST_CASE("a dimension command previews before it commits") {
        CommandHarness harness;
        harness.executor.execute("DIMLINEAR");
        harness.executor.supply(point(0.0, 0.0));
        harness.executor.supply(point(100.0, 0.0));

        harness.executor.moveCursor(Vec3{50.0, 30.0, 0.0});
        CHECK_FALSE(harness.executor.preview().empty());
        // Preview geometry is not in the drawing until the point is picked.
        CHECK(harness.entities().empty());
    }

    TEST_CASE("DIMRADIUS reads the circle it is given") {
        CommandHarness harness;
        Entity& circle = harness.db->addToModelSpace(
            std::make_unique<CircleEntity>(Vec3{10.0, 10.0, 0.0}, 25.0));

        REQUIRE(harness.executor.execute("DIMRADIUS").status ==
                CommandOutcome::Status::Running);
        REQUIRE(harness.executor.currentRequest() != nullptr);
        CHECK(harness.executor.currentRequest()->kind == InputRequest::Kind::Entity);

        harness.executor.supply(InputValue::makeEntity(circle.handle));
        harness.executor.supply(point(60.0, 10.0));

        const auto* dimension = harness.only<DimensionEntity>();
        REQUIRE(dimension != nullptr);
        CHECK(dimension->type == DimensionType::Radius);
        CHECK(dimension->measure() == doctest::Approx(25.0));
        // The leader points the way the user dragged.
        CHECK_POINT(dimension->point15, (Vec3{35.0, 10.0, 0.0}));
    }

    TEST_CASE("DIMDIAMETER stores both chord points") {
        CommandHarness harness;
        Entity& circle =
            harness.db->addToModelSpace(std::make_unique<CircleEntity>(Vec3::zero(), 25.0));

        harness.executor.execute("DIMDIAMETER");
        harness.executor.supply(InputValue::makeEntity(circle.handle));
        harness.executor.supply(point(40.0, 0.0));

        const auto* dimension = harness.only<DimensionEntity>();
        REQUIRE(dimension != nullptr);
        CHECK(dimension->type == DimensionType::Diameter);
        CHECK(dimension->measure() == doctest::Approx(50.0));
        CHECK_POINT(dimension->arcCenter(), (Vec3{0.0, 0.0, 0.0}));
    }

    TEST_CASE("DIMRADIUS refuses something that is not round") {
        CommandHarness harness;
        Entity& line = harness.db->addToModelSpace(
            std::make_unique<LineEntity>(Vec3::zero(), Vec3{10.0, 0.0, 0.0}));

        harness.executor.execute("DIMRADIUS");
        const CommandOutcome outcome =
            harness.executor.supply(InputValue::makeEntity(line.handle));
        CHECK(outcome.status == CommandOutcome::Status::Failed);
        CHECK(harness.entities().size() == 1);  // Only the line.
    }

    TEST_CASE("DIMANGULAR takes a vertex and two arms") {
        CommandHarness harness;
        harness.executor.execute("DIMANGULAR");
        harness.executor.supply(point(0.0, 0.0));    // vertex
        harness.executor.supply(point(10.0, 0.0));   // first arm
        harness.executor.supply(point(0.0, 10.0));   // second arm
        harness.executor.supply(point(15.0, 15.0));  // arc location

        const auto* dimension = harness.only<DimensionEntity>();
        REQUIRE(dimension != nullptr);
        CHECK(dimension->type == DimensionType::Angular3Point);
        CHECK(dimension->measure() == doctest::Approx(kPi / 2.0));
    }

    TEST_CASE("HATCH builds a boundary from a closed polyline") {
        CommandHarness harness;
        const std::array<Vec3, 4> corners = {Vec3{0.0, 0.0, 0.0}, Vec3{10.0, 0.0, 0.0},
                                             Vec3{10.0, 10.0, 0.0}, Vec3{0.0, 10.0, 0.0}};
        auto polyline = std::make_unique<PolylineEntity>(
            PolylineEntity::fromPoints(std::span<const Vec3>{corners}, true));
        Entity& boundary = harness.db->addToModelSpace(std::move(polyline));

        REQUIRE(harness.executor.execute("HATCH").status == CommandOutcome::Status::Running);
        harness.executor.supply(InputValue::makeSelection({boundary.handle}));
        harness.executor.supply(InputValue::makeText("SOLID"));

        const auto* hatch = harness.only<HatchEntity>();
        REQUIRE(hatch != nullptr);
        CHECK(hatch->pattern.isSolid);
        REQUIRE(hatch->paths.size() == 1);
        // Kept as a polyline path, so the boundary is exact rather than
        // tessellated.
        CHECK(hatch->paths[0].flags.isPolyline);
        CHECK(hatch->paths[0].vertices.size() == 4);
        CHECK(hatch->paths[0].flags.isExternal);
        CHECK(hatch->isAssociative);
        REQUIRE(hatch->paths[0].sourceHandles.size() == 1);
        CHECK(hatch->paths[0].sourceHandles[0] == boundary.handle);
        CHECK(hatch->contains(Vec3{5.0, 5.0, 0.0}));
    }

    TEST_CASE("HATCH chains loose lines into a loop") {
        CommandHarness harness;
        std::vector<Handle> handles;
        const std::array<Vec3, 4> corners = {Vec3{0.0, 0.0, 0.0}, Vec3{10.0, 0.0, 0.0},
                                             Vec3{10.0, 10.0, 0.0}, Vec3{0.0, 10.0, 0.0}};
        for (std::size_t i = 0; i < corners.size(); ++i) {
            // The third edge is drawn backwards on purpose: a user's boundary
            // is rarely all drawn in the same direction.
            const Vec3& from = corners[i];
            const Vec3& to = corners[(i + 1) % corners.size()];
            auto line = i == 2 ? std::make_unique<LineEntity>(to, from)
                               : std::make_unique<LineEntity>(from, to);
            handles.push_back(harness.db->addToModelSpace(std::move(line)).handle);
        }

        harness.executor.execute("HATCH");
        harness.executor.supply(InputValue::makeSelection(handles));
        harness.executor.supply(InputValue::makeText("SOLID"));

        const auto* hatch = harness.only<HatchEntity>();
        REQUIRE(hatch != nullptr);
        REQUIRE(hatch->paths.size() == 1);
        CHECK(hatch->paths[0].edges.size() == 4);
        CHECK(hatch->contains(Vec3{5.0, 5.0, 0.0}));
        CHECK_FALSE(hatch->contains(Vec3{15.0, 5.0, 0.0}));
    }

    TEST_CASE("HATCH refuses a boundary that does not close") {
        CommandHarness harness;
        Entity& line = harness.db->addToModelSpace(
            std::make_unique<LineEntity>(Vec3::zero(), Vec3{10.0, 0.0, 0.0}));

        harness.executor.execute("HATCH");
        harness.executor.supply(InputValue::makeSelection({line.handle}));
        const CommandOutcome outcome = harness.executor.supply(InputValue::makeText("SOLID"));

        // Inventing a closing segment would hatch a region the user never
        // drew, so the command declines instead.
        CHECK(outcome.status == CommandOutcome::Status::Failed);
        CHECK(harness.entities().size() == 1);
    }

    TEST_CASE("HATCH asks for scale and angle for a real pattern") {
        CommandHarness harness;
        Entity& circle =
            harness.db->addToModelSpace(std::make_unique<CircleEntity>(Vec3::zero(), 20.0));

        harness.executor.execute("HATCH");
        harness.executor.supply(InputValue::makeSelection({circle.handle}));
        harness.executor.supply(InputValue::makeText("ANSI31"));
        harness.executor.supply(InputValue::makeNumber(2.0));
        harness.executor.supply(InputValue::makeNumber(degToRad(45.0)));

        const auto* hatch = harness.only<HatchEntity>();
        REQUIRE(hatch != nullptr);
        CHECK(hatch->pattern.name == "ANSI31");
        CHECK(hatch->pattern.scale == doctest::Approx(2.0));
        CHECK(hatch->pattern.angle == doctest::Approx(degToRad(45.0)));
        // A circle keeps its exact curvature as a single arc edge.
        REQUIRE(hatch->paths.size() == 1);
        REQUIRE(hatch->paths[0].edges.size() == 1);
        CHECK(hatch->paths[0].edges[0].kind == HatchEdge::Kind::Arc);
    }

    TEST_CASE("a hatch and its dimension undo as single steps") {
        CommandHarness harness;
        Entity& circle =
            harness.db->addToModelSpace(std::make_unique<CircleEntity>(Vec3::zero(), 20.0));

        harness.executor.execute("HATCH");
        harness.executor.supply(InputValue::makeSelection({circle.handle}));
        harness.executor.supply(InputValue::makeText("SOLID"));
        REQUIRE(harness.entities().size() == 2);

        harness.executor.execute("DIMRADIUS");
        harness.executor.supply(InputValue::makeEntity(circle.handle));
        harness.executor.supply(point(40.0, 0.0));
        REQUIRE(harness.entities().size() == 3);

        CHECK(harness.transactions.undoDepth() == 2);
        harness.transactions.undo();
        CHECK(harness.entities().size() == 2);
        harness.transactions.undo();
        CHECK(harness.entities().size() == 1);
    }
}

TEST_SUITE("Annotation round trip") {
    TEST_CASE("a dimension survives a DXF write and read") {
        auto db = Database::createEmpty();
        DimStyle& style = db->ensureDimStyle("Fine");
        style.linearPrecision = 3;
        style.textHeight = 3.5;
        style.arrowSize = 4.0;
        style.linearFormat = LinearUnitsFormat::Architectural;
        style.linearPostfix = "<> mm";

        auto dimension = std::make_unique<DimensionEntity>();
        dimension->type = DimensionType::Aligned;
        dimension->point13 = Vec3{10.0, 20.0, 0.0};
        dimension->point14 = Vec3{110.0, 20.0, 0.0};
        dimension->definitionPoint = Vec3{10.0, 45.0, 0.0};
        dimension->textMidPoint = Vec3{60.0, 48.0, 0.0};
        dimension->textOverride = "<> TYP.";
        dimension->styleHandle = style.handle;
        dimension->textPositionIsUserDefined = true;
        db->addToModelSpace(std::move(dimension));

        io::DxfWriter writer;
        const std::string text = writer.write(*db);
        const io::DxfReadResult result = io::readDxf(text);

        const auto entities = result.database->entitiesIn(result.database->modelSpace());
        REQUIRE(entities.size() == 1);
        const auto* read = dynamic_cast<const DimensionEntity*>(entities[0]);
        REQUIRE(read != nullptr);

        CHECK(read->type == DimensionType::Aligned);
        CHECK_POINT(read->point13, (Vec3{10.0, 20.0, 0.0}));
        CHECK_POINT(read->point14, (Vec3{110.0, 20.0, 0.0}));
        CHECK_POINT(read->definitionPoint, (Vec3{10.0, 45.0, 0.0}));
        CHECK(read->textOverride == "<> TYP.");
        CHECK(read->textPositionIsUserDefined);
        CHECK(read->measure() == doctest::Approx(100.0));

        // And the style it names came back with it.
        const DimStyle& readStyle = result.database->dimStyleFor(*read);
        CHECK(readStyle.name == "Fine");
        CHECK(readStyle.linearPrecision == 3);
        CHECK(readStyle.textHeight == doctest::Approx(3.5));
        CHECK(readStyle.arrowSize == doctest::Approx(4.0));
        CHECK(readStyle.linearFormat == LinearUnitsFormat::Architectural);
        CHECK(readStyle.linearPostfix == "<> mm");
    }

    TEST_CASE("every dimension type survives a round trip") {
        auto db = Database::createEmpty();
        for (int type = 0; type <= 6; ++type) {
            auto dimension = std::make_unique<DimensionEntity>();
            dimension->type = static_cast<DimensionType>(type);
            dimension->definitionPoint = Vec3{static_cast<double>(type), 1.0, 0.0};
            dimension->point13 = Vec3{static_cast<double>(type), 2.0, 0.0};
            dimension->point14 = Vec3{static_cast<double>(type), 3.0, 0.0};
            dimension->point15 = Vec3{static_cast<double>(type), 4.0, 0.0};
            dimension->point16 = Vec3{static_cast<double>(type), 5.0, 0.0};
            dimension->leaderLength = 6.0;
            db->addToModelSpace(std::move(dimension));
        }

        io::DxfWriter writer;
        const io::DxfReadResult result = io::readDxf(writer.write(*db));
        const auto entities = result.database->entitiesIn(result.database->modelSpace());
        REQUIRE(entities.size() == 7);

        for (int type = 0; type <= 6; ++type) {
            const auto* read = dynamic_cast<const DimensionEntity*>(entities[type]);
            REQUIRE(read != nullptr);
            CHECK(read->type == static_cast<DimensionType>(type));
        }
    }

    TEST_CASE("a hatch survives a DXF write and read") {
        auto db = Database::createEmpty();

        auto hatch = std::make_unique<HatchEntity>();
        hatch->paths.push_back(squarePath(0.0, 0.0, 50.0, 50.0, true));
        hatch->paths.push_back(squarePath(10.0, 10.0, 20.0, 20.0, false));
        hatch->pattern = *standardHatchPattern("ANSI31");
        hatch->pattern.scale = 2.0;
        hatch->pattern.angle = degToRad(30.0);
        hatch->style = HatchStyle::Normal;
        hatch->isAssociative = false;
        hatch->seedPoints.push_back(Vec3{5.0, 5.0, 0.0});
        db->addToModelSpace(std::move(hatch));

        io::DxfWriter writer;
        const io::DxfReadResult result = io::readDxf(writer.write(*db));

        const auto entities = result.database->entitiesIn(result.database->modelSpace());
        REQUIRE(entities.size() == 1);
        const auto* read = dynamic_cast<const HatchEntity*>(entities[0]);
        REQUIRE(read != nullptr);

        REQUIRE(read->paths.size() == 2);
        CHECK(read->paths[0].flags.isExternal);
        CHECK(read->paths[0].vertices.size() == 4);
        CHECK_FALSE(read->paths[1].flags.isExternal);
        CHECK_POINT(read->paths[0].vertices[2].position, (Vec3{50.0, 50.0, 0.0}));

        CHECK(read->pattern.name == "ANSI31");
        CHECK_FALSE(read->pattern.isSolid);
        CHECK(read->pattern.scale == doctest::Approx(2.0));
        CHECK(read->pattern.angle == doctest::Approx(degToRad(30.0)));
        REQUIRE(read->pattern.lines.size() == 1);
        CHECK(read->pattern.lines[0].offset.y == doctest::Approx(3.175));

        REQUIRE(read->seedPoints.size() == 1);
        CHECK(read->seedPoints[0].x == doctest::Approx(5.0));

        // And it still fills the same region.
        CHECK(read->contains(Vec3{5.0, 5.0, 0.0}));
        CHECK_FALSE(read->contains(Vec3{15.0, 15.0, 0.0}));
    }

    TEST_CASE("a hatch with edge-list boundaries survives a round trip") {
        auto db = Database::createEmpty();

        HatchBoundaryPath path;
        path.flags.isExternal = true;
        path.edges.push_back(HatchEdge::line(Vec3{0.0, 0.0, 0.0}, Vec3{10.0, 0.0, 0.0}));
        path.edges.push_back(HatchEdge::arc(Vec3{5.0, 0.0, 0.0}, 5.0, 0.0, kPi, true));

        auto hatch = std::make_unique<HatchEntity>();
        hatch->paths.push_back(path);
        hatch->pattern.isSolid = true;
        db->addToModelSpace(std::move(hatch));

        io::DxfWriter writer;
        const io::DxfReadResult result = io::readDxf(writer.write(*db));
        const auto entities = result.database->entitiesIn(result.database->modelSpace());
        REQUIRE(entities.size() == 1);
        const auto* read = dynamic_cast<const HatchEntity*>(entities[0]);
        REQUIRE(read != nullptr);

        REQUIRE(read->paths.size() == 1);
        REQUIRE(read->paths[0].edges.size() == 2);
        CHECK(read->paths[0].edges[0].kind == HatchEdge::Kind::Line);
        CHECK(read->paths[0].edges[1].kind == HatchEdge::Kind::Arc);
        CHECK(read->paths[0].edges[1].radius == doctest::Approx(5.0));
        CHECK(read->paths[0].edges[1].endAngle == doctest::Approx(kPi));
    }
}
