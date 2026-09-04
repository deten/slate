/// Scene-building benchmark.
///
/// ADR 0003 deferred the GPU backend pending profiling, on the argument that
/// the determinants of large-drawing speed — the spatial index, level of
/// detail, view-dependent tessellation — are backend-independent and worth
/// building first. That argument has been repeated for a while without
/// evidence. This measures it.
///
/// What is measured here is everything up to the draw call: index rebuild, and
/// scene building at several zoom levels and entity counts. That is the part
/// that lives in `cad_render` and would be *unchanged* by a GPU backend, so it
/// is also the part that decides whether a GPU backend would help.
///
///     bench_scene [--max 500000] [--csv]

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "cad/db/CurveEntities.hpp"
#include "cad/db/Database.hpp"
#include "cad/db/InsertEntity.hpp"
#include "cad/db/TextEntities.hpp"
#include "cad/render/Scene.hpp"

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] double millisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>{Clock::now() - start}.count();
}

/// A drawing shaped like a real one rather than a uniform field of lines.
///
/// The mix matters: a benchmark of 100,000 identical short lines measures the
/// spatial index and nothing else. Real drawings are mostly polylines and block
/// references, with text and circles scattered through, and blocks are the case
/// that makes or breaks the culling strategy because one reference expands into
/// many entities.
[[nodiscard]] std::unique_ptr<cad::Database> makeDrawing(std::size_t entityCount) {
    auto db = cad::Database::createEmpty();

    // A small block, referenced many times — the pattern that dominates a real
    // drawing's entity count (bolts, fixtures, furniture, grid bubbles).
    auto blockRecord = std::make_unique<cad::BlockTableRecord>();
    blockRecord->name = "WIDGET";
    cad::BlockTableRecord& widget = db->addBlock(std::move(blockRecord));
    for (int i = 0; i < 8; ++i) {
        const double angle = static_cast<double>(i) * cad::kTau / 8.0;
        db->addEntity(std::make_unique<cad::LineEntity>(
                          cad::Vec3::zero(), cad::Vec3::fromAngle(angle, 5.0)),
                      widget);
    }

    // Layers, so property resolution does real work rather than hitting one
    // cached answer.
    std::vector<cad::Handle> layers;
    for (int i = 0; i < 12; ++i) {
        layers.push_back(db->ensureLayer("Layer" + std::to_string(i)).handle);
    }

    // Deterministic, so two runs are comparable.
    std::mt19937 rng{20260812};
    // A square site: 100,000 entities over 200x200 metres is a plausible
    // density for a large drawing at millimetre scale.
    const double extent = 200000.0;
    std::uniform_real_distribution<double> position{0.0, extent};
    std::uniform_real_distribution<double> small{50.0, 2000.0};

    db->bulkLoad([&] {
        for (std::size_t i = 0; i < entityCount; ++i) {
            const cad::Vec3 at{position(rng), position(rng), 0.0};
            std::unique_ptr<cad::Entity> entity;

            switch (i % 10) {
                case 0:
                case 1:
                case 2:
                case 3: {
                    // Polylines: the workhorse of real drawings.
                    std::vector<cad::Vec3> points;
                    for (int p = 0; p < 5; ++p) {
                        points.push_back(at + cad::Vec3{small(rng), small(rng), 0.0});
                    }
                    entity = std::make_unique<cad::PolylineEntity>(
                        cad::PolylineEntity::fromPoints(points, i % 8 == 0));
                    break;
                }
                case 4:
                case 5:
                    entity = std::make_unique<cad::LineEntity>(
                        at, at + cad::Vec3{small(rng), small(rng), 0.0});
                    break;
                case 6: {
                    auto insert = std::make_unique<cad::InsertEntity>();
                    insert->position = at;
                    insert->blockHandle = widget.handle;
                    entity = std::move(insert);
                    break;
                }
                case 7:
                    entity = std::make_unique<cad::CircleEntity>(at, small(rng) / 4.0);
                    break;
                case 8:
                    entity = std::make_unique<cad::ArcEntity>(at, small(rng) / 4.0, 0.0,
                                                              cad::kPi * 1.2);
                    break;
                default:
                    entity = std::make_unique<cad::TextEntity>(at, "NOTE 12", 250.0);
                    break;
            }

            entity->layerHandle = layers[i % layers.size()];
            db->addToModelSpace(std::move(entity));
        }
        return 0;
    });
    return db;
}

struct Measurement {
    double reindexMs = 0.0;
    /// Spatial index query alone.
    double searchMs = 0.0;
    /// Everything: query, culling, property resolution, tessellation.
    double buildMs = 0.0;
    /// Tessellating the same entities into throwaway buffers, with no
    /// DrawItem construction. Separates the geometry cost from the
    /// per-item allocation cost, which is the question a GPU backend
    /// would or would not answer.
    double tessellateMs = 0.0;
    std::size_t items = 0;
    std::size_t culled = 0;
};

