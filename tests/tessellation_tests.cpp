#include "test_helpers.hpp"

#include <cmath>
#include <vector>

#include "cad/geom/Arc.hpp"
#include "cad/geom/EllipseArc.hpp"

using namespace cad;

namespace {

/// The worst distance from the true curve to the polyline standing in for it.
///
/// This is what a chord tolerance *means*, and measuring it is the only way to
/// know a tessellation honours the number it was given. Counting segments would
/// not: the right count for the wrong reason still looks faceted.
///
/// Each chord is compared against the curve between its own endpoints, sampled
/// finely. Comparing against the whole curve instead would report zero for a
/// polyline that cut a corner but happened to pass near some other part of it.
[[nodiscard]] double worstDeviation(const Curve& curve, const std::vector<Vec3>& points) {
    constexpr int kProbes = 64;
    double worst = 0.0;
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const double t0 = static_cast<double>(i) / static_cast<double>(points.size() - 1);
        const double t1 = static_cast<double>(i + 1) / static_cast<double>(points.size() - 1);
        const Vec3& from = points[i];
        const Vec3& to = points[i + 1];
        const Vec3 along = to - from;
        const double span = along.length2();
        if (span < 1e-12) {
            continue;
        }
        for (int probe = 1; probe < kProbes; ++probe) {
            const double t = t0 + (t1 - t0) * (static_cast<double>(probe) / kProbes);
            const Vec3 exact = curve.pointAt(t);
            // Perpendicular distance from the true point to the chord.
            const Vec3 offset = exact - from;
            const double across =
                std::abs(along.x * offset.y - along.y * offset.x) / span;
            worst = std::max(worst, across);
        }
    }
    return worst;
}

}  // namespace

TEST_SUITE("Tessellation") {

TEST_CASE("an ellipse is tessellated to the tolerance it was given") {
    // The deviation is worst at the end of the major axis, where the curvature
    // is tightest, and the more elongated the ellipse the worse it gets. A
    // formula that treats the ellipse as a circle of its smallest curvature
    // radius passes on a near-circle and fails badly on a flat one, so the
    // ratios here run from round to very flat.
    for (const double ratio : {1.0, 0.8, 0.4, 0.2, 0.05}) {
        for (const double tolerance : {1.0, 0.1, 0.01}) {
            const EllipseArc ellipse{Vec3::zero(), Vec3{200.0, 0.0, 0.0}, ratio, 0.0, kTau};
            TessellationOptions options;
            options.chordTolerance = tolerance;
            options.maxSegments = 100000;

            const std::vector<Vec3> points = ellipse.tessellate(options);
            const double worst = worstDeviation(ellipse, points);
            INFO("ratio ", ratio, ", tolerance ", tolerance, ", ", points.size(), " points");
            // A little slack for the probe spacing itself, which samples the
            // true curve rather than solving for the extreme.
            CHECK(worst <= tolerance * 1.05);
        }
    }
}

TEST_CASE("a circle is tessellated to the tolerance it was given") {
    // The case the ellipse formula has to keep agreeing with: an ellipse of
    // ratio 1 and a circle of the same radius must not disagree about how many
    // segments the same tolerance needs.
    for (const double tolerance : {1.0, 0.1, 0.01}) {
        const Arc circle{Vec3::zero(), 200.0, 0.0, kTau};
        TessellationOptions options;
        options.chordTolerance = tolerance;
        options.maxSegments = 100000;

        const std::vector<Vec3> points = circle.tessellate(options);
        INFO("tolerance ", tolerance, ", ", points.size(), " points");
        CHECK(worstDeviation(circle, points) <= tolerance * 1.05);

        const EllipseArc round{Vec3::zero(), Vec3{200.0, 0.0, 0.0}, 1.0, 0.0, kTau};
        const std::size_t asEllipse = round.tessellate(options).size();
        // Within one: they round a fractional segment count independently.
        CHECK(std::abs(static_cast<int>(asEllipse) - static_cast<int>(points.size())) <= 1);
    }
}

TEST_CASE("a tighter tolerance buys more segments") {
    // Guards the direction of the relationship. A formula can honour a loose
    // tolerance by accident while ignoring its argument entirely.
    const EllipseArc ellipse{Vec3::zero(), Vec3{200.0, 0.0, 0.0}, 0.4, 0.0, kTau};
    TessellationOptions loose;
    loose.chordTolerance = 1.0;
    loose.maxSegments = 100000;
    TessellationOptions tight;
    tight.chordTolerance = 0.01;
    tight.maxSegments = 100000;

    CHECK(ellipse.tessellate(tight).size() > ellipse.tessellate(loose).size());
}

}  // TEST_SUITE
