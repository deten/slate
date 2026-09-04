#include <algorithm>
#include "test_helpers.hpp"

#include "cad/db/CurveEntities.hpp"
#include "cad/db/SolidEntity.hpp"
#include "cad/geom/Arc.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/Transaction.hpp"

using namespace cad;

namespace {

/// A database plus its transaction manager, the pair every test needs.
struct Harness {
    std::unique_ptr<Database> db = Database::createEmpty();
    TransactionManager tm{*db};

    [[nodiscard]] std::vector<Entity*> modelEntities() {
        return db->entitiesIn(db->modelSpace());
    }

    [[nodiscard]] std::size_t modelCount() { return modelEntities().size(); }
};

[[nodiscard]] std::unique_ptr<LineEntity> makeLine(const Vec3& from, const Vec3& to) {
    return std::make_unique<LineEntity>(from, to);
}

}  // namespace

TEST_SUITE("Handle") {
    TEST_CASE("canonicalizes hex text") {
        CHECK(Handle::parse("1a").value() == 0x1A);
        CHECK(Handle::parse("001F").value() == 0x1F);
        CHECK(Handle{255}.toString() == "FF");
        CHECK(Handle::null().toString() == "0");
    }

    TEST_CASE("rejects malformed handles") {
        CHECK_THROWS_AS((void)Handle::parse("zz"), std::invalid_argument);
        CHECK_THROWS_AS((void)Handle::parse(""), std::invalid_argument);
        // A partially valid string must not silently parse as its prefix.
        CHECK_FALSE(Handle::tryParse("1EZ").has_value());
    }

    TEST_CASE("round-trips through text") {
        const Handle original{0xDEADBEEF};
        CHECK(Handle::parse(original.toString()) == original);
    }

    TEST_CASE("never re-issues a handle it has observed") {
        HandleAllocator allocator{1};
        allocator.observe(Handle{0xFF});
        CHECK(allocator.allocate().value() == 0x100);
    }
}

TEST_SUITE("Database initialization") {
    TEST_CASE("creates the records every drawing must have") {
        Harness h;
        CHECK(h.db->findLayer(kLayerZero) != nullptr);
        CHECK(h.db->findLinetype(kLinetypeContinuous) != nullptr);
        CHECK(h.db->findTextStyle("Standard") != nullptr);
        CHECK(h.db->findBlock(kModelSpace) != nullptr);
        CHECK(h.db->modelSpace().isModelSpace());
    }

    TEST_CASE("marks reserved records so they cannot be deleted") {
        Harness h;
        CHECK(h.db->defaultLayer().isReserved());
        CHECK(h.db->modelSpace().isReserved());
        CHECK(h.db->findLinetype(kLinetypeContinuous)->isReserved());
    }

    TEST_CASE("seeds the standard linetype patterns") {
        Harness h;
        const Linetype* dashed = h.db->findLinetype("Dashed");
        REQUIRE(dashed != nullptr);
        CHECK_FALSE(dashed->isContinuous());
        CHECK(dashed->elements.size() == 2);
        CHECK(h.db->findLinetype(kLinetypeContinuous)->isContinuous());
    }
}

TEST_SUITE("Symbol tables") {
    TEST_CASE("looks up names case-insensitively but preserves original casing") {
        Harness h;
        auto layer = std::make_unique<Layer>();
        layer->name = "Walls";
        h.db->addLayer(std::move(layer));

        CHECK(h.db->findLayer("WALLS") != nullptr);
        CHECK(h.db->findLayer("walls")->name == "Walls");
    }

    TEST_CASE("rejects duplicate names that differ only by case") {
        Harness h;
        auto first = std::make_unique<Layer>();
        first->name = "Walls";
        h.db->addLayer(std::move(first));

        auto second = std::make_unique<Layer>();
        second->name = "WALLS";
        CHECK_THROWS_AS(h.db->addLayer(std::move(second)), std::invalid_argument);
    }

    TEST_CASE("renames while keeping the index consistent") {
        Harness h;
        Layer& layer = h.db->ensureLayer("Old");
        CHECK(h.db->layersMutable().rename("Old", "New"));
        layer.name = "New";

        CHECK(h.db->findLayer("Old") == nullptr);
        CHECK(h.db->findLayer("New") == &layer);
    }

    TEST_CASE("refuses a rename that would collide") {
        Harness h;
        h.db->ensureLayer("A");
        h.db->ensureLayer("B");
        CHECK_FALSE(h.db->layersMutable().rename("A", "b"));
    }

    TEST_CASE("generates unique names for importers") {
        Harness h;
        h.db->ensureLayer("Grid");
        CHECK(h.db->layers().uniqueName("Grid") == "Grid1");
        CHECK(h.db->layers().uniqueName("Other") == "Other");
    }

    TEST_CASE("creates layers on demand for malformed imports") {
        Harness h;
        // Real files do reference layers their own LAYER table omits.
        Layer& created = h.db->ensureLayer("Imported");
        CHECK(h.db->findLayer("Imported") == &created);
        CHECK(&h.db->ensureLayer("Imported") == &created);
    }

    TEST_CASE("normalizes surrounding whitespace in symbol names") {
        Harness h;
        h.db->ensureLayer("Padded");
        CHECK(h.db->findLayer("  Padded  ") != nullptr);
    }
}