[[nodiscard]] Measurement measure(cad::Database& db, cad::render::SpatialIndex& index,
                                  const cad::Box3& view, int repeats) {
    cad::render::SceneBuilder builder{db, index};

    const Clock::time_point indexStart = Clock::now();
    builder.reindex(db.modelSpace());
    Measurement result;
    result.reindexMs = millisecondsSince(indexStart);

    const cad::render::Viewport viewport =
        cad::render::Viewport{}.withSize(1920, 1080).zoomToFit(view);

    // Best of N. A frame budget is about the *typical* frame, and the first
    // build pays for cold caches that a running application would not.
    double best = std::numeric_limits<double>::infinity();
    for (int i = 0; i < repeats; ++i) {
        const Clock::time_point start = Clock::now();
        const cad::render::Scene scene = builder.build(db.modelSpace(), viewport);
        best = std::min(best, millisecondsSince(start));
        result.items = scene.items.size();
        result.culled = scene.culled;
    }
    result.buildMs = best;

    // The index query on its own.
    double searchBest = std::numeric_limits<double>::infinity();
    std::vector<cad::Handle> candidates;
    for (int i = 0; i < repeats; ++i) {
        const Clock::time_point start = Clock::now();
        candidates = index.search(viewport.visibleBounds());
        searchBest = std::min(searchBest, millisecondsSince(start));
    }
    result.searchMs = searchBest;

    // Tessellation of the same candidates, into a reused buffer. Whatever this
    // does *not* account for is per-item overhead: DrawItem construction,
    // the vector-of-vectors, and the allocations behind them.
    cad::TessellationOptions tessellation;
    tessellation.chordTolerance = cad::render::chordToleranceFor(viewport);
    tessellation.minSegments = 2;

    double tessellateBest = std::numeric_limits<double>::infinity();
    for (int i = 0; i < repeats; ++i) {
        std::vector<cad::Vec3> scratch;
        const Clock::time_point start = Clock::now();
        for (const cad::Handle handle : candidates) {
            const cad::Entity* entity = db.getEntity(handle);
            if (entity == nullptr) {
                continue;
            }
            for (const cad::CurvePtr& curve : entity->toCurves()) {
                scratch = curve->tessellate(tessellation);
            }
        }
        tessellateBest = std::min(tessellateBest, millisecondsSince(start));
    }
    result.tessellateMs = tessellateBest;
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t maximum = 500000;
    bool csv = false;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--max" && i + 1 < argc) {
            maximum = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (argument == "--csv") {
            csv = true;
        }
    }

    if (csv) {
        std::cout << "entities,view,reindex_ms,search_ms,tessellate_ms,build_ms,items,culled\n";
    } else {
        std::cout << "Scene building, 1920x1080, best of 5 (ADR 0003 profiling)\n\n"
                  << std::left << std::setw(10) << "entities" << std::setw(12) << "view"
                  << std::right << std::setw(11) << "reindex" << std::setw(10) << "search" << std::setw(11) << "tessell." << std::setw(10) << "build" << std::setw(10) << "items" << std::setw(9) << "culled" << '\n'
                  << std::string(73, '-') << '\n';
    }

    for (const std::size_t count : {std::size_t{1000}, std::size_t{10000}, std::size_t{50000},
                                    std::size_t{100000}, std::size_t{250000},
                                    std::size_t{500000}}) {
        if (count > maximum) {
            break;
        }
        std::unique_ptr<cad::Database> db = makeDrawing(count);
        cad::render::SpatialIndex index;

        const cad::Box3 whole = db->modelExtents();
        // Three views, because they exercise different bottlenecks. Zoomed out
        // is level of detail and sheer volume; zoomed in is the spatial index
        // earning its keep; mid is the realistic working case.
        const cad::Vec3 middle = whole.min.midpoint(whole.max);
        const cad::Box3 room =
            cad::Box3::fromCorners(middle - cad::Vec3{5000.0, 5000.0, 0.0},
                                   middle + cad::Vec3{5000.0, 5000.0, 0.0});
        const cad::Box3 wing =
            cad::Box3::fromCorners(middle - cad::Vec3{50000.0, 50000.0, 0.0},
                                   middle + cad::Vec3{50000.0, 50000.0, 0.0});

        for (const auto& [name, view] :
             std::vector<std::pair<const char*, cad::Box3>>{
                 {"extents", whole}, {"wing", wing}, {"room", room}}) {
            const Measurement m = measure(*db, index, view, 5);
            if (csv) {
                std::cout << count << ',' << name << ',' << m.reindexMs << ',' << m.searchMs << ','
                          << m.tessellateMs << ',' << m.buildMs << ',' << m.items << ','
                          << m.culled << '\n';
            } else {
                std::cout << std::left << std::setw(10) << count << std::setw(10) << name
                          << std::right << std::fixed << std::setprecision(1) << std::setw(11)
                          << m.reindexMs << std::setw(10) << m.searchMs << std::setw(11)
                          << m.tessellateMs << std::setw(10) << m.buildMs << std::setw(10)
                          << m.items << std::setw(9) << m.culled << '\n';
            }
        }
    }
    return 0;
}
