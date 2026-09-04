/// Tests for the property system.
///
/// The properties palette is generic — it enumerates whatever the registry
/// offers and knows nothing about entity types. That makes these tests the
/// specification for what the palette does, since the palette itself has no
/// type-specific behaviour left to test.

#include <doctest/doctest.h>

#include <algorithm>
#include <string>

#include "cad/db/CurveEntities.hpp"
#include "cad/db/DimensionEntity.hpp"
#include "cad/db/HatchEntity.hpp"
#include "cad/db/Property.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/db/Transaction.hpp"
#include "test_helpers.hpp"

#include "cad/db/SelectionFilter.hpp"

using namespace cad;

namespace {

struct Harness {
    std::unique_ptr<Database> db = Database::createEmpty();
    TransactionManager transactions{*db};

    template <typename T, typename... Args>
    T& add(Args&&... args) {
        return static_cast<T&>(
            db->addToModelSpace(std::make_unique<T>(std::forward<Args>(args)...)));
    }

    /// Set a property on a selection, as the palette does.
    std::size_t set(const std::vector<Handle>& handles, const std::string& id,
                    const PropertyValue& value) {
        Transaction& transaction = transactions.begin("Property");
        const std::size_t changed = applyProperty(transaction, handles, id, value);
        if (changed == 0) {
            transactions.abort(transaction);
        } else {
            transactions.commit(transaction);
        }
        return changed;
    }
};

[[nodiscard]] const AggregatedProperty* row(const std::vector<AggregatedProperty>& rows,
                                            const std::string& id) {
    const auto it = std::find_if(rows.begin(), rows.end(), [&](const AggregatedProperty& r) {
        return r.definition->id == id;
    });
    return it == rows.end() ? nullptr : &*it;
}

[[nodiscard]] bool hasRow(const std::vector<AggregatedProperty>& rows, const std::string& id) {
    return row(rows, id) != nullptr;
}

}  // namespace

TEST_SUITE("PropertyRegistry") {
    TEST_CASE("every entity gets the common properties") {
        Harness harness;
        LineEntity& line = harness.add<LineEntity>(Vec3::zero(), Vec3{10.0, 0.0, 0.0});

        const auto properties = PropertyRegistry::standard().propertiesFor(line);
        REQUIRE_FALSE(properties.empty());

        std::vector<std::string> ids;
        for (const PropertyDefinition* property : properties) {
            ids.push_back(property->id);
        }
        for (const char* id : {"general.layer", "general.color", "general.linetype",
                               "general.lineweight", "general.visible", "geometry.start"}) {
            INFO("expected property ", id);
            CHECK(std::find(ids.begin(), ids.end(), id) != ids.end());
        }
    }

    TEST_CASE("a symbol table record does not get the entity properties") {
        Harness harness;
        Layer& layer = harness.db->ensureLayer("Walls");

        const auto properties = PropertyRegistry::standard().propertiesFor(layer);
        REQUIRE_FALSE(properties.empty());
        for (const PropertyDefinition* property : properties) {
            // A layer with a "Layer" row pointing at itself would be nonsense.
            INFO("unexpected ", property->id);
            CHECK(property->id.rfind("general.layer", 0) != 0);
        }
        CHECK(PropertyRegistry::standard().find(layer, "layer.locked") != nullptr);
    }

    TEST_CASE("read-only properties have no setter, and settable ones do") {
        Harness harness;
        CircleEntity& circle = harness.add<CircleEntity>(Vec3::zero(), 5.0);

        for (const PropertyDefinition* property :
             PropertyRegistry::standard().propertiesFor(circle)) {
            INFO("property ", property->id);
            // The two must agree, or the palette offers an edit that silently
            // does nothing.
            CHECK(property->readOnly == (property->set == nullptr));
            CHECK(property->get != nullptr);
        }
    }

    TEST_CASE("a plugin can add properties to a type it did not define") {
        PropertyRegistry registry;
        registry.registerFor("Line", {[] {
                                 PropertyDefinition property;
                                 property.id = "acme.grade";
                                 property.name = "Grade";
                                 property.category = "Acme";
                                 property.type = PropertyType::Text;
                                 property.readOnly = true;
                                 property.get = [](const DbObject&) -> PropertyValue {
                                     return std::string{"A36"};
                                 };
                                 return property;
                             }()});

        Harness harness;
        LineEntity& line = harness.add<LineEntity>(Vec3::zero(), Vec3{1.0, 0.0, 0.0});
        // This is what makes the palette extensible: the row appears without
        // the palette knowing anything about it.
        const PropertyDefinition* found = registry.find(line, "acme.grade");
        REQUIRE(found != nullptr);
        CHECK(std::get<std::string>(found->get(line)) == "A36");
    }
}