TEST_SUITE("Property resolution") {
    TEST_CASE("resolves ByLayer colour from the entity layer") {
        Harness h;
        Layer& layer = h.db->ensureLayer("Red");
        layer.color = Color::fromIndex(1);

        auto line = makeLine(Vec3::zero(), Vec3{10, 0});
        line->layerHandle = layer.handle;
        line->color = Color::byLayer();
        Entity& added = h.db->addToModelSpace(std::move(line));

        CHECK(added.resolveProperties(h.db->resolutionContextFor(added)).rgb == 0xFF0000u);
    }

    TEST_CASE("prefers an explicit entity colour over the layer") {
        Harness h;
        Layer& layer = h.db->ensureLayer("Red");
        layer.color = Color::fromIndex(1);

        auto line = makeLine(Vec3::zero(), Vec3{10, 0});
        line->layerHandle = layer.handle;
        line->color = Color::fromRgb(0, 0x80, 0xFF);
        Entity& added = h.db->addToModelSpace(std::move(line));

        CHECK(added.resolveProperties(h.db->resolutionContextFor(added)).rgb == 0x0080FFu);
    }

    TEST_CASE("resolves ByBlock from the enclosing insert") {
        Harness h;
        Layer& layer = h.db->ensureLayer("L");
        layer.color = Color::fromIndex(1);  // red

        LineEntity line{Vec3::zero(), Vec3{1, 0}};
        line.layerHandle = layer.handle;
        line.color = Color::byBlock();

        LineEntity blockReference{Vec3::zero(), Vec3{1, 0}};
        blockReference.color = Color::fromIndex(3);  // green

        const ResolutionContext context = h.db->resolutionContextFor(line, blockReference);
        CHECK(line.resolveProperties(context).rgb == 0x00FF00u);
    }

    TEST_CASE("falls back to the layer for ByBlock outside any block") {
        Harness h;
        Layer& layer = h.db->ensureLayer("L");
        layer.color = Color::fromIndex(1);

        LineEntity line{Vec3::zero(), Vec3{1, 0}};
        line.layerHandle = layer.handle;
        line.color = Color::byBlock();

        CHECK(line.resolveProperties(h.db->resolutionContextFor(line)).rgb == 0xFF0000u);
    }

    TEST_CASE("multiplies entity and global linetype scale") {
        Harness h;
        h.db->settings.globalLinetypeScale = 2.0;

        LineEntity line{Vec3::zero(), Vec3{1, 0}};
        line.linetypeScale = 3.0;
        line.layerHandle = h.db->defaultLayer().handle;

        CHECK(line.resolveProperties(h.db->resolutionContextFor(line)).linetypeScale ==
              doctest::Approx(6.0));
    }

    TEST_CASE("resolves Default lineweight to the 0.25mm system default") {
        Harness h;
        LineEntity line{Vec3::zero(), Vec3{1, 0}};
        line.layerHandle = h.db->defaultLayer().handle;
        line.lineweight = toRaw(Lineweight::ByLayer);
        h.db->defaultLayer().lineweight = toRaw(Lineweight::Default);

        CHECK(line.resolveProperties(h.db->resolutionContextFor(line)).lineweight == 25);
    }

    TEST_CASE("hides entities on a layer that is off or frozen, but not locked") {
        Harness h;
        Layer& layer = h.db->ensureLayer("Hidden");
        LineEntity line{Vec3::zero(), Vec3{1, 0}};
        line.layerHandle = layer.handle;

        layer.isOff = true;
        CHECK_FALSE(line.resolveProperties(h.db->resolutionContextFor(line)).visible);

        layer.isOff = false;
        layer.isFrozen = true;
        CHECK_FALSE(line.resolveProperties(h.db->resolutionContextFor(line)).visible);

        // Locking restricts editing only; the geometry stays visible.
        layer.isFrozen = false;
        layer.isLocked = true;
        CHECK(line.resolveProperties(h.db->resolutionContextFor(line)).visible);
        CHECK_FALSE(layer.isEditable());
    }
}

TEST_SUITE("Colour") {
    TEST_CASE("maps the reserved ACI indices to inherited colours") {
        CHECK(Color::fromIndex(kAciByLayer).isByLayer());
        CHECK(Color::fromIndex(kAciByBlock).isByBlock());
        CHECK(Color::fromIndex(1).isConcrete());
    }

    TEST_CASE("resolves the named ACI primaries") {
        CHECK(aciToRgb(1) == 0xFF0000u);
        CHECK(aciToRgb(3) == 0x00FF00u);
        CHECK(aciToRgb(5) == 0x0000FFu);
    }

    TEST_CASE("finds the nearest ACI index for a true colour") {
        CHECK(rgbToAci(0xFF0000) == 1);
        CHECK(rgbToAci(0x00FF00) == 3);
    }

    TEST_CASE("snaps a lineweight to the nearest permitted value") {
        CHECK(normalizeLineweight(27) == 25);
        CHECK(normalizeLineweight(0) == 0);
        // Negative sentinels are preserved rather than snapped.
        CHECK(normalizeLineweight(toRaw(Lineweight::ByBlock)) == toRaw(Lineweight::ByBlock));
    }
}

