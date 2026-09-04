#include "test_helpers.hpp"

#include <algorithm>
#include <memory>
#include <string>

#include "cad/db/BlockTableRecord.hpp"
#include "cad/db/CurveEntities.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/Layer.hpp"
#include "cad/db/Purge.hpp"
#include "cad/db/Transaction.hpp"

using namespace cad;

namespace {

struct Harness {
    std::unique_ptr<Database> db = Database::createEmpty();
    TransactionManager tm{*db};

    /// A block holding one line, on `layerName`, which is created for it.
    BlockTableRecord& addBlock(const std::string& name, const std::string& layerName) {
        auto record = std::make_unique<BlockTableRecord>();
        record->name = name;
        BlockTableRecord& block = static_cast<BlockTableRecord&>(db->addObject(std::move(record)));

        Layer& layer = db->ensureLayer(layerName);
        auto line = std::make_unique<LineEntity>(Vec3::zero(), Vec3{10, 0});
        line->layerHandle = layer.handle;
        db->addEntity(std::move(line), block);
        return block;
    }

    void insertInModelSpace(const BlockTableRecord& block) {
        auto insert = std::make_unique<InsertEntity>();
        insert->blockHandle = block.handle;
        db->addToModelSpace(std::move(insert));
    }

    [[nodiscard]] bool offers(PurgeCandidate::Kind kind, const std::string& name,
                              PurgeKinds allowed = PurgeKinds::All) {
        const std::vector<PurgeCandidate> candidates = findPurgeable(*db, allowed);
        return std::any_of(candidates.begin(), candidates.end(),
                           [&](const PurgeCandidate& c) {
                               return c.kind == kind && c.name == name;
                           });
    }
};

}  // namespace

