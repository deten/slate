/// Report on a drawing without opening a window.
///
/// It is how DWG translation is verified — the GUI is the wrong place to find
/// out whether a converter works — and with `--sheets` it answers the question
/// a screenshot answers slowly and ambiguously: does this layout actually draw
/// anything, and if not, at which step did its contents disappear.
///
///     dwg_info <drawing.dwg|drawing.dxf> [--dxf out.dxf] [--annotations] [--sheets]
///              [--fields] [--attachments]

#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>

#include "cad/db/DimensionEntity.hpp"
#include "cad/db/FieldEvaluator.hpp"
#include "cad/db/TableEntity.hpp"
#include "cad/db/FieldObject.hpp"
#include "cad/db/HatchEntity.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/db/ViewportEntity.hpp"
#include "cad/io/OdaFileConverter.hpp"
#include "cad/io/XrefLoader.hpp"
#include "cad/render/Scene.hpp"

namespace {

/// Build every sheet as the canvas would, and say what came out.
///
/// Each viewport is reported with the model window it looks through and how
/// many entities the index finds there, so an empty sheet can be told apart
/// from a sheet whose viewport is aimed at empty space.

/// Every table as a grid, so a formula over its cells can be checked by eye.
///
/// A schedule's total is written `SUM(B2:B11)`, and whether that means the
/// second row of the grid or the second row *of data* is not something to guess
/// at: printing the grid with the labels the formula uses settles it against a
/// drawing whose own cached total says what the answer should be.
void reportTables(cad::Database& db) {
    std::cout << "\n  tables:\n";
    for (const cad::Handle blockHandle : db.blocks().handles()) {
        const auto* block = db.get<cad::BlockTableRecord>(blockHandle);
        if (block == nullptr) {
            continue;
        }
        for (const cad::Entity* entity : db.entitiesIn(*block)) {
            const auto* table = dynamic_cast<const cad::TableEntity*>(entity);
            if (table == nullptr) {
                continue;
            }
            std::cout << "    table " << table->handle.toString() << ": " << table->rowCount()
                      << " rows x " << table->columnCount() << " columns\n";
            for (std::size_t row = 0; row < table->rowCount(); ++row) {
                std::cout << "      row " << (row + 1) << " |";
                for (std::size_t column = 0; column < table->columnCount(); ++column) {
                    const cad::TableCell* cell = table->cellAt(row, column);
                    std::cout << ' ' << static_cast<char>('A' + static_cast<int>(column))
                              << (row + 1) << "=\"" << (cell == nullptr ? "" : cell->displayText())
                              << '"';
                    if (cell != nullptr && !cell->fieldHandle.isNull()) {
                        if (const auto* f = db.get<cad::FieldObject>(cell->fieldHandle)) {
                            std::cout << " [" << f->fieldCode << " cached=" << f->cachedValue
                                      << ']';
                        }
                    }
                }
                std::cout << '\n';
            }
        }
    }
}

/// Report every field in the drawing, with what it says now and what the
/// authoring application last computed.
///
/// The two should agree. Where they do not, either the drawing has changed since
/// it was last saved — which is the whole point of a field — or this
/// application's evaluation is wrong, and the difference is the fastest way to
/// tell which.
void reportFields(cad::Database& db, const std::filesystem::path& source) {
    cad::FieldContext context;
    context.fileName = source.filename().string();
    context.filePath = std::filesystem::absolute(source).string();
    const cad::FieldEvaluator evaluator{db, context};

    std::size_t total = 0;
    std::size_t evaluated = 0;
    std::size_t agreed = 0;
    std::map<std::string, std::size_t> unsupported;
    std::vector<std::string> samples;

    for (const cad::Handle blockHandle : db.blocks().handles()) {
        const auto* block = db.get<cad::BlockTableRecord>(blockHandle);
        if (block == nullptr) {
            continue;
        }
        for (const cad::Entity* entity : db.entitiesIn(*block)) {
            if (entity->fieldHandle.isNull()) {
                continue;
            }
            const auto* field = db.get<cad::FieldObject>(entity->fieldHandle);
            if (field == nullptr) {
                continue;
            }
            ++total;
            const cad::FieldResult result = evaluator.evaluate(*field);
            if (result.evaluated) {
                ++evaluated;
            } else if (!result.problem.empty()) {
                ++unsupported[result.problem];
            }

            std::string stored;
            if (const auto* text = dynamic_cast<const cad::MTextEntity*>(entity)) {
                stored = text->contents;
            } else if (const auto* plain = dynamic_cast<const cad::TextEntity*>(entity)) {
                stored = plain->contents;
            }
            if (result.text == stored) {
                ++agreed;
            } else if (samples.size() < 6) {
                std::string detail = "      " + field->evaluatorId + " " + field->fieldCode +
                                     "\n        ours    " + result.text +
                                     "\n        drawing " + stored;
                for (const cad::Handle child : field->childFields) {
                    if (const auto* sub = db.get<cad::FieldObject>(child)) {
                        detail += "\n        child " + sub->evaluatorId + " " + sub->fieldCode +
                                  "  cached=" + sub->cachedValue +
                                  "  ours=" + evaluator.evaluate(*sub).text;
                        for (const cad::Handle object : sub->referencedObjects) {
                            detail += "\n          -> " + object.toString();
                            if (const cad::DbObject* named = db.getObject(object)) {
                                detail += " " + named->objectType();
                            }
                        }
                    }
                }
                samples.push_back(detail);
            }
        }
    }

    std::cout << "  fields     " << total << " (" << evaluated << " evaluated here, " << agreed
              << " matching the drawing)\n";
    for (const std::string& sample : samples) {
        std::cout << sample << "\n";
    }
    for (const auto& [problem, count] : unsupported) {
        std::cout << "      " << count << "x " << problem << "\n";
    }
}

/// The files a drawing displays rather than merges, and whether they were found.
///
/// A missing image is invisible in the drawing by definition — that is what
/// makes it worth a report of its own. Placement counts are here because they
/// are what decides whether a broken path matters: one photograph on a detail
/// sheet is not the same problem as a survey used on every sheet in the set.
void reportAttachments(const std::vector<cad::io::AttachedFileNode>& attachments) {
    std::cout << "\n  attached files " << attachments.size() << '\n';
    for (const cad::io::AttachedFileNode& node : attachments) {
        std::cout << "    " << std::left << std::setw(6) << node.kind
                  << (node.found ? "found   " : "MISSING ") << node.placements
                  << " placement(s), " << node.drawnPlacements << " drawn  "
                  << node.storedPath << '\n';
        if (node.found) {
            std::cout << "           -> " << node.resolvedPath.string() << '\n';
        }
        for (const std::string& block : node.unplacedBlocks) {
            std::cout << "           not drawn: it is in block \"" << block
                      << "\", which no layout inserts\n";
        }
    }
}

void reportSheets(cad::Database& db) {
    cad::render::SpatialIndex index;
    cad::render::SceneBuilder builder{db, index};
    builder.reindex(db.modelSpace());

    std::cout << "  sheet rendering:\n";
    for (const cad::Layout* layout : db.layouts()) {
        if (layout->isModel()) {
            continue;
        }
        const cad::Vec3 size = layout->plot.sheetSize();

        // The view the application opens a sheet with: the whole page.
        cad::render::Viewport viewport{cad::Vec3{size.x / 2.0, size.y / 2.0, 0.0}, 1.0, 1600, 900};
        viewport = viewport.zoomToFit(cad::Box3::fromCorners(cad::Vec3::zero(), size));

        const cad::render::Scene scene = builder.buildLayout(*layout, viewport, {});
        std::cout << "    " << std::left << std::setw(24) << layout->name << scene.items.size()
                  << " items\n";

        const auto* sheet = db.get<cad::BlockTableRecord>(layout->blockRecordHandle);
        if (sheet == nullptr) {
            continue;
        }
        for (const cad::Entity* entity : db.entitiesIn(*sheet)) {
            const auto* window = dynamic_cast<const cad::ViewportEntity*>(entity);
            if (window == nullptr || window->isPaperSpaceViewport()) {
                continue;
            }
            const cad::Box3 region = window->modelRegion();
            std::cout << "      viewport id " << window->viewportId
                      << (window->isOn() ? " on " : " OFF") << " paper "
                      << window->paperRect().min.x << "," << window->paperRect().min.y << ".."
                      << window->paperRect().max.x << "," << window->paperRect().max.y
                      << "  model " << region.min.x << "," << region.min.y << ".." << region.max.x
                      << "," << region.max.y << "  " << index.search(region).size()
                      << " entities there\n";
        }
    }
}

void reportTranslator(const cad::io::OdaFileConverter& converter) {
    const cad::io::DwgTranslator::Status status = converter.probe();
    std::cout << "DWG translator: " << converter.displayName() << " — "
              << (status.available ? "available" : "NOT AVAILABLE") << '\n';
    if (status.available) {
        std::cout << "  at " << converter.findExecutable().string() << '\n';
        return;
    }
    std::cout << "  " << status.problem << '\n';
    std::cout << "  searched:\n";
    for (const std::filesystem::path& location : cad::io::OdaFileConverter::searchLocations()) {
        std::error_code ec;
        const bool present = std::filesystem::is_regular_file(location, ec);
        std::cout << "    [" << (present ? "found  " : "missing") << "] " << location.string()
                  << '\n';
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: dwg_info <drawing.dwg|drawing.dxf> [--dxf out.dxf]\n"
                     "                 [--annotations] [--sheets] [--fields] [--attachments]\n";
        cad::io::OdaFileConverter probe;
        reportTranslator(probe);
        return 2;
    }

    const std::filesystem::path source = argv[1];
    std::filesystem::path dxfOutput;
    bool showAnnotations = false;
    bool showSheets = false;
    bool showFields = false;
    bool showAttachments = false;
    for (int i = 2; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--dxf" && i + 1 < argc) {
            dxfOutput = argv[++i];
        } else if (argument == "--annotations") {
            showAnnotations = true;
        } else if (argument == "--sheets") {
            showSheets = true;
        } else if (argument == "--fields") {
            showFields = true;
        } else if (argument == "--attachments") {
            showAttachments = true;
        }
    }