TEST_SUITE("Entities") {
    TEST_CASE("computes bounds and curves for a line") {
        const LineEntity line{Vec3{1, 2}, Vec3{4, 6}};
        CHECK(line.bounds().min.equals(Vec3{1, 2}));
        CHECK(line.toCurves().size() == 1);
        CHECK(line.distanceToPoint(Vec3{1, 2}) == doctest::Approx(0.0));
    }

    TEST_CASE("transforms a line in place") {
        LineEntity line{Vec3::zero(), Vec3{10, 0}};
        line.transformBy(Mat4::translation(Vec3{5, 5}));
        CHECK(line.start.equals(Vec3{5, 5}));
        CHECK(line.end.equals(Vec3{15, 5}));
    }

    TEST_CASE("keeps a circle circular under a uniform scale") {
        CircleEntity circle{Vec3::zero(), 2.0};
        circle.transformBy(Mat4::scaling(3.0));
        CHECK(circle.radius == doctest::Approx(6.0));
        CHECK(circle.requiresEllipseAfter(Mat4::scaling(Vec3{2, 3, 1})));
        CHECK_FALSE(circle.requiresEllipseAfter(Mat4::rotationZ(1.0)));
    }

    TEST_CASE("offers grips that actually edit the entity") {
        LineEntity line{Vec3::zero(), Vec3{10, 0}};
        const std::vector<GripPoint> grips = line.gripPoints();
        REQUIRE(grips.size() == 3);
        // Dragging the midpoint grip translates the whole line.
        line.moveGrip(grips[1], Vec3{5, 5});
        CHECK(line.start.equals(Vec3{0, 5}));
        CHECK(line.end.equals(Vec3{10, 5}));
    }

    TEST_CASE("reshapes an arc by dragging its midpoint grip") {
        ArcEntity arc{Vec3::zero(), 5.0, 0.0, kPi};
        const std::vector<GripPoint> grips = arc.gripPoints();
        arc.moveGrip(grips[2], Vec3{0, 3});
        CHECK(arc.geometry().containsPoint(Vec3{0, 3}, Tolerance{1e-6, 1e-6}));
    }

    TEST_CASE("a solid traces its corners in DXF's order, not ring order") {
        // The trap in the format: the shape runs first, second, *fourth*,
        // third. Read in the stored order the four corners of a rectangle
        // describe a bow tie, and every filled rectangle in a drawing would
        // render as an hourglass.
        SolidEntity solid;
        solid.corners = {Vec3{0.0, 0.0, 0.0}, Vec3{100.0, 0.0, 0.0}, Vec3{0.0, 60.0, 0.0},
                         Vec3{100.0, 60.0, 0.0}};

        const std::vector<Vec3> ring = solid.ring();
        REQUIRE(ring.size() == 4);
        CHECK(ring[0].equals(Vec3{0.0, 0.0, 0.0}));
        CHECK(ring[1].equals(Vec3{100.0, 0.0, 0.0}));
        CHECK(ring[2].equals(Vec3{100.0, 60.0, 0.0}));
        CHECK(ring[3].equals(Vec3{0.0, 60.0, 0.0}));

        // A bow tie would enclose two triangles of 1500 each; the rectangle is
        // 6000. Checking the area is what actually catches the mistake.
        CHECK(solid.bounds().width() == doctest::Approx(100.0));
        CHECK(solid.bounds().height() == doctest::Approx(60.0));
        // Inside the rectangle but outside either lobe of the bow tie.
        CHECK(solid.distanceToPoint(Vec3{10.0, 30.0, 0.0}) == doctest::Approx(0.0));
        CHECK(solid.distanceToPoint(Vec3{90.0, 30.0, 0.0}) == doctest::Approx(0.0));
    }

    TEST_CASE("a solid with a repeated corner is a triangle") {
        // How DXF says "only three". Left as four, the shape carries a
        // zero-length edge and offers a grip on top of another one.
        SolidEntity solid;
        solid.corners = {Vec3{0.0, 0.0, 0.0}, Vec3{100.0, 0.0, 0.0}, Vec3{50.0, 60.0, 0.0},
                         Vec3{50.0, 60.0, 0.0}};

        CHECK(solid.ring().size() == 3);
        CHECK(solid.toCurves().size() == 3);
        CHECK(solid.gripPoints().size() == 3);
        CHECK(solid.distanceToPoint(Vec3{50.0, 20.0, 0.0}) == doctest::Approx(0.0));
        // Outside the triangle, near the apex but off to one side.
        CHECK(solid.distanceToPoint(Vec3{5.0, 55.0, 0.0}) > 1.0);
    }

    TEST_CASE("moving a triangle's apex grip keeps it a triangle") {
        // Both stored corners have to move together. Moving one leaves a
        // quadrilateral with a hairline edge, which looks like the shape grew
        // a spike out of nowhere.
        SolidEntity solid;
        solid.corners = {Vec3{0.0, 0.0, 0.0}, Vec3{100.0, 0.0, 0.0}, Vec3{50.0, 60.0, 0.0},
                         Vec3{50.0, 60.0, 0.0}};

        const std::vector<GripPoint> grips = solid.gripPoints();
        REQUIRE(grips.size() == 3);
        solid.moveGrip(grips[2], Vec3{50.0, 120.0, 0.0});

        CHECK(solid.corners[2].equals(Vec3{50.0, 120.0, 0.0}));
        CHECK(solid.corners[3].equals(Vec3{50.0, 120.0, 0.0}));
        CHECK(solid.ring().size() == 3);
    }

    TEST_CASE("a polyline segment's midpoint grip adds a vertex") {
        // How a straight run gets a new corner without being redrawn.
        PolylineEntity polyline = PolylineEntity::fromPoints(
            std::vector<Vec3>{Vec3::zero(), Vec3{400.0, 0.0, 0.0}, Vec3{400.0, 300.0, 0.0}},
            false);
        const std::vector<GripPoint> grips = polyline.gripPoints();
        // Three vertices, then one midpoint per segment.
        REQUIRE(grips.size() == 5);
        CHECK(grips[3].kind == GripKind::Midpoint);
        CHECK(grips[3].position.equals(Vec3{200.0, 0.0, 0.0}));

        polyline.moveGrip(grips[3], Vec3{200.0, 80.0, 0.0});
        REQUIRE(polyline.vertices.size() == 4);
        CHECK(polyline.vertices[1].position.equals(Vec3{200.0, 80.0, 0.0}));
        // The corner goes *between* the two it was drawn from, not on the end.
        CHECK(polyline.vertices[2].position.equals(Vec3{400.0, 0.0, 0.0}));
    }

    TEST_CASE("a bulged segment's midpoint grip sits on the arc it belongs to") {
        // The grip is placed by a closed-form shortcut rather than by building
        // the arc, so it has to be checked against the arc the renderer draws.
        // Getting the side wrong puts the grip on the far side of the chord,
        // where there is nothing to grab.
        for (const double bulge : {0.4, 1.0, -0.6, -1.0}) {
            PolylineEntity polyline = PolylineEntity::fromPoints(
                std::vector<Vec3>{Vec3::zero(), Vec3{200.0, 40.0, 0.0}}, false);
            polyline.vertices[0].bulge = bulge;

            const std::optional<Arc> arc = Arc::fromBulge(polyline.vertices[0].position,
                                                          polyline.vertices[1].position, bulge);
            REQUIRE(arc.has_value());
            INFO("bulge ", bulge);
            CHECK(polyline.segmentMidpoint(0).equals(arc->midPoint()));
        }
    }

    TEST_CASE("a bulged segment's midpoint grip bends the arc instead") {
        // Splitting an arc where the user meant to adjust its curvature would
        // lose the shape being adjusted, so the bulge changes and the vertex
        // count does not.
        PolylineEntity polyline = PolylineEntity::fromPoints(
            std::vector<Vec3>{Vec3::zero(), Vec3{200.0, 0.0, 0.0}}, false);
        polyline.vertices[0].bulge = 1.0;  // A half circle.

        const std::vector<GripPoint> grips = polyline.gripPoints();
        REQUIRE(grips.size() == 3);
        // The grip sits on the arc, not on the chord: a half circle over a
        // 200-unit chord departs it by 100 -- below, because a positive bulge
        // swings right of the chord.
        CHECK(grips[2].position.equals(Vec3{100.0, -100.0, 0.0}));

        polyline.moveGrip(grips[2], Vec3{100.0, -50.0, 0.0});
        REQUIRE(polyline.vertices.size() == 2);
        // Sagitta over half-chord is the bulge: 50 / 100.
        CHECK(polyline.vertices[0].bulge == doctest::Approx(0.5));
        CHECK(polyline.gripPoints()[2].position.equals(Vec3{100.0, -50.0, 0.0}));
    }

    TEST_CASE("a closed polyline has a midpoint grip on its closing segment") {
        // The segment from the last vertex back to the first is a segment like
        // any other, and leaving it without a grip would make one side of a
        // closed shape uneditable.
        PolylineEntity polyline = PolylineEntity::fromPoints(
            std::vector<Vec3>{Vec3::zero(), Vec3{100.0, 0.0, 0.0}, Vec3{100.0, 100.0, 0.0},
                              Vec3{0.0, 100.0, 0.0}},
            true);
        CHECK(polyline.segmentCount() == 4);
        const std::vector<GripPoint> grips = polyline.gripPoints();
        REQUIRE(grips.size() == 8);
        CHECK(grips.back().position.equals(Vec3{0.0, 50.0, 0.0}));

        polyline.moveGrip(grips.back(), Vec3{-40.0, 50.0, 0.0});
        REQUIRE(polyline.vertices.size() == 5);
        CHECK(polyline.vertices.back().position.equals(Vec3{-40.0, 50.0, 0.0}));
    }

    TEST_CASE("every grip a curve entity offers actually moves something") {
        // Direct editing is a press, a drag and a release, so a grip that
        // does nothing is silently dead — there is no error to notice. This
        // sweeps every grip of every curve type and insists each one changes
        // the entity, which is the property the canvas relies on.
        // Bounds alone are too weak a witness: a polyline segment's midpoint
        // grip adds a vertex, and a vertex added *inside* the existing extent
        // changes the polyline without changing its box. The grip positions
        // catch that -- there is one more of them afterwards.
        const auto fingerprint = [](const Entity& entity) {
            std::vector<Vec3> marks;
            const Box3 box = entity.bounds();
            marks.push_back(box.min);
            marks.push_back(box.max);
            for (const GripPoint& grip : entity.gripPoints()) {
                marks.push_back(grip.position);
            }
            return marks;
        };
        const auto same = [](const std::vector<Vec3>& a, const std::vector<Vec3>& b) {
            return a.size() == b.size() &&
                   std::equal(a.begin(), a.end(), b.begin(),
                              [](const Vec3& l, const Vec3& r) { return l.equals(r); });
        };

        const auto sweep = [&](Entity& entity, const char* what) {
            const std::vector<GripPoint> grips = entity.gripPoints();
            INFO(what, " offers ", grips.size(), " grip(s)");
            REQUIRE_FALSE(grips.empty());

            for (const GripPoint& grip : grips) {
                const DbObjectPtr before = entity.clone();
                const std::vector<Vec3> was = fingerprint(entity);
                // Two units away in both axes: far enough to be unambiguous
                // and not on any other grip.
                entity.moveGrip(grip, grip.position + Vec3{2.0, 2.0, 0.0});

                INFO(what, " grip ", grip.index, " left the entity unchanged");
                CHECK_FALSE(same(was, fingerprint(entity)));

                entity.restoreFrom(*before);
            }
        };

        LineEntity line{Vec3::zero(), Vec3{10.0, 0.0, 0.0}};
        sweep(line, "Line");

        CircleEntity circle{Vec3::zero(), 5.0};
        sweep(circle, "Circle");

        ArcEntity arc{Vec3::zero(), 5.0, 0.0, kPi};
        sweep(arc, "Arc");

        PolylineEntity polyline = PolylineEntity::fromPoints(
            std::vector<Vec3>{Vec3::zero(), Vec3{10.0, 0.0, 0.0}, Vec3{10.0, 10.0, 0.0}}, false);
        sweep(polyline, "Polyline");

        PointEntity point{Vec3{3.0, 4.0, 0.0}};
        sweep(point, "Point");
    }

    TEST_CASE("a grip drag is undoable through the ordinary transaction path") {
        // What the canvas does on release, and the reason it does it that way.
        auto db = Database::createEmpty();
        TransactionManager transactions{*db};
        auto& line = static_cast<LineEntity&>(
            db->addToModelSpace(std::make_unique<LineEntity>(Vec3::zero(), Vec3{10, 0})));

        const std::vector<GripPoint> grips = line.gripPoints();
        Transaction& transaction = transactions.begin("Grip edit");
        transaction.modifyEntity(line,
                                 [&](Entity& target) { target.moveGrip(grips[2], Vec3{10, 8}); });
        transactions.commit(transaction);

        CHECK(line.end.equals(Vec3{10, 8}));
        REQUIRE(transactions.canUndo());
        transactions.undo();
        CHECK(line.end.equals(Vec3{10, 0}));
    }

    TEST_CASE("clones an entity without sharing state") {
        LineEntity line{Vec3::zero(), Vec3{10, 0}};
        line.setXData("ACME", {{1000, std::string{"payload"}}});

        const DbObjectPtr copy = line.clone();
        const auto* copied = dynamic_cast<const LineEntity*>(copy.get());
        REQUIRE(copied != nullptr);
        CHECK(copied->end.equals(Vec3{10, 0}));
        CHECK(copied->getXData("ACME") != nullptr);

        line.end = Vec3{99, 0};
        CHECK(copied->end.equals(Vec3{10, 0}));
    }

    TEST_CASE("rejects a snapshot of the wrong type") {
        LineEntity line{Vec3::zero(), Vec3{1, 0}};
        const CircleEntity circle{Vec3::zero(), 1.0};
        CHECK_THROWS_AS(line.restoreFrom(circle), std::invalid_argument);
    }
}