TEST_SUITE("PropertyAggregation") {
    TEST_CASE("a single object reports all of its properties") {
        Harness harness;
        CircleEntity& circle = harness.add<CircleEntity>(Vec3{1.0, 2.0, 0.0}, 5.0);

        const auto rows = aggregateProperties(*harness.db, {circle.handle});
        CHECK(hasRow(rows, "geometry.radius"));
        CHECK(hasRow(rows, "geometry.diameter"));
        CHECK(hasRow(rows, "general.layer"));

        const AggregatedProperty* radius = row(rows, "geometry.radius");
        REQUIRE(radius != nullptr);
        REQUIRE(radius->value.has_value());
        CHECK(std::get<double>(*radius->value) == doctest::Approx(5.0));
        CHECK_FALSE(radius->varies);
    }

    TEST_CASE("a mixed selection keeps only what they all have") {
        Harness harness;
        CircleEntity& circle = harness.add<CircleEntity>(Vec3::zero(), 5.0);
        LineEntity& line = harness.add<LineEntity>(Vec3::zero(), Vec3{10.0, 0.0, 0.0});

        const auto rows = aggregateProperties(*harness.db, {circle.handle, line.handle});
        // Shared: both are entities.
        CHECK(hasRow(rows, "general.layer"));
        CHECK(hasRow(rows, "general.color"));
        // Not shared: a row that applied to half a selection would invite an
        // edit that silently did half a job.
        CHECK_FALSE(hasRow(rows, "geometry.radius"));
        CHECK_FALSE(hasRow(rows, "geometry.start"));
    }

    TEST_CASE("disagreement is reported as varying, not as one of the values") {
        Harness harness;
        CircleEntity& small = harness.add<CircleEntity>(Vec3::zero(), 5.0);
        CircleEntity& large = harness.add<CircleEntity>(Vec3::zero(), 9.0);

        const auto rows = aggregateProperties(*harness.db, {small.handle, large.handle});
        const AggregatedProperty* radius = row(rows, "geometry.radius");
        REQUIRE(radius != nullptr);
        CHECK(radius->varies);
        CHECK_FALSE(radius->value.has_value());

        // Agreement still reports a value.
        const AggregatedProperty* center = row(rows, "geometry.center");
        REQUIRE(center != nullptr);
        CHECK_FALSE(center->varies);
        REQUIRE(center->value.has_value());
    }

    TEST_CASE("doubles agree within tolerance") {
        // Two radii differing in the sixteenth decimal are the same radius; a
        // palette reporting them as varying would then rewrite both to "fix" a
        // difference nobody can see.
        Harness harness;
        CircleEntity& a = harness.add<CircleEntity>(Vec3::zero(), 5.0);
        CircleEntity& b = harness.add<CircleEntity>(Vec3::zero(), 5.0 + 1e-15);

        const auto rows = aggregateProperties(*harness.db, {a.handle, b.handle});
        const AggregatedProperty* radius = row(rows, "geometry.radius");
        REQUIRE(radius != nullptr);
        CHECK_FALSE(radius->varies);
    }

    TEST_CASE("an erased object contributes nothing") {
        Harness harness;
        CircleEntity& circle = harness.add<CircleEntity>(Vec3::zero(), 5.0);
        LineEntity& line = harness.add<LineEntity>(Vec3::zero(), Vec3{1.0, 0.0, 0.0});
        harness.db->eraseObject(line.handle);

        // With the line gone the selection is effectively one circle, so the
        // circle's own rows must come back.
        const auto rows = aggregateProperties(*harness.db, {circle.handle, line.handle});
        CHECK(hasRow(rows, "geometry.radius"));
    }
}

