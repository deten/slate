#include "test_helpers.hpp"

#include <filesystem>
#include <fstream>
#include <string>

#include "cad/db/CurveEntities.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/io/XrefLoader.hpp"

using namespace cad;
using namespace cad::io;

namespace {

namespace fs = std::filesystem;

/// A temporary directory tree of DXF files, removed when the fixture dies.
///
/// The xref tests genuinely need the filesystem: path resolution order, the
/// filename fallback and cycle detection are all about what is actually on
/// disk, and mocking that would test the mock.
class TempProject {
public:
    TempProject() {
        root_ = fs::temp_directory_path() /
                ("cad-xref-test-" + std::to_string(std::hash<const void*>{}(this)));
        fs::create_directories(root_);
    }

    ~TempProject() {
        std::error_code error;
        fs::remove_all(root_, error);
    }

    TempProject(const TempProject&) = delete;
    TempProject& operator=(const TempProject&) = delete;
    TempProject(TempProject&&) = delete;
    TempProject& operator=(TempProject&&) = delete;

    [[nodiscard]] const fs::path& root() const noexcept { return root_; }

    void write(const fs::path& relative, const std::string& contents) const {
        const fs::path full = root_ / relative;
        fs::create_directories(full.parent_path());
        std::ofstream stream{full, std::ios::binary};
        stream << contents;
    }

private:
    fs::path root_;
};

struct XrefSpec {
    std::string name;
    std::string path;
    bool overlay = false;
};

/// Build a DXF declaring xrefs and containing one line, so each generated
/// drawing is distinguishable by its geometry.
[[nodiscard]] std::string drawingWith(const std::vector<XrefSpec>& refs, double lineAt) {
    std::string out;
    int handle = 0x100;
    const auto pair = [&out](int code, const std::string& value) {
        out += std::to_string(code) + "\r\n" + value + "\r\n";
    };
    const auto nextHandle = [&handle] {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%X", handle++);
        return std::string{buffer};
    };

    pair(0, "SECTION");
    pair(2, "TABLES");
    pair(0, "TABLE");
    pair(2, "LAYER");
    pair(0, "LAYER");
    pair(5, "10");
    pair(2, "Walls");
    pair(70, "0");
    pair(62, "1");
    pair(6, "Continuous");
    pair(0, "ENDTAB");
    pair(0, "ENDSEC");

    pair(0, "SECTION");
    pair(2, "BLOCKS");
    for (const XrefSpec& ref : refs) {
        pair(0, "BLOCK");
        pair(5, nextHandle());
        pair(2, ref.name);
        // Bit 4 marks an xref; 4 | 8 marks an overlay.
        pair(70, ref.overlay ? "12" : "4");
        pair(1, ref.path);
        pair(10, "0");
        pair(20, "0");
        pair(0, "ENDBLK");
        pair(5, nextHandle());
    }
    pair(0, "ENDSEC");

    pair(0, "SECTION");
    pair(2, "ENTITIES");
    pair(0, "LINE");
    pair(5, nextHandle());
    pair(8, "Walls");
    pair(10, std::to_string(lineAt));
    pair(20, "0");
    pair(11, std::to_string(lineAt + 100.0));
    pair(21, "0");
    for (const XrefSpec& ref : refs) {
        pair(0, "INSERT");
        pair(5, nextHandle());
        pair(8, "0");
        pair(2, ref.name);
        pair(10, "0");
        pair(20, "0");
    }
    pair(0, "ENDSEC");
    pair(0, "EOF");
    return out;
}

[[nodiscard]] const XrefNode* findNode(const XrefLoadResult& result, std::string_view name) {
    for (const XrefNode& node : result.nodes) {
        if (node.name == name) {
            return &node;
        }
    }
    return nullptr;
}

}  // namespace