TEST_SUITE("Transactions and undo/redo") {
    TEST_CASE("records an addition and reverses it") {
        Harness h;
        h.tm.run("Draw Line", [&](Transaction& tx) {
            tx.addToModelSpace(makeLine(Vec3::zero(), Vec3{10, 0}));
        });
        CHECK(h.modelCount() == 1);
        CHECK(h.tm.canUndo());
        CHECK(h.tm.undoName() == "Draw Line");

        CHECK(h.tm.undo());
        CHECK(h.modelCount() == 0);
        CHECK(h.tm.redo());
        CHECK(h.modelCount() == 1);
    }

    TEST_CASE("restores modified geometry exactly") {
        Harness h;
        Handle lineHandle;
        h.tm.run("Draw", [&](Transaction& tx) {
            lineHandle = tx.addToModelSpace(makeLine(Vec3::zero(), Vec3{10, 0})).handle;
        });

        auto* line = h.db->get<LineEntity>(lineHandle);
        REQUIRE(line != nullptr);

        h.tm.run("Move", [&](Transaction& tx) {
            tx.modifyEntity(*line, [](LineEntity& e) {
                e.transformBy(Mat4::translation(Vec3{0, 20}));
            });
        });
        CHECK(line->start.y == doctest::Approx(20.0));

        h.tm.undo();
        CHECK(line->start.y == doctest::Approx(0.0));
        CHECK(line->end.equals(Vec3{10, 0}));

        h.tm.redo();
        CHECK(line->start.y == doctest::Approx(20.0));
    }

    TEST_CASE("treats a multi-object command as a single undo step") {
        Harness h;
        h.tm.run("Draw Many", [&](Transaction& tx) {
            for (int i = 0; i < 5; ++i) {
                tx.addToModelSpace(makeLine(Vec3{static_cast<double>(i), 0},
                                            Vec3{static_cast<double>(i), 10}));
            }
        });
        CHECK(h.modelCount() == 5);
        CHECK(h.tm.undoDepth() == 1);

        h.tm.undo();
        CHECK(h.modelCount() == 0);
    }

    TEST_CASE("restores an erased entity to its original draw order position") {
        Harness h;
        std::vector<Handle> handles;
        h.tm.run("Draw", [&](Transaction& tx) {
            for (int i = 0; i < 3; ++i) {
                handles.push_back(
                    tx.addToModelSpace(std::make_unique<PointEntity>(
                                           Vec3{static_cast<double>(i), 0}))
                        .handle);
            }
        });

        DbObject* middle = h.db->getObject(handles[1]);
        REQUIRE(middle != nullptr);
        h.tm.run("Erase", [&](Transaction& tx) { tx.erase(*middle); });

        CHECK(h.db->modelSpace().entityHandles ==
              std::vector<Handle>{handles[0], handles[2]});

        h.tm.undo();
        CHECK(h.db->modelSpace().entityHandles == handles);
    }

    TEST_CASE("rolls everything back when the body throws") {
        Harness h;
        CHECK_THROWS_AS(h.tm.run("Failing",
                                 [&](Transaction& tx) {
                                     tx.addToModelSpace(makeLine(Vec3::zero(), Vec3{1, 0}));
                                     tx.addToModelSpace(makeLine(Vec3::zero(), Vec3{2, 0}));
                                     throw std::runtime_error("command failed midway");
                                 }),
                        std::runtime_error);

        // Nothing was left behind, and no undo entry was created.
        CHECK(h.modelCount() == 0);
        CHECK_FALSE(h.tm.canUndo());
    }

    TEST_CASE("leaves modified objects untouched after an abort") {
        Harness h;
        Handle lineHandle;
        h.tm.run("Draw", [&](Transaction& tx) {
            lineHandle = tx.addToModelSpace(makeLine(Vec3::zero(), Vec3{10, 0})).handle;
        });
        auto* line = h.db->get<LineEntity>(lineHandle);
        REQUIRE(line != nullptr);
        const std::size_t depthBeforeAbort = h.tm.undoDepth();

        Transaction& tx = h.tm.begin("Cancelled Move");
        tx.modifyEntity(*line, [](LineEntity& e) {
            e.transformBy(Mat4::translation(Vec3{100, 100}));
        });
        h.tm.abort(tx);

        CHECK(line->start.equals(Vec3::zero()));
        CHECK(line->end.equals(Vec3{10, 0}));
        // The aborted transaction contributes no history of its own; the
        // earlier Draw legitimately remains undoable.
        CHECK(h.tm.undoDepth() == depthBeforeAbort);
        CHECK(h.tm.undoName() == "Draw");
    }

    TEST_CASE("joins a nested transaction into the outer one") {
        Harness h;
        h.tm.run("Outer", [&](Transaction& tx) {
            tx.addToModelSpace(makeLine(Vec3::zero(), Vec3{1, 0}));
            h.tm.run("Inner", [&](Transaction& inner) {
                inner.addToModelSpace(makeLine(Vec3::zero(), Vec3{2, 0}));
            });
        });
        CHECK(h.modelCount() == 2);
        // A single undo step, not two.
        CHECK(h.tm.undoDepth() == 1);
        h.tm.undo();
        CHECK(h.modelCount() == 0);
    }

    TEST_CASE("discards the redo branch once a new edit is made") {
        Harness h;
        h.tm.run("A", [&](Transaction& tx) {
            tx.addToModelSpace(std::make_unique<PointEntity>(Vec3::zero()));
        });
        h.tm.undo();
        CHECK(h.tm.canRedo());

        h.tm.run("B", [&](Transaction& tx) {
            tx.addToModelSpace(std::make_unique<PointEntity>(Vec3{1, 1}));
        });
        CHECK_FALSE(h.tm.canRedo());
    }

    TEST_CASE("snapshots an object only once however many times it is modified") {
        Harness h;
        Handle lineHandle;
        h.tm.run("Draw", [&](Transaction& tx) {
            lineHandle = tx.addToModelSpace(makeLine(Vec3::zero(), Vec3{1, 0})).handle;
        });
        auto* line = h.db->get<LineEntity>(lineHandle);
        REQUIRE(line != nullptr);

        h.tm.run("Move Repeatedly", [&](Transaction& tx) {
            for (int i = 0; i < 10; ++i) {
                tx.modifyEntity(*line, [](LineEntity& e) {
                    e.transformBy(Mat4::translation(Vec3{1, 0}));
                });
            }
        });
        CHECK(line->start.x == doctest::Approx(10.0));

        // A single undo returns it all the way to the start.
        h.tm.undo();
        CHECK(line->start.x == doctest::Approx(0.0));
    }

    TEST_CASE("survives many undo/redo cycles without drift") {
        Harness h;
        Handle circleHandle;
        h.tm.run("Draw", [&](Transaction& tx) {
            circleHandle =
                tx.addToModelSpace(std::make_unique<CircleEntity>(Vec3{5, 5}, 3.0)).handle;
        });
        auto* circle = h.db->get<CircleEntity>(circleHandle);
        REQUIRE(circle != nullptr);

        h.tm.run("Scale", [&](Transaction& tx) {
            tx.modifyEntity(*circle,
                            [](CircleEntity& e) { e.transformBy(Mat4::scaling(2.0, Vec3{5, 5})); });
        });

        for (int i = 0; i < 20; ++i) {
            h.tm.undo();
            CHECK(circle->radius == doctest::Approx(3.0));
            h.tm.redo();
            CHECK(circle->radius == doctest::Approx(6.0));
        }
        CHECK(circle->center.equals(Vec3{5, 5}));
    }

    TEST_CASE("preserves XData through a clone and restore cycle") {
        Harness h;
        Handle lineHandle;
        h.tm.run("Draw", [&](Transaction& tx) {
            Entity& added = tx.addToModelSpace(makeLine(Vec3::zero(), Vec3{1, 0}));
            added.setXData("ACME_STRUCTURAL", {{1000, std::string{"BEAM-12"}}});
            lineHandle = added.handle;
        });
        auto* line = h.db->get<LineEntity>(lineHandle);
        REQUIRE(line != nullptr);

        h.tm.run("Move", [&](Transaction& tx) {
            tx.modifyEntity(*line,
                            [](LineEntity& e) { e.transformBy(Mat4::translation(Vec3{5, 0})); });
        });
        h.tm.undo();

        // Third-party data must survive an undo untouched.
        const auto* xdata = line->getXData("ACME_STRUCTURAL");
        REQUIRE(xdata != nullptr);
        REQUIRE(xdata->size() == 1);
        CHECK(std::get<std::string>((*xdata)[0].value) == "BEAM-12");
    }

    TEST_CASE("refuses to undo while a transaction is open") {
        Harness h;
        Transaction& tx = h.tm.begin("Bad");
        CHECK_THROWS_AS((void)h.tm.undo(), std::logic_error);
        h.tm.abort(tx);
    }

    TEST_CASE("refuses to open two transactions at once") {
        Harness h;
        Transaction& tx = h.tm.begin("First");
        CHECK_THROWS_AS((void)h.tm.begin("Second"), std::logic_error);
        h.tm.abort(tx);
    }

    TEST_CASE("records no history for a transaction that changed nothing") {
        Harness h;
        h.tm.run("Empty", [](Transaction&) {});
        CHECK_FALSE(h.tm.canUndo());
    }

    TEST_CASE("caps the undo stack at the configured depth") {
        Harness h;
        h.tm.maxUndoDepth = 5;
        for (int i = 0; i < 12; ++i) {
            h.tm.run("Op " + std::to_string(i), [&](Transaction& tx) {
                tx.addToModelSpace(
                    std::make_unique<PointEntity>(Vec3{static_cast<double>(i), 0}));
            });
        }
        CHECK(h.tm.undoDepth() == 5);
        CHECK(h.tm.history().back() == "Op 11");
    }
}

