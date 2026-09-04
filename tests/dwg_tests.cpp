/// Tests for the DWG entry path.
///
/// The converter itself is a third-party binary that may or may not be present,
/// so these test *our* half: that the translator is looked for in the right
/// places, that its absence produces a message with a remedy rather than a
/// crash, and that the loader routes DWG and DXF differently. When a converter
/// is available the last suite runs a real conversion; when it is not, it
/// reports that rather than silently passing.

#include <doctest/doctest.h>

#include <cstdlib>
#include <fstream>
#include <string>

#include "cad/db/CurveEntities.hpp"
#include "cad/io/DxfWriter.hpp"
#include "cad/io/OdaFileConverter.hpp"
#include "cad/io/XrefLoader.hpp"
#include "test_helpers.hpp"

using namespace cad;

namespace {

/// A translator that always fails, for exercising the callers' error paths
/// without depending on anything installed.
class FailingTranslator final : public io::DwgTranslator {
public:
    [[nodiscard]] std::string id() const override { return "failing"; }
    [[nodiscard]] std::string displayName() const override { return "Failing Translator"; }

    [[nodiscard]] Status probe() const override {
        Status status;
        status.available = false;
        status.problem = "deliberately unavailable";
        status.remedy = "nothing to be done";
        return status;
    }

    [[nodiscard]] std::string toDxf(const std::filesystem::path&) const override {
        throw io::TranslationError(io::TranslationFailure::TranslatorFailed, "always fails",
                                   "try something else");
    }
};

/// A translator that returns a fixed DXF, so the routing can be checked
/// without a converter.
class StubTranslator final : public io::DwgTranslator {
public:
    mutable int calls = 0;

    [[nodiscard]] std::string id() const override { return "stub"; }
    [[nodiscard]] std::string displayName() const override { return "Stub"; }

    [[nodiscard]] Status probe() const override {
        Status status;
        status.available = true;
        return status;
    }

    [[nodiscard]] std::string toDxf(const std::filesystem::path&) const override {
        ++calls;
        return "0\nSECTION\n2\nENTITIES\n0\nLINE\n5\n100\n8\n0\n"
               "10\n0.0\n20\n0.0\n11\n10.0\n21\n0.0\n0\nENDSEC\n0\nEOF\n";
    }
};

/// Write a file into the scratch area and return its path.
[[nodiscard]] std::filesystem::path writeTemp(const std::string& name,
                                              const std::string& contents) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream out{path, std::ios::binary};
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return path;
}

}  // namespace

TEST_SUITE("DwgPathDetection") {
    TEST_CASE("DWG is recognised by extension, case-insensitively") {
        CHECK(io::isDwgPath("plan.dwg"));
        CHECK(io::isDwgPath("PLAN.DWG"));
        CHECK(io::isDwgPath("a/b/Plan.Dwg"));
        CHECK_FALSE(io::isDwgPath("plan.dxf"));
        CHECK_FALSE(io::isDwgPath("plan"));
        // Not fooled by the extension appearing elsewhere in the name.
        CHECK_FALSE(io::isDwgPath("plan.dwg.dxf"));
    }
}

TEST_SUITE("OdaFileConverterSearch") {
    TEST_CASE("the search covers the documented locations") {
        const std::vector<std::filesystem::path> locations =
            io::OdaFileConverter::searchLocations();
        REQUIRE_FALSE(locations.empty());

        bool besideApplication = false;
        for (const std::filesystem::path& location : locations) {
            const std::string text = location.string();
            if (text.find("converters") != std::string::npos) {
                besideApplication = true;
            }
        }
        // A converter shipped next to the executable is what keeps the
        // portable build portable, so it must always be searched.
        CHECK(besideApplication);
    }

    TEST_CASE("an explicit path that is wrong fails rather than falling back") {
        // An explicit path is a statement of intent. Quietly searching
        // elsewhere and using a different converter would be worse than
        // failing: the user would never learn their setting was wrong.
        const io::OdaFileConverter converter{"Z:/definitely/not/here/ODAFileConverter.exe"};
        const io::DwgTranslator::Status status = converter.probe();
        CHECK_FALSE(status.available);
        CHECK(status.problem.find("Z:/definitely") != std::string::npos);
        CHECK(converter.findExecutable().empty());
    }

    TEST_CASE("an unavailable converter reports a remedy, not just a failure") {
        const io::OdaFileConverter converter{"Z:/nope/ODAFileConverter.exe"};
        const io::DwgTranslator::Status status = converter.probe();
        REQUIRE_FALSE(status.available);
        CHECK_FALSE(status.remedy.empty());
        // The remedy has to say where to get it, or it is not a remedy.
        CHECK(status.remedy.find("opendesign.com") != std::string::npos);
    }

    TEST_CASE("translating without a converter throws a TranslationError with a remedy") {
        const io::OdaFileConverter converter{"Z:/nope/ODAFileConverter.exe"};
        bool threw = false;
        try {
            (void)converter.toDxf("anything.dwg");
        } catch (const io::TranslationError& error) {
            threw = true;
            CHECK(error.failure() == io::TranslationFailure::TranslatorUnavailable);
            CHECK_FALSE(error.remedy().empty());
        }
        CHECK(threw);
    }
}