TEST_SUITE("Purge") {

TEST_CASE("a block nothing inserts is offered, and one that is inserted is not") {
    Harness h;
    h.addBlock("Unused", "Scratch");
    const BlockTableRecord& used = h.addBlock("Used", "Walls");
    h.insertInModelSpace(used);

    CHECK(h.offers(PurgeCandidate::Kind::Block, "Unused"));
    CHECK_FALSE(h.offers(PurgeCandidate::Kind::Block, "Used"));
}

TEST_CASE("purging a block frees the layer only its contents were drawn on") {
    Harness h;
    h.addBlock("Traced", "PDF_Geometry");

    // The layer is unused *because* the block goes. Reported in the same pass:
    // a purge that needs running twice to settle has not finished.
    CHECK(h.offers(PurgeCandidate::Kind::Block, "Traced"));
    CHECK(h.offers(PurgeCandidate::Kind::Layer, "PDF_Geometry"));
}

TEST_CASE("keeping the blocks keeps the layers their contents sit on") {
    Harness h;
    h.addBlock("Traced", "PDF_Geometry");

    // Purging layers alone must not take a layer out from under geometry that
    // is staying, even geometry nothing currently draws: the block definition
    // still names it, and a block whose entities point at a layer that is not
    // in the file is a broken drawing.
    CHECK_FALSE(h.offers(PurgeCandidate::Kind::Layer, "PDF_Geometry", PurgeKinds::Layers));
    // Taking the block too is what frees it.
    CHECK(h.offers(PurgeCandidate::Kind::Layer, "PDF_Geometry",
                   PurgeKinds::Layers | PurgeKinds::Blocks));
}

TEST_CASE("an empty layer is free whether or not blocks are being purged") {
    Harness h;
    h.db->ensureLayer("Nothing here");
    CHECK(h.offers(PurgeCandidate::Kind::Layer, "Nothing here", PurgeKinds::Layers));
}

TEST_CASE("a filter offers only the kinds asked for") {
    Harness h;
    h.addBlock("Traced", "PDF_Geometry");
    CHECK_FALSE(h.offers(PurgeCandidate::Kind::Block, "Traced", PurgeKinds::Layers));
    CHECK(h.offers(PurgeCandidate::Kind::Block, "Traced", PurgeKinds::Blocks));
}

TEST_CASE("a layer still carrying geometry is kept") {
    Harness h;
    Layer& layer = h.db->ensureLayer("Walls");
    auto line = std::make_unique<LineEntity>(Vec3::zero(), Vec3{10, 0});
    line->layerHandle = layer.handle;
    h.db->addToModelSpace(std::move(line));

    CHECK_FALSE(h.offers(PurgeCandidate::Kind::Layer, "Walls"));
}

TEST_CASE("the names a drawing is invalid without are never offered") {
    Harness h;
    // All unused in an empty drawing, and none of them may go: every reader
    // has to invent them again on load.
    CHECK_FALSE(h.offers(PurgeCandidate::Kind::Layer, "0"));
    CHECK_FALSE(h.offers(PurgeCandidate::Kind::Linetype, "Continuous"));
    CHECK_FALSE(h.offers(PurgeCandidate::Kind::TextStyle, "Standard"));
    CHECK_FALSE(h.offers(PurgeCandidate::Kind::DimStyle, "Standard"));
}

TEST_CASE("model space is not an unused block") {
    Harness h;
    CHECK_FALSE(h.offers(PurgeCandidate::Kind::Block, "*Model_Space"));
}

TEST_CASE("the current layer is kept even with nothing on it") {
    Harness h;
    h.db->ensureLayer("Next");
    h.db->settings.currentLayerName = "Next";

    // It is about to be used by the next thing drawn, which is a reference the
    // drawing's contents cannot express.
    CHECK_FALSE(h.offers(PurgeCandidate::Kind::Layer, "Next"));
}

TEST_CASE("an xref is not purged as though it were an unused definition") {
    Harness h;
    BlockTableRecord& block = h.addBlock("SitePlan", "Survey");
    block.xrefType = XrefType::Attachment;
    block.xrefPath = R"(..\survey.dwg)";

    // Nothing inserts it, so it looks purgeable -- but discarding it is
    // detaching a reference to another drawing, which is a different decision
    // and belongs to a different command.
    CHECK_FALSE(h.offers(PurgeCandidate::Kind::Block, "SitePlan"));
}

TEST_CASE("a purge undoes as one action") {
    Harness h;
    h.addBlock("Traced", "PDF_Geometry");
    const std::size_t before = h.db->objectCount();

    Transaction& transaction = h.tm.begin("Purge");
    const std::size_t removed = purge(transaction, findPurgeable(*h.db));
    h.tm.commit(transaction);

    CHECK(removed >= 2);
    CHECK(h.db->findBlock("Traced") == nullptr);
    CHECK(h.db->findLayer("PDF_Geometry") == nullptr);

    // One undo, not one per item. A purge of fifty things a user did not mean
    // to purge must not take fifty undos to reverse.
    h.tm.undo();
    CHECK(h.db->objectCount() == before);
    CHECK(h.db->findBlock("Traced") != nullptr);
    CHECK(h.db->findLayer("PDF_Geometry") != nullptr);
}

TEST_CASE("a block's contents go with it") {
    Harness h;
    const BlockTableRecord& block = h.addBlock("Traced", "PDF_Geometry");
    const Handle content = block.entityHandles.front();

    Transaction& transaction = h.tm.begin("Purge");
    purge(transaction, findPurgeable(*h.db));
    h.tm.commit(transaction);

    // Left behind they would keep naming their layer, and the next purge would
    // find work the first one claimed to have finished.
    const DbObject* orphan = h.db->getObject(content);
    CHECK((orphan == nullptr || orphan->erased));
}

TEST_CASE("a machine-named block is told apart from one a person named") {
    // The list is grouped on this, so a false positive folds a drawing the user
    // recognises in among the debris and a false negative does the reverse.

    // other CAD software's own anonymous records: dimensions, tables, hatches, groups.
    CHECK(isMachineNamedBlock("*D2"));
    CHECK(isMachineNamedBlock("*T19"));
    CHECK(isMachineNamedBlock("*U5"));
    CHECK(isMachineNamedBlock("*Model_Space"));

    // The copies it makes when a block is pasted or redefined.
    CHECK(isMachineNamedBlock("A$C441ce93d"));
    CHECK(isMachineNamedBlock("A$Cc032b726"));

    // Names a person chose, including ones that start the same way. "A$C" is
    // not enough on its own -- somebody may well have a block of cabinets.
    CHECK_FALSE(isMachineNamedBlock("A$Cabinets"));
    CHECK_FALSE(isMachineNamedBlock("A$C"));
    CHECK_FALSE(isMachineNamedBlock("Binder2"));
    CHECK_FALSE(isMachineNamedBlock("2nd Floor Remo Sketch"));
    CHECK_FALSE(isMachineNamedBlock("_Oblique"));
    CHECK_FALSE(isMachineNamedBlock(""));
}

}  // TEST_SUITE