TEST_SUITE("Erase and purge") {
    TEST_CASE("keeps erased objects retrievable but out of iteration") {
        Harness h;
        Entity& line = h.db->addToModelSpace(makeLine(Vec3::zero(), Vec3{1, 0}));
        const Handle handle = line.handle;

        h.db->eraseObject(handle);
        CHECK(h.modelCount() == 0);
        // Still present for undo and for dangling references.
        CHECK(h.db->getObject(handle) != nullptr);
    }

    TEST_CASE("purges only erased objects that nothing references") {
        Harness h;
        Entity& orphan = h.db->addToModelSpace(std::make_unique<PointEntity>(Vec3::zero()));
        const Handle orphanHandle = orphan.handle;

        auto block = std::make_unique<BlockTableRecord>();
        block->name = "Unused";
        const Handle blockHandle = h.db->addBlock(std::move(block)).handle;

        h.db->eraseObject(orphanHandle);
        h.db->eraseObject(blockHandle);

        CHECK(h.db->purgeErased() == 2);
        CHECK(h.db->getObject(orphanHandle) == nullptr);
        CHECK(h.db->getObject(blockHandle) == nullptr);
    }

    TEST_CASE("keeps an erased object that something still references") {
        Harness h;
        Layer& layer = h.db->ensureLayer("Referenced");
        const Handle layerHandle = layer.handle;

        Entity& line = h.db->addToModelSpace(makeLine(Vec3::zero(), Vec3{1, 0}));
        line.layerHandle = layerHandle;

        h.db->eraseObject(layerHandle);
        // The live line still names the layer, so it must not be purged.
        CHECK(h.db->purgeErased() == 0);
        CHECK(h.db->getObject(layerHandle) != nullptr);
    }

    TEST_CASE("erasing a symbol table record releases its name") {
        // Membership of a name index is part of an object's state, so erase
        // has to maintain it. Otherwise the name stays taken by an object that
        // no longer exists: `findLayer` keeps returning the erased layer, and
        // re-creating one with the same name fails. Undo drives erase, so this
        // is what makes creating a layer inside a transaction reversible.
        Harness h;
        const std::size_t before = h.db->layers().size();
        Layer& layer = h.db->ensureLayer("Temporary");
        const Handle handle = layer.handle;
        REQUIRE(h.db->layers().size() == before + 1);

        h.db->eraseObject(handle);
        CHECK(h.db->layers().size() == before);
        CHECK(h.db->findLayer("Temporary") == nullptr);
        // The object itself survives, because undo may bring it back.
        CHECK(h.db->getObject(handle) != nullptr);

        h.db->uneraseObject(handle);
        CHECK(h.db->layers().size() == before + 1);
        CHECK(h.db->findLayer("Temporary") == &layer);
    }

    TEST_CASE("unerase does not displace a name taken while it was gone") {
        Harness h;
        Layer& original = h.db->ensureLayer("Contested");
        const Handle originalHandle = original.handle;
        h.db->eraseObject(originalHandle);

        Layer& replacement = h.db->ensureLayer("Contested");
        REQUIRE(replacement.handle != originalHandle);

        h.db->uneraseObject(originalHandle);
        // The live record keeps the name. Silently swapping it would change
        // which layer every ByLayer entity on it resolves against.
        CHECK(h.db->findLayer("Contested") == &replacement);
        CHECK(h.db->getObject(originalHandle) != nullptr);
    }
}