TEST_SUITE("DwgLoadRouting") {
    TEST_CASE("a DXF is read directly, without touching the translator") {
        StubTranslator stub;
        const std::filesystem::path path = writeTemp(
            "slate-routing.dxf",
            "0\nSECTION\n2\nENTITIES\n0\nPOINT\n5\n90\n8\n0\n10\n1.0\n20\n2.0\n"
            "0\nENDSEC\n0\nEOF\n");

        io::XrefLoadOptions options;
        options.translator = &stub;
        const io::DrawingLoadResult loaded = io::loadDrawingWithReferences(path, options);

        CHECK(stub.calls == 0);
        CHECK(loaded.database->entitiesIn(loaded.database->modelSpace()).size() == 1);
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    TEST_CASE("a DWG is routed through the translator") {
        StubTranslator stub;
        // Contents are irrelevant: the stub never reads the file, and that is
        // the point — routing is decided by the path, before any parsing.
        const std::filesystem::path path = writeTemp("slate-routing.dwg", "AC1032");

        io::XrefLoadOptions options;
        options.translator = &stub;
        const io::DrawingLoadResult loaded = io::loadDrawingWithReferences(path, options);

        CHECK(stub.calls == 1);
        CHECK(loaded.database->entitiesIn(loaded.database->modelSpace()).size() == 1);
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    TEST_CASE("opening a DWG with no translator configured says so") {
        const std::filesystem::path path = writeTemp("slate-notranslator.dwg", "AC1032");
        io::XrefLoadOptions options;  // translator deliberately left null
        CHECK_THROWS_AS((void)io::loadDrawingWithReferences(path, options), std::runtime_error);
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    TEST_CASE("a failing translator surfaces its own error") {
        FailingTranslator failing;
        const std::filesystem::path path = writeTemp("slate-failing.dwg", "AC1032");

        io::XrefLoadOptions options;
        options.translator = &failing;
        bool threw = false;
        try {
            (void)io::loadDrawingWithReferences(path, options);
        } catch (const io::TranslationError& error) {
            threw = true;
            CHECK(error.remedy() == "try something else");
        }
        CHECK(threw);
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
}

TEST_SUITE("BlockRecordHandles") {
    TEST_CASE("a block is keyed by its BLOCK_RECORD handle, not the BLOCK entity's") {
        // These differ in every drawing other CAD software writes, and it is the *record*
        // handle that layouts, dimension style arrowheads and every contained
        // entity's owner link reference. Keying by the BLOCK entity's own
        // handle looks right on hand-written files and dangles on real ones.
        const std::string dxf =
            "0\nSECTION\n2\nBLOCKS\n"
            "0\nBLOCK\n5\nD654\n330\nD653\n2\nWIDGET\n70\n0\n10\n0.0\n20\n0.0\n3\nWIDGET\n"
            "0\nLINE\n5\nD655\n330\nD653\n8\n0\n10\n0.0\n20\n0.0\n11\n1.0\n21\n0.0\n"
            "0\nENDBLK\n5\nD656\n330\nD653\n"
            "0\nENDSEC\n0\nEOF\n";

        const io::DxfReadResult result = io::readDxf(dxf);
        const BlockTableRecord* block = result.database->findBlock("WIDGET");
        REQUIRE(block != nullptr);
        CHECK(block->handle == Handle::parse("D653"));

        // And the entity inside it now owns up to the same handle, so the
        // owner link resolves.
        REQUIRE(block->entityHandles.size() == 1);
        const Entity* line = result.database->getEntity(block->entityHandles.front());
        REQUIRE(line != nullptr);
        CHECK(line->ownerHandle == Handle::parse("D653"));

        // The BLOCK entity's own handle is reserved too, so nothing allocated
        // later can collide with it.
        CHECK(result.database->peekNextHandle() > Handle::parse("D656"));
    }

    TEST_CASE("a block with no owner code falls back to its own handle") {
        // Hand-written and older DXF omits code 330 on BLOCK. Falling back
        // keeps those files readable.
        const std::string dxf =
            "0\nSECTION\n2\nBLOCKS\n"
            "0\nBLOCK\n5\n40\n2\nDOOR\n70\n0\n10\n0.0\n20\n0.0\n3\nDOOR\n"
            "0\nENDBLK\n5\n43\n"
            "0\nENDSEC\n0\nEOF\n";

        const io::DxfReadResult result = io::readDxf(dxf);
        const BlockTableRecord* block = result.database->findBlock("DOOR");
        REQUIRE(block != nullptr);
        CHECK(block->handle == Handle::parse("40"));
    }

    TEST_CASE("block handles survive a write and read") {
        auto db = Database::createEmpty();
        auto widget = std::make_unique<BlockTableRecord>();
        widget->name = "WIDGET";
        const Handle original = db->addBlock(std::move(widget)).handle;
        db->addEntity(std::make_unique<LineEntity>(Vec3::zero(), Vec3{1.0, 0.0, 0.0}),
                      *db->findBlock("WIDGET"));

        io::DxfWriter writer;
        const io::DxfReadResult reread = io::readDxf(writer.write(*db));

        const BlockTableRecord* block = reread.database->findBlock("WIDGET");
        REQUIRE(block != nullptr);
        // Renumbering blocks on save would break every reference to them.
        CHECK(block->handle == original);
    }
}

TEST_SUITE("OdaFileConverterLive") {
    TEST_CASE("a real conversion round-trips, when a converter is installed") {
        const io::OdaFileConverter converter;
        const io::DwgTranslator::Status status = converter.probe();
        if (!status.available) {
            // Reported rather than silently skipped: a suite that quietly does
            // nothing on the build machine is worse than one that says so.
            MESSAGE("No ODA File Converter installed; live conversion not exercised. "
                    << status.problem);
            return;
        }

        MESSAGE("Using converter at " << converter.findExecutable().string());

        // Round-trip through the converter's own DWG writer, so the test needs
        // no checked-in binary drawing: DXF in, DWG out, DXF back.
        auto db = Database::createEmpty();
        db->addToModelSpace(std::make_unique<LineEntity>(Vec3::zero(), Vec3{100.0, 50.0, 0.0}));
        db->addToModelSpace(std::make_unique<CircleEntity>(Vec3{10.0, 10.0, 0.0}, 25.0));

        io::DxfWriter writer;
        const std::filesystem::path dwgPath =
            std::filesystem::temp_directory_path() / "slate-live.dwg";

        std::error_code ec;
        std::filesystem::remove(dwgPath, ec);

        converter.fromDxf(writer.write(*db), dwgPath);
        REQUIRE(std::filesystem::is_regular_file(dwgPath, ec));

        io::XrefLoadOptions options;
        options.translator = &converter;
        const io::DrawingLoadResult loaded = io::loadDrawingWithReferences(dwgPath, options);

        const auto entities = loaded.database->entitiesIn(loaded.database->modelSpace());
        CHECK(entities.size() == 2);

        bool foundLine = false;
        bool foundCircle = false;
        for (const Entity* entity : entities) {
            if (const auto* line = dynamic_cast<const LineEntity*>(entity)) {
                foundLine = true;
                CHECK(line->end.x == doctest::Approx(100.0));
                CHECK(line->end.y == doctest::Approx(50.0));
            }
            if (const auto* circle = dynamic_cast<const CircleEntity*>(entity)) {
                foundCircle = true;
                CHECK(circle->radius == doctest::Approx(25.0));
            }
        }
        CHECK(foundLine);
        CHECK(foundCircle);

        std::filesystem::remove(dwgPath, ec);
    }
}