TEST_SUITE("PropertyEditing") {
    TEST_CASE("setting a property is one undoable change across the selection") {
        Harness harness;
        CircleEntity& a = harness.add<CircleEntity>(Vec3::zero(), 5.0);
        CircleEntity& b = harness.add<CircleEntity>(Vec3{20.0, 0.0, 0.0}, 9.0);

        CHECK(harness.set({a.handle, b.handle}, "geometry.radius", PropertyValue{7.0}) == 2);
        CHECK(a.radius == doctest::Approx(7.0));
        CHECK(b.radius == doctest::Approx(7.0));

        // Two entities, one undo step.
        CHECK(harness.transactions.undoDepth() == 1);
        harness.transactions.undo();
        CHECK(a.radius == doctest::Approx(5.0));
        CHECK(b.radius == doctest::Approx(9.0));
    }

    TEST_CASE("setting a value an object already has changes nothing") {
        Harness harness;
        CircleEntity& circle = harness.add<CircleEntity>(Vec3::zero(), 5.0);

        // Otherwise every click in the palette would push an undo entry
        // describing nothing.
        CHECK(harness.set({circle.handle}, "geometry.radius", PropertyValue{5.0}) == 0);
        CHECK(harness.transactions.undoDepth() == 0);
    }

    TEST_CASE("a read-only property cannot be set") {
        Harness harness;
        CircleEntity& circle = harness.add<CircleEntity>(Vec3::zero(), 5.0);
        const double before = kPi * 25.0;

        CHECK(harness.set({circle.handle}, "geometry.area", PropertyValue{1.0}) == 0);
        const auto rows = aggregateProperties(*harness.db, {circle.handle});
        CHECK(std::get<double>(*row(rows, "geometry.area")->value) == doctest::Approx(before));
    }

    TEST_CASE("an entity on a locked layer is not edited through the palette") {
        Harness harness;
        Layer& locked = harness.db->ensureLayer("Locked");
        locked.isLocked = true;

        CircleEntity& protectedCircle = harness.add<CircleEntity>(Vec3::zero(), 5.0);
        protectedCircle.layerHandle = locked.handle;
        CircleEntity& editable = harness.add<CircleEntity>(Vec3{20.0, 0.0, 0.0}, 5.0);

        // A locked layer means visible but not editable, and the palette is
        // exactly the back door that has to respect it.
        CHECK(harness.set({protectedCircle.handle, editable.handle}, "geometry.radius",
                          PropertyValue{8.0}) == 1);
        CHECK(protectedCircle.radius == doctest::Approx(5.0));
        CHECK(editable.radius == doctest::Approx(8.0));
    }

    TEST_CASE("editing a varying row sets every object") {
        Harness harness;
        CircleEntity& a = harness.add<CircleEntity>(Vec3::zero(), 5.0);
        CircleEntity& b = harness.add<CircleEntity>(Vec3::zero(), 9.0);
        REQUIRE(row(aggregateProperties(*harness.db, {a.handle, b.handle}),
                    "geometry.radius")->varies);

        CHECK(harness.set({a.handle, b.handle}, "geometry.radius", PropertyValue{3.0}) == 2);
        const auto rows = aggregateProperties(*harness.db, {a.handle, b.handle});
        CHECK_FALSE(row(rows, "geometry.radius")->varies);
    }

    TEST_CASE("diameter is settable and drives the radius") {
        // A drafter given a diameter should not have to halve it in their head.
        Harness harness;
        CircleEntity& circle = harness.add<CircleEntity>(Vec3::zero(), 5.0);
        CHECK(harness.set({circle.handle}, "geometry.diameter", PropertyValue{24.0}) == 1);
        CHECK(circle.radius == doctest::Approx(12.0));
    }

    TEST_CASE("layer, colour and linetype can be changed across a mixed selection") {
        Harness harness;
        Layer& walls = harness.db->ensureLayer("Walls");
        CircleEntity& circle = harness.add<CircleEntity>(Vec3::zero(), 5.0);
        LineEntity& line = harness.add<LineEntity>(Vec3::zero(), Vec3{1.0, 0.0, 0.0});

        // The whole point of a common id: `general.layer` is spelled the same
        // on every entity type, so a mixed selection edits together.
        CHECK(harness.set({circle.handle, line.handle}, "general.layer",
                          PropertyValue{walls.handle}) == 2);
        CHECK(circle.layerHandle == walls.handle);
        CHECK(line.layerHandle == walls.handle);

        CHECK(harness.set({circle.handle, line.handle}, "general.color",
                          PropertyValue{Color::fromIndex(1)}) == 2);
        CHECK(circle.color.index() == 1);
        CHECK(line.color.index() == 1);
    }

    TEST_CASE("angles are stored in radians whatever the palette shows") {
        Harness harness;
        ArcEntity& arc = harness.add<ArcEntity>(Vec3::zero(), 5.0, 0.0, kPi);
        CHECK(harness.set({arc.handle}, "geometry.startAngle",
                          PropertyValue{degToRad(45.0)}) == 1);
        CHECK(arc.startAngle == doctest::Approx(kPi / 4.0));
    }

    TEST_CASE("a hatch pattern change swaps the definition, not just the name") {
        Harness harness;
        auto hatch = std::make_unique<HatchEntity>();
        hatch->pattern = *standardHatchPattern("ANSI31");
        hatch->pattern.scale = 3.0;
        HatchEntity& added = static_cast<HatchEntity&>(harness.db->addToModelSpace(
            std::move(hatch)));

        CHECK(harness.set({added.handle}, "hatch.pattern", PropertyValue{std::string{"NET"}}) ==
              1);
        CHECK(added.pattern.name == "NET");
        // Otherwise it would keep drawing ANSI31 under a new label.
        CHECK(added.pattern.lines.size() == 2);
        // And the scale the user chose survives the swap.
        CHECK(added.pattern.scale == doctest::Approx(3.0));
    }

    TEST_CASE("a dimension's style and override are editable") {
        Harness harness;
        DimStyle& fine = harness.db->ensureDimStyle("Fine");
        auto dimension = std::make_unique<DimensionEntity>();
        dimension->point13 = Vec3::zero();
        dimension->point14 = Vec3{100.0, 0.0, 0.0};
        DimensionEntity& added =
            static_cast<DimensionEntity&>(harness.db->addToModelSpace(std::move(dimension)));

        CHECK(harness.set({added.handle}, "dim.style", PropertyValue{fine.handle}) == 1);
        CHECK(added.styleHandle == fine.handle);

        CHECK(harness.set({added.handle}, "dim.textOverride",
                          PropertyValue{std::string{"<> TYP."}}) == 1);
        CHECK(added.measurementText(fine) == "100.00 TYP.");
    }

    TEST_CASE("layer properties are editable through the same mechanism") {
        // The layer manager uses this too, so a layer is not a special case.
        Harness harness;
        Layer& walls = harness.db->ensureLayer("Walls");

        CHECK(harness.set({walls.handle}, "layer.locked", PropertyValue{true}) == 1);
        CHECK(walls.isLocked);
        CHECK(harness.set({walls.handle}, "layer.color",
                          PropertyValue{Color::fromIndex(3)}) == 1);
        CHECK(walls.color.index() == 3);

        harness.transactions.undo();
        CHECK(walls.color.index() != 3);
    }
}