TEST_SUITE("Validation") {
    TEST_CASE("reports dangling references without throwing") {
        Harness h;
        Entity& line = h.db->addToModelSpace(makeLine(Vec3::zero(), Vec3{1, 0}));
        line.layerHandle = Handle{0xDEADBEEF};

        const std::vector<DatabaseIssue> issues = h.db->validate();
        const bool found = std::any_of(issues.begin(), issues.end(), [](const DatabaseIssue& i) {
            return i.message.find("DEADBEEF") != std::string::npos;
        });
        CHECK(found);
    }

    TEST_CASE("reports a clean database as having no issues") {
        Harness h;
        h.db->addToModelSpace(makeLine(Vec3::zero(), Vec3{1, 0}));
        CHECK(h.db->validate().empty());
    }
}

TEST_SUITE("Events") {
    TEST_CASE("notifies listeners of adds, modifies and erases") {
        Harness h;
        std::vector<DatabaseEvent::Kind> events;
        const std::size_t id =
            h.db->addListener([&](const DatabaseEvent& e) { events.push_back(e.kind); });

        Entity& line = h.db->addToModelSpace(makeLine(Vec3::zero(), Vec3{1, 0}));
        h.db->notifyModified(line.handle);
        h.db->eraseObject(line.handle);
        h.db->removeListener(id);
        h.db->addToModelSpace(std::make_unique<PointEntity>(Vec3::zero()));

        REQUIRE(events.size() == 3);
        CHECK(events[0] == DatabaseEvent::Kind::ObjectAdded);
        CHECK(events[1] == DatabaseEvent::Kind::ObjectModified);
        CHECK(events[2] == DatabaseEvent::Kind::ObjectErased);
    }

    TEST_CASE("collapses bulk-load notifications into a single reset") {
        Harness h;
        std::vector<DatabaseEvent::Kind> events;
        h.db->addListener([&](const DatabaseEvent& e) { events.push_back(e.kind); });

        h.db->bulkLoad([&] {
            for (int i = 0; i < 100; ++i) {
                h.db->addToModelSpace(
                    std::make_unique<PointEntity>(Vec3{static_cast<double>(i), 0}));
            }
        });

        REQUIRE(events.size() == 1);
        CHECK(events[0] == DatabaseEvent::Kind::Reset);
    }
}

