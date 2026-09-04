#include "cad/geom/NurbsCurve.hpp"

#include <algorithm>
#include <cmath>

namespace cad {
namespace {

/// How finely the curve is sampled for the queries that have no closed form.
///
/// Per knot span, so a curve with many spans is sampled in proportion rather
/// than being smeared over a fixed budget.
constexpr int kSamplesPerSpan = 24;

/// Step used to difference the derivative, as a fraction of the domain. Large
/// enough not to be lost to cancellation, small enough that the direction is
/// right well inside a span.
constexpr double kDerivativeStep = 1e-6;

}  // namespace

NurbsCurve::NurbsCurve(std::vector<Vec3> points, std::vector<double> knotVector, int degreeValue)
    : controlPoints(std::move(points)), knots(std::move(knotVector)), degree(degreeValue) {}

NurbsCurve::NurbsCurve(const NurbsCurve& other)
    : controlPoints(other.controlPoints),
      weights(other.weights),
      knots(other.knots),
      degree(other.degree),
      closed(other.closed) {}

NurbsCurve& NurbsCurve::operator=(const NurbsCurve& other) {
    if (this != &other) {
        controlPoints = other.controlPoints;
        weights = other.weights;
        knots = other.knots;
        degree = other.degree;
        closed = other.closed;
        sampled_.reset();
    }
    return *this;
}

bool NurbsCurve::isValid() const {
    if (degree < 1 || controlPoints.size() < static_cast<std::size_t>(degree) + 1) {
        return false;
    }
    if (knots.size() != controlPoints.size() + static_cast<std::size_t>(degree) + 1) {
        return false;
    }
    // Weights are optional, but a partial set is a file error rather than a
    // non-rational curve, and guessing which points the given ones belong to
    // would be inventing geometry.
    return weights.empty() || weights.size() == controlPoints.size();
}

double NurbsCurve::domainStart() const {
    return isValid() ? knots[static_cast<std::size_t>(degree)] : 0.0;
}

double NurbsCurve::domainEnd() const {
    return isValid() ? knots[controlPoints.size()] : 0.0;
}

Vec3 NurbsCurve::evaluate(double u) const {
    if (!isValid()) {
        return Vec3::zero();
    }
    const double first = domainStart();
    const double last = domainEnd();
    u = std::clamp(u, first, last);

    const std::size_t count = controlPoints.size();
    const auto order = static_cast<std::size_t>(degree);

    // The span containing u. Clamped to the final span so that u == last
    // evaluates on the curve rather than one span past its end.
    std::size_t span = count - 1;
    for (std::size_t i = order; i < count; ++i) {
        if (u >= knots[i] && u < knots[i + 1]) {
            span = i;
            break;
        }
    }

    // De Boor, in homogeneous coordinates so the rational and polynomial cases
    // are one piece of code: a non-rational curve is the same algorithm with
    // every weight at one.
    std::vector<Vec3> points(order + 1);
    std::vector<double> w(order + 1);
    for (std::size_t i = 0; i <= order; ++i) {
        const std::size_t index = span - order + i;
        const double weight = weights.empty() ? 1.0 : weights[index];
        points[i] = controlPoints[index] * weight;
        w[i] = weight;
    }

    for (std::size_t r = 1; r <= order; ++r) {
        for (std::size_t i = order; i >= r; --i) {
            const std::size_t index = span - order + i;
            const double lower = knots[index];
            const double upper = knots[index + order - r + 1];
            const double denominator = upper - lower;
            // A repeated knot collapses the interval; the point on that side is
            // already the answer, and dividing would be a division by zero.
            const double alpha = denominator <= 0.0 ? 0.0 : (u - lower) / denominator;
            points[i] = points[i - 1] * (1.0 - alpha) + points[i] * alpha;
            w[i] = w[i - 1] * (1.0 - alpha) + w[i] * alpha;
        }
    }

    if (std::abs(w[order]) < 1e-12) {
        return Vec3::zero();
    }
    return points[order] * (1.0 / w[order]);
}

Vec3 NurbsCurve::derivative(double u) const {
    const double first = domainStart();
    const double last = domainEnd();
    const double span = last - first;
    if (span <= 0.0) {
        return Vec3::unitX();
    }
    const double step = span * kDerivativeStep;
    // One-sided at the ends, so the difference never reaches outside the
    // domain, where evaluation clamps and would report no motion at all.
    const double a = std::clamp(u - step, first, last);
    const double b = std::clamp(u + step, first, last);
    if (b - a <= 0.0) {
        return Vec3::unitX();
    }
    return (evaluate(b) - evaluate(a)) * (1.0 / (b - a));
}

Vec3 NurbsCurve::startPoint() const { return evaluate(domainStart()); }

Vec3 NurbsCurve::endPoint() const { return evaluate(domainEnd()); }

bool NurbsCurve::isClosed() const {
    return closed || (isValid() && startPoint().equals(endPoint()));
}

Vec3 NurbsCurve::pointAt(double t) const {
    const double clamped = std::clamp(t, 0.0, 1.0);
    return evaluate(domainStart() + clamped * (domainEnd() - domainStart()));
}

Vec3 NurbsCurve::tangentAt(double t) const {
    const double clamped = std::clamp(t, 0.0, 1.0);
    return derivative(domainStart() + clamped * (domainEnd() - domainStart()));
}

std::vector<Vec3> NurbsCurve::tessellate(const TessellationOptions& options) const {
    std::vector<Vec3> points;
    if (!isValid()) {
        return points;
    }
    const double first = domainStart();
    const double last = domainEnd();
    if (last <= first) {
        points.push_back(startPoint());
        return points;
    }

    // Sampled per span rather than over the whole domain: the shape changes
    // where the knots are, and a fixed budget starves a long curve with many
    // spans while wasting samples on a short one.
    const auto spans = static_cast<int>(controlPoints.size()) - degree;
    int steps = std::max(spans, 1) * kSamplesPerSpan;
    steps = std::max(steps, options.minSegments);
    steps = std::min(steps, 4096);

    points.reserve(static_cast<std::size_t>(steps) + 1);
    for (int i = 0; i <= steps; ++i) {
        const double u = first + (last - first) * (static_cast<double>(i) / steps);
        points.push_back(evaluate(u));
    }
    // Exactly the endpoints, which the interface promises and which arithmetic
    // on the parameter does not otherwise guarantee.
    points.front() = startPoint();
    points.back() = endPoint();
    return points;
}

const Polyline& NurbsCurve::sampled() const {
    if (sampled_ == nullptr) {
        TessellationOptions fine;
        fine.minSegments = 64;
        sampled_ = std::make_unique<Polyline>(Polyline::fromPoints(tessellate(fine)));
    }
    return *sampled_;
}

double NurbsCurve::length() const { return isValid() ? sampled().length() : 0.0; }

Box3 NurbsCurve::bounds() const {
    Box3 box;
    if (!isValid()) {
        return box;
    }
    // From the sampled curve, not from the control polygon. The control hull
    // does bound the curve, but loosely and often by a wide margin -- and
    // these bounds drive culling, zoom extents and the spatial index.
    for (const Vec3& point : tessellate()) {
        box = box.united(Box3::fromPoint(point));
    }
    return box;
}

ClosestPointResult NurbsCurve::closestPoint(const Vec3& p) const {
    if (!isValid()) {
        return ClosestPointResult{Vec3::zero(), 0.0, 0.0};
    }
    ClosestPointResult result = sampled().closestPoint(p);

    // The sampled polyline says roughly where; the answer is then refined
    // against the real curve, which is what keeps a snap on the curve rather
    // than on the chord across it.
    const double first = domainStart();
    const double last = domainEnd();
    const auto distanceAt = [&](double t) {
        return evaluate(first + t * (last - first)).distanceTo(p);
    };

    double best = result.param;
    double window =
        1.0 / static_cast<double>(std::max<std::size_t>(sampled().vertexCount(), 2));
    for (int pass = 0; pass < 40; ++pass) {
        const double low = std::clamp(best - window, 0.0, 1.0);
        const double high = std::clamp(best + window, 0.0, 1.0);
        const double dLow = distanceAt(low);
        const double dBest = distanceAt(best);
        const double dHigh = distanceAt(high);
        if (dLow < dBest && dLow <= dHigh) {
            best = low;
        } else if (dHigh < dBest) {
            best = high;
        }
        window *= 0.5;
    }

    result.param = best;
    result.point = evaluate(first + best * (last - first));
    result.distance = result.point.distanceTo(p);
    return result;
}

std::optional<ClosestPointResult> NurbsCurve::closestPointExtended(const Vec3&) const {
    // A NURBS has no natural extension: past its last knot it is undefined, and
    // continuing the final polynomial diverges from anything a drafter would
    // call the same curve. Reporting none is the honest answer, and EXTEND
    // declines rather than inventing a direction.
    return std::nullopt;
}

CurvePtr NurbsCurve::transformed(const Mat4& m) const {
    auto copy = std::make_unique<NurbsCurve>(*this);
    copy->sampled_.reset();
    // Control points transform and the curve follows, because a NURBS is an
    // affine combination of them. That property is exactly why the format
    // stores control points rather than sampled ones.
    for (Vec3& point : copy->controlPoints) {
        point = m.transformPoint(point);
    }
    return copy;
}

CurvePtr NurbsCurve::reversed() const {
    auto copy = std::make_unique<NurbsCurve>(*this);
    copy->sampled_.reset();
    std::reverse(copy->controlPoints.begin(), copy->controlPoints.end());
    if (!copy->weights.empty()) {
        std::reverse(copy->weights.begin(), copy->weights.end());
    }
    // The knot vector reflects about its own range, which keeps it
    // non-decreasing and the parameterization consistent with the points.
    const double first = knots.front();
    const double last = knots.back();
    std::vector<double> flipped;
    flipped.reserve(knots.size());
    for (auto it = knots.rbegin(); it != knots.rend(); ++it) {
        flipped.push_back(first + last - *it);
    }
    copy->knots = std::move(flipped);
    return copy;
}

CurvePtr NurbsCurve::trimmed(double t0, double t1) const {
    // Returned as a polyline. Trimming a NURBS properly means inserting knots
    // until the curve splits, and the result would still be a NURBS the caller
    // could not tell apart from a sampled one. Returning the approximation
    // openly beats returning a curve that claims to be exact.
    const double a = std::clamp(std::min(t0, t1), 0.0, 1.0);
    const double b = std::clamp(std::max(t0, t1), 0.0, 1.0);
    const double first = domainStart();
    const double last = domainEnd();

    constexpr int kSteps = 96;
    std::vector<Vec3> points;
    points.reserve(kSteps + 1);
    for (int i = 0; i <= kSteps; ++i) {
        const double t = a + (b - a) * (static_cast<double>(i) / kSteps);
        points.push_back(evaluate(first + t * (last - first)));
    }
    return std::make_unique<Polyline>(Polyline::fromPoints(points));
}

}  // namespace cad
