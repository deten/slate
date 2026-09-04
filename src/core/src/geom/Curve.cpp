#include "cad/geom/Curve.hpp"

#include <algorithm>

namespace cad {

std::vector<double> sanitizeSplitParams(std::span<const double> params, double eps) {
    std::vector<double> sorted;
    sorted.reserve(params.size());
    for (const double t : params) {
        if (std::isfinite(t) && t > eps && t < 1.0 - eps) {
            sorted.push_back(t);
        }
    }
    std::sort(sorted.begin(), sorted.end());

    std::vector<double> out;
    out.reserve(sorted.size());
    for (const double t : sorted) {
        if (out.empty() || t - out.back() > eps) {
            out.push_back(t);
        }
    }
    return out;
}

std::vector<CurvePtr> Curve::split(std::span<const double> params) const {
    const std::vector<double> cuts = sanitizeSplitParams(params);

    std::vector<CurvePtr> result;
    if (cuts.empty()) {
        result.push_back(clone());
        return result;
    }

    result.reserve(cuts.size() + 1);
    double previous = 0.0;
    for (const double t : cuts) {
        result.push_back(trimmed(previous, t));
        previous = t;
    }
    result.push_back(trimmed(previous, 1.0));
    return result;
}

}  // namespace cad