TEST_SUITE("Extents") {
    TEST_CASE("unions entity bounds across model space") {
        Harness h;
        h.db->addToModelSpace(makeLine(Vec3::zero(), Vec3{10, 0}));
        h.db->addToModelSpace(makeLine(Vec3{0, -5}, Vec3{0, 20}));

        const Box3 extents = h.db->modelExtents();
        CHECK(extents.min.y == doctest::Approx(-5.0));
        CHECK(extents.max.x == doctest::Approx(10.0));
        CHECK(extents.max.y == doctest::Approx(20.0));
    }

    TEST_CASE("excludes frozen layers, matching ZOOM EXTENTS") {
        Harness h;
        Layer& frozen = h.db->ensureLayer("Frozen");
        frozen.isFrozen = true;

        h.db->addToModelSpace(makeLine(Vec3::zero(), Vec3{10, 0}));
        Entity& hidden = h.db->addToModelSpace(makeLine(Vec3{1000, 1000}, Vec3{2000, 2000}));
        hidden.layerHandle = frozen.handle;

        CHECK(h.db->modelExtents().max.x == doctest::Approx(10.0));
    }
}

TEST_SUITE("Units") {
    TEST_CASE("converts between drawing units") {
        CHECK(unitConversionFactor(DrawingUnits::Inches, DrawingUnits::Millimeters) ==
              doctest::Approx(25.4));
        CHECK(unitConversionFactor(DrawingUnits::Meters, DrawingUnits::Millimeters) ==
              doctest::Approx(1000.0));
    }

    TEST_CASE("declines to guess when either side is unitless") {
        // Guessing here is how an xref ends up 25.4 times too large.
        CHECK(unitConversionFactor(DrawingUnits::Unitless, DrawingUnits::Meters) ==
              doctest::Approx(1.0));
    }
}