TEST_SUITE("ResourceResolver") {
    TEST_CASE("resolves relative, absolute and Windows-separator paths") {
        TempProject project;
        project.write("refs/grid.dxf", drawingWith({}, 2000.0));

        ResolverOptions options;
        options.hostDirectory = project.root();
        ResourceResolver resolver{options};

        const ResolutionResult relative = resolver.resolve("./refs/grid.dxf");
        CHECK(relative.found);
        CHECK(relative.source == ResolutionSource::DrawingRelative);

        // DWG stores backslash paths regardless of authoring platform.
        CHECK(resolver.resolve(".\\refs\\grid.dxf").found);

        const std::string absolute = (project.root() / "refs" / "grid.dxf").string();
        CHECK(resolver.resolve(absolute).source == ResolutionSource::Absolute);
    }

    TEST_CASE("falls back to the bare filename when the stored path is stale") {
        // A project copied from another machine has correct filenames and
        // wrong directories, which is what this rescues.
        TempProject project;
        project.write("moved.dxf", drawingWith({}, 500.0));

        ResolverOptions options;
        options.hostDirectory = project.root();
        ResourceResolver resolver{options};

        const ResolutionResult result = resolver.resolve("C:\\old\\machine\\moved.dxf");
        CHECK(result.found);
        CHECK(result.source == ResolutionSource::FilenameFallback);
    }

    TEST_CASE("searches configured search paths") {
        TempProject project;
        project.write("refs/grid.dxf", drawingWith({}, 1.0));
        project.write("elsewhere/marker.txt", "x");

        ResolverOptions options;
        options.hostDirectory = project.root() / "elsewhere";
        options.searchPaths = {project.root() / "refs"};
        options.filenameFallback = false;
        ResourceResolver resolver{options};

        CHECK(resolver.resolve("grid.dxf").source == ResolutionSource::SearchPath);
    }

    TEST_CASE("reports every location tried and never rewrites the stored path") {
        TempProject project;
        ResolverOptions options;
        options.hostDirectory = project.root();
        options.searchPaths = {project.root() / "refs"};
        ResourceResolver resolver{options};

        const std::string stored = ".\\does-not-exist.dxf";
        const ResolutionResult result = resolver.resolve(stored);
        CHECK_FALSE(result.found);
        CHECK(result.source == ResolutionSource::NotFound);
        CHECK(result.attempted.size() > 1);
        // Resolution must not mutate what the drawing recorded, or saving would
        // silently convert a portable relative path to an absolute one.
        CHECK(result.originalPath == stored);
    }

    TEST_CASE("derives a nested resolver rooted at the referencing drawing") {
        TempProject project;
        project.write("refs/deep/detail.dxf", drawingWith({}, 3000.0));

        ResolverOptions options;
        options.hostDirectory = project.root();
        const ResourceResolver resolver{options};

        ResourceResolver nested = resolver.forNestedDrawing(project.root() / "refs" / "grid.dxf");
        CHECK(nested.resolve("./deep/detail.dxf").found);
    }
}

