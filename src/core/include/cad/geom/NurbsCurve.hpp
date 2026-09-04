#pragma once

#include <memory>
#include <vector>

#include "cad/geom/Curve.hpp"
#include "cad/geom/Polyline.hpp"

namespace cad {

/// A NURBS curve: the SPLINE entity's geometry.
///
/// Evaluation is exact -- de Boor's algorithm over the knot vector, with
/// homogeneous coordinates when the curve is rational. What is *not* exact is
/// everything that has no closed form for a NURBS: arc length, the closest
/// point, and trimming. Those are answered from a finely tessellated copy, and
/// that is a deliberate choice rather than an oversight. A cubic's arc length
/// has no elementary antiderivative, so every kernel integrates or samples it;
/// pretending otherwise would mean inventing a number.
///
/// The parameter `t` of the `Curve` interface is normalized to `[0, 1]` and
/// maps onto the curve's own knot domain, which for a clamped curve is
/// `[knots[degree], knots[count]]` -- not the whole knot vector. Using the
/// whole vector would evaluate outside the curve's support and return the
/// origin for the first and last spans.
class NurbsCurve final : public Curve {
public:
    /// The de Boor points. At least `degree + 1` of them, or the curve is
    /// degenerate and evaluates to nothing.
    std::vector<Vec3> controlPoints;

    /// One per control point, or empty for a non-rational curve. Empty is not
    /// the same as all-ones to a reader: it decides whether weights are
    /// written back out, and a file that had none should get none.
    std::vector<double> weights;

    /// Non-decreasing, and `controlPoints.size() + degree + 1` long.
    std::vector<double> knots;

    int degree = 3;

    /// Set from the SPLINE flags. Affects the reported closedness only; the
    /// wrapped control points a closed spline carries do the real work.
    bool closed = false;

    NurbsCurve() = default;
    NurbsCurve(std::vector<Vec3> points, std::vector<double> knotVector, int degreeValue);

    // Spelled out because of the sampled cache: a `unique_ptr` member would
    // otherwise delete the copy constructor, and a curve has to be copyable.
    // The cache is deliberately not carried over -- it is derived data, and the
    // copy rebuilds it on demand.
    NurbsCurve(const NurbsCurve& other);
    NurbsCurve& operator=(const NurbsCurve& other);
    NurbsCurve(NurbsCurve&&) noexcept = default;
    NurbsCurve& operator=(NurbsCurve&&) noexcept = default;
    ~NurbsCurve() override = default;

    /// True when the data describes a curve that can be evaluated at all:
    /// enough control points for the degree, and a knot vector of the length
    /// the degree and count imply. A file can contain neither.
    [[nodiscard]] bool isValid() const;

    /// The parameter range the curve is actually defined over.
    [[nodiscard]] double domainStart() const;
    [[nodiscard]] double domainEnd() const;

    /// Position at a parameter in *knot space*, by de Boor's algorithm.
    [[nodiscard]] Vec3 evaluate(double u) const;

    /// Derivative with respect to the knot parameter, by differencing. A NURBS
    /// derivative has a closed form, but the rational quotient rule makes it
    /// long, and every caller here normalizes the result and discards the
    /// magnitude.
    [[nodiscard]] Vec3 derivative(double u) const;

    [[nodiscard]] CurvePtr clone() const override { return std::make_unique<NurbsCurve>(*this); }

    [[nodiscard]] Vec3 startPoint() const override;
    [[nodiscard]] Vec3 endPoint() const override;
    [[nodiscard]] bool isClosed() const override;
    [[nodiscard]] Vec3 pointAt(double t) const override;
    [[nodiscard]] Vec3 tangentAt(double t) const override;
    [[nodiscard]] double length() const override;
    [[nodiscard]] Box3 bounds() const override;
    [[nodiscard]] ClosestPointResult closestPoint(const Vec3& p) const override;
    [[nodiscard]] std::optional<ClosestPointResult> closestPointExtended(
        const Vec3& p) const override;
    [[nodiscard]] CurvePtr transformed(const Mat4& m) const override;
    [[nodiscard]] CurvePtr reversed() const override;
    [[nodiscard]] std::vector<Vec3> tessellate(
        const TessellationOptions& options = {}) const override;
    [[nodiscard]] CurvePtr trimmed(double t0, double t1) const override;

private:
    /// A polyline approximation, fine enough that measuring against it does not
    /// show. Built once on demand: the queries that need it are the expensive
    /// ones, and a curve is often only ever drawn.
    [[nodiscard]] const Polyline& sampled() const;

    mutable std::unique_ptr<Polyline> sampled_;
};

}  // namespace cad