/// Quick select filters through the same registry the palette reads, so a
/// property an entity type contributes is queryable without this code knowing
/// the type exists. These check the query semantics rather than the dialog.
TEST_SUITE("Quick select") {
    TEST_CASE("filters by object type alone") {
        Harness harness;
        harness.add<LineEntity>(Vec3::zero(), Vec3{10, 0});
        harness.add<LineEntity>(Vec3::zero(), Vec3{0, 10});
        harness.add<CircleEntity>(Vec3::zero(), 5.0);

        SelectionFilter filter;
        filter.objectType = "Circle";

        const std::vector<Handle> matches =
            findByFilter(*harness.db, harness.db->modelSpace(), filter);
        REQUIRE(matches.size() == 1);
        CHECK(harness.db->getEntity(matches.front())->objectType() == "Circle");
    }

    TEST_CASE("filters by a property across every type") {
        Harness harness;
        Layer& demolition = harness.db->ensureLayer("A-DEMO");
        auto& line = harness.add<LineEntity>(Vec3::zero(), Vec3{10, 0});
        auto& circle = harness.add<CircleEntity>(Vec3::zero(), 5.0);
        harness.add<LineEntity>(Vec3::zero(), Vec3{0, 10});
        line.layerHandle = demolition.handle;
        circle.layerHandle = demolition.handle;

        SelectionFilter filter;
        filter.property = "general.layer";
        filter.value = demolition.handle;

        // Reaching objects that share a property but are nowhere near each
        // other is the whole point: a window selection cannot express this.
        const std::vector<Handle> matches =
            findByFilter(*harness.db, harness.db->modelSpace(), filter);
        CHECK(matches.size() == 2);
    }

    TEST_CASE("an object without the property is not a match rather than an error") {
        Harness harness;
        harness.add<CircleEntity>(Vec3::zero(), 5.0);
        harness.add<LineEntity>(Vec3::zero(), Vec3{10, 0});

        SelectionFilter filter;
        filter.property = "geometry.radius";
        filter.value = 5.0;

        // "radius equals 5" over a mixed drawing returns the circles; it does
        // not complain about the lines.
        const std::vector<Handle> matches =
            findByFilter(*harness.db, harness.db->modelSpace(), filter);
        REQUIRE(matches.size() == 1);
        CHECK(harness.db->getEntity(matches.front())->objectType() == "Circle");
    }

    TEST_CASE("ordering comparisons work on numbers and are harmless elsewhere") {
        Harness harness;
        harness.add<CircleEntity>(Vec3::zero(), 2.0);
        harness.add<CircleEntity>(Vec3{50, 0}, 8.0);

        SelectionFilter filter;
        filter.objectType = "Circle";
        filter.property = "geometry.radius";
        filter.comparison = SelectionFilter::Comparison::Greater;
        filter.value = 5.0;
        CHECK(findByFilter(*harness.db, harness.db->modelSpace(), filter).size() == 1);

        filter.comparison = SelectionFilter::Comparison::Less;
        CHECK(findByFilter(*harness.db, harness.db->modelSpace(), filter).size() == 1);

        // Ordering a colour means nothing; a nonsensical query selects nothing
        // rather than failing.
        CHECK_FALSE(comparePropertyValues(Color::fromIndex(1), Color::fromIndex(2),
                                          SelectionFilter::Comparison::Greater));
    }

    TEST_CASE("contains matches text case-insensitively") {
        Harness harness;
        harness.add<MTextEntity>(Vec3::zero(), "Master Bedroom", 2.5);
        harness.add<MTextEntity>(Vec3{50, 0}, "Kitchen", 2.5);

        SelectionFilter filter;
        filter.objectType = "MText";
        filter.property = "text.contents";
        filter.comparison = SelectionFilter::Comparison::Contains;
        filter.value = std::string{"bed"};

        // Nobody searching for "bed" means to exclude "Bedroom".
        CHECK(findByFilter(*harness.db, harness.db->modelSpace(), filter).size() == 1);
    }

    TEST_CASE("objects on a locked or hidden layer are never matched") {
        Harness harness;
        Layer& locked = harness.db->ensureLayer("Locked");
        locked.isLocked = true;
        Layer& off = harness.db->ensureLayer("Off");
        off.isOff = true;

        harness.add<CircleEntity>(Vec3::zero(), 5.0).layerHandle = locked.handle;
        harness.add<CircleEntity>(Vec3{20, 0}, 5.0).layerHandle = off.handle;
        harness.add<CircleEntity>(Vec3{40, 0}, 5.0);

        SelectionFilter filter;
        filter.objectType = "Circle";

        // A selection the user cannot then edit is a trap, and it is the same
        // rule picking already applies.
        CHECK(findByFilter(*harness.db, harness.db->modelSpace(), filter).size() == 1);
    }

    TEST_CASE("a query can be narrowed to an existing selection") {
        Harness harness;
        Layer& demolition = harness.db->ensureLayer("A-DEMO");
        auto& a = harness.add<CircleEntity>(Vec3::zero(), 5.0);
        auto& b = harness.add<CircleEntity>(Vec3{20, 0}, 5.0);
        harness.add<CircleEntity>(Vec3{40, 0}, 5.0).layerHandle = demolition.handle;
        a.layerHandle = demolition.handle;
        b.layerHandle = demolition.handle;

        SelectionFilter filter;
        filter.property = "general.layer";
        filter.value = demolition.handle;
        filter.within = std::vector<Handle>{a.handle};

        // Successive queries narrow rather than start over: "every circle",
        // then "of those, the ones on the demolition layer".
        CHECK(findByFilter(*harness.db, harness.db->modelSpace(), filter).size() == 1);
    }

    TEST_CASE("the result mode combines with what is already selected") {
        const std::vector<Handle> current{Handle{1}, Handle{2}};
        const std::vector<Handle> matches{Handle{2}, Handle{3}};

        CHECK(applySelectionMode(current, matches, SelectionFilter::Mode::Replace) == matches);

        const auto added = applySelectionMode(current, matches, SelectionFilter::Mode::Add);
        CHECK(added.size() == 3);

        const auto removed = applySelectionMode(current, matches, SelectionFilter::Mode::Remove);
        REQUIRE(removed.size() == 1);
        CHECK(removed.front() == Handle{1});
    }
}