TEST_SUITE("XrefLoader") {
    TEST_CASE("loads references, nests them, and survives a missing one") {
        TempProject project;
        // host -> refs/grid -> refs/deep/detail, plus one reference that is
        // genuinely absent.
        project.write("refs/deep/detail.dxf", drawingWith({}, 3000.0));
        project.write("refs/grid.dxf", drawingWith({{"DETAIL", "./deep/detail.dxf"}}, 2000.0));
        project.write("host.dxf", drawingWith({{"GRID", "./refs/grid.dxf"},
                                               {"MISSING", "./refs/nope.dxf"}},
                                              1000.0));

        const DrawingLoadResult loaded =
            loadDrawingWithReferences(project.root() / "host.dxf");
        Database& db = *loaded.database;

        const XrefNode* grid = findNode(loaded.xrefs, "GRID");
        REQUIRE(grid != nullptr);
        CHECK(grid->status == XrefStatus::Resolved);
        CHECK(grid->entityCount > 0);

        // A nested path resolves relative to *its own* drawing, not the host:
        // "./deep/detail.dxf" from refs/grid.dxf, not from the host directory.
        const XrefNode* detail = findNode(loaded.xrefs, "DETAIL");
        REQUIRE(detail != nullptr);
        CHECK(detail->status == XrefStatus::Resolved);
        CHECK(detail->depth == 1);
        CHECK(detail->resolvedPath ==
              (project.root() / "refs" / "deep" / "detail.dxf").lexically_normal());

        // A missing reference must never prevent the host from opening.
        const XrefNode* missing = findNode(loaded.xrefs, "MISSING");
        REQUIRE(missing != nullptr);
        CHECK(missing->status == XrefStatus::NotFound);
        CHECK_FALSE(missing->attemptedPaths.empty());

        // The host's own geometry is present and correct regardless.
        std::size_t hostLines = 0;
        for (const Entity* entity : db.entitiesIn(db.modelSpace())) {
            if (const auto* line = dynamic_cast<const LineEntity*>(entity)) {
                CHECK(line->start.equals(Vec3{1000, 0}));
                ++hostLines;
            }
        }
        CHECK(hostLines == 1);
        CHECK(loaded.xrefs.failedCount == 1);
        CHECK(loaded.xrefs.resolvedCount >= 2);
    }

    TEST_CASE("prefixes xref symbol records so they cannot collide with the host") {
        TempProject project;
        project.write("refs/grid.dxf", drawingWith({}, 2000.0));
        project.write("host.dxf", drawingWith({{"GRID", "./refs/grid.dxf"}}, 1000.0));

        const DrawingLoadResult loaded =
            loadDrawingWithReferences(project.root() / "host.dxf");
        Database& db = *loaded.database;

        // The host has its own "Walls"; the reference's becomes "GRID|Walls".
        const Layer* hostLayer = db.findLayer("Walls");
        const Layer* xrefLayer = db.findLayer("GRID|Walls");
        REQUIRE(hostLayer != nullptr);
        REQUIRE(xrefLayer != nullptr);
        CHECK(xrefLayer->isXrefDependent);
        // Editing the host's layer must not affect the reference's.
        CHECK(hostLayer->handle != xrefLayer->handle);
    }

    TEST_CASE("detects a reference cycle instead of recursing forever") {
        TempProject project;
        project.write("cycle-a.dxf", drawingWith({{"B", "./cycle-b.dxf"}}, 10.0));
        project.write("cycle-b.dxf", drawingWith({{"A", "./cycle-a.dxf"}}, 20.0));

        const DrawingLoadResult loaded =
            loadDrawingWithReferences(project.root() / "cycle-a.dxf");

        const bool anyCircular =
            std::any_of(loaded.xrefs.nodes.begin(), loaded.xrefs.nodes.end(),
                        [](const XrefNode& node) {
                            return node.status == XrefStatus::Circular;
                        });
        CHECK(anyCircular);
    }

    TEST_CASE("does not load an overlay nested inside another reference") {
        TempProject project;
        project.write("site.dxf", drawingWith({}, 9000.0));
        project.write("refs/with-overlay.dxf",
                      drawingWith({{"SITE", "../site.dxf", true}}, 8000.0));
        project.write("overlay-host.dxf",
                      drawingWith({{"INNER", "./refs/with-overlay.dxf"}}, 7000.0));

        const DrawingLoadResult loaded =
            loadDrawingWithReferences(project.root() / "overlay-host.dxf");

        CHECK(findNode(loaded.xrefs, "INNER")->status == XrefStatus::Resolved);
        // Skipping a nested overlay is exactly what distinguishes it from an
        // attachment, and is how users stop site plans multiplying through a
        // drawing set.
        CHECK(findNode(loaded.xrefs, "SITE")->status == XrefStatus::Unloaded);
    }

    TEST_CASE("loads a top-level overlay normally") {
        TempProject project;
        project.write("site.dxf", drawingWith({}, 9000.0));
        project.write("host.dxf", drawingWith({{"SITE", "./site.dxf", true}}, 1.0));

        const DrawingLoadResult loaded =
            loadDrawingWithReferences(project.root() / "host.dxf");
        CHECK(findNode(loaded.xrefs, "SITE")->status == XrefStatus::Resolved);
    }

    TEST_CASE("reports a stale stored path that resolved by filename") {
        TempProject project;
        project.write("moved.dxf", drawingWith({}, 500.0));
        project.write("stale-path.dxf",
                      drawingWith({{"MOVED", "C:\\old\\machine\\moved.dxf"}}, 100.0));

        const DrawingLoadResult loaded =
            loadDrawingWithReferences(project.root() / "stale-path.dxf");

        CHECK(findNode(loaded.xrefs, "MOVED")->status == XrefStatus::Resolved);
        const bool warned = std::any_of(
            loaded.diagnostics.begin(), loaded.diagnostics.end(),
            [](const DxfDiagnostic& d) {
                return d.message.find("out of date") != std::string::npos;
            });
        CHECK(warned);
    }

    TEST_CASE("includes resolved reference geometry in model extents") {
        TempProject project;
        project.write("refs/deep/detail.dxf", drawingWith({}, 3000.0));
        project.write("refs/grid.dxf", drawingWith({{"DETAIL", "./deep/detail.dxf"}}, 2000.0));
        project.write("host.dxf", drawingWith({{"GRID", "./refs/grid.dxf"}}, 1000.0));

        const DrawingLoadResult loaded =
            loadDrawingWithReferences(project.root() / "host.dxf");
        Database& db = *loaded.database;

        // The host line ends at 1100; the grid reference reaches 2100 and the
        // nested detail 3100. All must contribute.
        const Box3 extents = db.modelExtents();
        CHECK(extents.max.x >= 3000.0);
    }

    TEST_CASE("keeps the insert pointing at the xref block after loading") {
        TempProject project;
        project.write("refs/grid.dxf", drawingWith({}, 2000.0));
        project.write("host.dxf", drawingWith({{"GRID", "./refs/grid.dxf"}}, 1000.0));

        const DrawingLoadResult loaded =
            loadDrawingWithReferences(project.root() / "host.dxf");
        Database& db = *loaded.database;

        const BlockTableRecord* grid = db.findBlock("GRID");
        REQUIRE(grid != nullptr);

        bool found = false;
        for (const Entity* entity : db.entitiesIn(db.modelSpace())) {
            if (const auto* insert = dynamic_cast<const InsertEntity*>(entity)) {
                if (insert->blockHandle == grid->handle) {
                    found = true;
                }
            }
        }
        CHECK(found);
    }

    TEST_CASE("reports a DWG reference clearly when no translator is configured") {
        TempProject project;
        project.write("something.dwg", "not really a dwg");
        project.write("dwg-ref.dxf", drawingWith({{"DWGREF", "./something.dwg"}}, 1.0));

        const DrawingLoadResult loaded =
            loadDrawingWithReferences(project.root() / "dwg-ref.dxf");

        const XrefNode* node = findNode(loaded.xrefs, "DWGREF");
        REQUIRE(node != nullptr);
        CHECK(node->status == XrefStatus::Unresolved);
        CHECK(node->problem.find("translator") != std::string::npos);
    }
}