    cad::io::OdaFileConverter converter;
    converter.setProgressCallback(
        [](const std::string& message) { std::cout << message << '\n'; });

    if (cad::io::isDwgPath(source)) {
        reportTranslator(converter);
    }

    cad::io::XrefLoadOptions options;
    options.translator = &converter;
    options.searchPaths.push_back(std::filesystem::absolute(source).parent_path());

    try {
        // Kept separate from the load so the intermediate DXF can be written
        // out for inspection: when a drawing reads oddly, the first question is
        // always whether the translator or the reader is at fault.
        if (!dxfOutput.empty() && cad::io::isDwgPath(source)) {
            const std::string text = converter.toDxf(source);
            std::ofstream out{dxfOutput, std::ios::binary};
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            std::cout << "wrote intermediate DXF: " << dxfOutput.string() << " (" << text.size()
                      << " bytes)\n";
        }

        const cad::io::DrawingLoadResult loaded =
            cad::io::loadDrawingWithReferences(source, options);
        const cad::Database& db = *loaded.database;

        std::cout << "\n" << source.filename().string() << '\n';
        std::cout << "  objects    " << db.objectCount() << '\n';
        std::cout << "  layers     " << db.layers().size() << '\n';
        std::cout << "  linetypes  " << db.linetypes().size() << '\n';
        std::cout << "  textstyles " << db.textStyles().size() << '\n';
        std::cout << "  dimstyles  " << db.dimStyles().size() << '\n';
        std::cout << "  blocks     " << db.blocks().size() << '\n';

        std::map<std::string, std::size_t> byType;
        for (const cad::Entity* entity :
             loaded.database->entitiesIn(loaded.database->modelSpace())) {
            ++byType[entity->objectType()];
        }
        std::cout << "  model space entities by type:\n";
        for (const auto& [type, count] : byType) {
            std::cout << "    " << std::left << std::setw(24) << type << count << '\n';
        }

        const cad::Box3 extents = loaded.database->modelExtents();
        if (!extents.isEmpty()) {
            std::cout << "  extents    (" << extents.min.x << ", " << extents.min.y << ") .. ("
                      << extents.max.x << ", " << extents.max.y << ")\n";
        }

        const std::vector<const cad::Layout*> layouts = db.layouts();
        std::cout << "  layouts    " << layouts.size() << '\n';
        for (const cad::Layout* layout : layouts) {
            const auto* sheet =
                loaded.database->get<cad::BlockTableRecord>(layout->blockRecordHandle);
            std::size_t viewports = 0;
            std::size_t entities = 0;
            if (sheet != nullptr) {
                for (const cad::Entity* entity : loaded.database->entitiesIn(*sheet)) {
                    ++entities;
                    if (const auto* viewport =
                            dynamic_cast<const cad::ViewportEntity*>(entity);
                        viewport != nullptr && !viewport->isPaperSpaceViewport()) {
                        ++viewports;
                    }
                }
            }
            // Reported in the sheet's own units, because the point of this line
            // is to compare against where the viewports actually sit.
            const cad::Vec3 size = layout->plot.sheetSize();
            const char* unit = layout->plot.paperUnits == cad::PlotPaperUnits::Inches ? "in"
                               : layout->plot.paperUnits == cad::PlotPaperUnits::Pixels
                                   ? "px"
                                   : "mm";
            std::cout << "    " << std::left << std::setw(24) << layout->name
                      << (sheet == nullptr ? "(no block)" : sheet->name) << "  " << entities
                      << " entities, " << viewports << " viewport(s), " << size.x << "x" << size.y
                      << unit;
            if (layout->plot.rotation != 0) {
                std::cout << " rotated " << layout->plot.rotation << "deg";
            }
            std::cout << "\n";
        }

        if (showSheets) {
            reportSheets(*loaded.database);
        }

        if (showAttachments) {
            reportAttachments(loaded.attachments);
        }

        if (showFields) {
            reportTables(*loaded.database);
            reportFields(*loaded.database, source);
        }

        if (!loaded.xrefs.nodes.empty()) {
            std::cout << "  references " << loaded.xrefs.nodes.size() << " ("
                      << loaded.xrefs.resolvedCount << " resolved, " << loaded.xrefs.failedCount
                      << " failed)\n";
            for (const cad::io::XrefNode& node : loaded.xrefs.nodes) {
                std::cout << "    " << node.name << ": "
                          << (node.problem.empty() ? "ok" : node.problem) << '\n';
            }
        }

        if (showAnnotations) {
            // Text angles, grouped. Nearly all text in an architectural drawing
            // is horizontal, so a spread of odd angles is the signature of a
            // reader picking rotation out of the wrong group — which is exactly
            // what an embedded object's repeated codes cause.
            std::map<int, std::size_t> textAngles;
            for (const cad::Entity* entity :
                 loaded.database->entitiesIn(loaded.database->modelSpace())) {
                if (const auto* mtext = dynamic_cast<const cad::MTextEntity*>(entity)) {
                    textAngles[static_cast<int>(std::lround(cad::radToDeg(mtext->rotation)))] +=
                        1;
                }
            }
            std::cout << "\n  MText by rotation:\n";
            for (const auto& [degrees, count] : textAngles) {
                std::cout << "    " << std::right << std::setw(5) << degrees << "deg  " << count
                          << '\n';
            }

            // What a dimension *says* is the real test of the reader: the
            // points can be right while the style, the units or the override
            // rules are wrong, and the text is where that shows.
            std::cout << "\n  dimension styles:\n";
            for (const cad::Handle handle : db.dimStyles().handles()) {
                const auto* style = db.get<cad::DimStyle>(handle);
                if (style == nullptr) {
                    continue;
                }
                std::cout << "    " << std::left << std::setw(32) << style->name
                          << " units=" << static_cast<int>(style->linearFormat)
                          << " precision=" << style->linearPrecision
                          << " scale=" << style->overallScale
                          << " textHeight=" << style->textHeight << '\n';
            }

            std::cout << "\n  dimensions:\n";
            for (const cad::Entity* entity : loaded.database->entitiesIn(loaded.database->modelSpace())) {
                const auto* dimension = dynamic_cast<const cad::DimensionEntity*>(entity);
                if (dimension == nullptr) {
                    continue;
                }
                const cad::DimStyle style = db.effectiveDimStyle(*dimension);
                cad::DimStyle base;
                base.restoreFrom(db.dimStyleFor(*dimension));
                const std::size_t overrides = cad::applyDimStyleOverrides(base, *dimension);
                std::cout << "    type=" << static_cast<int>(dimension->type)
                          << " measured=" << dimension->measure() << " reads \""
                          << dimension->measurementText(style) << "\" (style " << style.name;
                if (overrides > 0) {
                    std::cout << ", " << overrides << " override(s)";
                }
                std::cout << ")\n";
            }

            std::cout << "\n  hatches:\n";
            for (const cad::Entity* entity : loaded.database->entitiesIn(loaded.database->modelSpace())) {
                const auto* hatch = dynamic_cast<const cad::HatchEntity*>(entity);
                if (hatch == nullptr) {
                    continue;
                }
                std::size_t edges = 0;
                for (const cad::HatchBoundaryPath& path : hatch->paths) {
                    edges += path.edges.size() + path.vertices.size();
                }
                const std::size_t generated =
                    hatch->isSolidFill() ? 0 : hatch->generateLines(hatch->bounds(), 200000).size();
                std::cout << "    " << std::left << std::setw(14) << hatch->pattern.name
                          << (hatch->isSolidFill() ? "solid " : "      ")
                          << hatch->paths.size() << " path(s), " << edges << " edge(s), "
                          << generated << " generated line(s)\n";
            }
        }

        std::size_t warnings = 0;
        for (const cad::io::DxfDiagnostic& diagnostic : loaded.diagnostics) {
            if (diagnostic.severity == cad::io::DxfDiagnostic::Severity::Warning) {
                if (warnings < 15) {
                    std::cout << "  warning: " << diagnostic.message << '\n';
                }
                ++warnings;
            }
        }
        if (warnings > 15) {
            std::cout << "  … and " << (warnings - 15) << " more warning(s)\n";
        }

        const std::vector<cad::DatabaseIssue> issues = db.validate();
        if (!issues.empty()) {
            std::cout << "  validation: " << issues.size() << " issue(s)\n";
            for (std::size_t i = 0; i < issues.size() && i < 10; ++i) {
                std::cout << "    " << issues[i].message << '\n';
            }
        }
        return 0;
    } catch (const cad::io::TranslationError& error) {
        std::cerr << "\ntranslation failed: " << error.what() << '\n';
        if (!error.remedy().empty()) {
            std::cerr << "  " << error.remedy() << '\n';
        }
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "\nfailed: " << error.what() << '\n';
        return 1;
    }
}
