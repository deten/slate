#include "cad/snap/Tracking.hpp"

#include <algorithm>
#include <cmath>

namespace cad {
namespace {

/// One alignment path, already resolved to the angle nearest the cursor.
struct Ray {
    Vec3 origin;
    Vec3 direction;
    Vec3 point;
    double across = 0.0;
};

}  // namespace

std::optional<TrackingAlignment> alignToAcquired(std::span<const Vec3> acquired,
                                                 const Vec3& cursor, double angleIncrement,
                                                 double reach) {
    if (acquired.empty() || angleIncrement < 1e-9 || reach <= 0.0) {
        return std::nullopt;
    }

    std::vector<Ray> rays;
    rays.reserve(acquired.size());
    for (const Vec3& origin : acquired) {
        const double dx = cursor.x - origin.x;
        const double dy = cursor.y - origin.y;
        // Sitting on the acquired point itself: there is no direction to align
        // along, and every path passes through it.
        if (std::hypot(dx, dy) < reach) {
            continue;
        }
        const double snapped =
            std::round(std::atan2(dy, dx) / angleIncrement) * angleIncrement;
        const Vec3 direction{std::cos(snapped), std::sin(snapped), 0.0};
        const double along = dx * direction.x + dy * direction.y;
        if (along <= 0.0) {
            continue;
        }
        const double across = std::abs(dx * -direction.y + dy * direction.x);
        if (across >= reach) {
            continue;
        }
        rays.push_back(Ray{origin,
                           direction,
                           Vec3{origin.x + along * direction.x, origin.y + along * direction.y,
                                origin.z},
                           across});
    }
    if (rays.empty()) {
        return std::nullopt;
    }

    // Two paths crossing beat either one alone.
    for (std::size_t i = 0; i < rays.size(); ++i) {
        for (std::size_t j = i + 1; j < rays.size(); ++j) {
            const Ray& a = rays[i];
            const Ray& b = rays[j];
            const double cross = a.direction.x * b.direction.y - a.direction.y * b.direction.x;
            // Parallel paths have no crossing, or every point in common.
            if (std::abs(cross) < 1e-9) {
                continue;
            }
            const double ox = b.origin.x - a.origin.x;
            const double oy = b.origin.y - a.origin.y;
            const double t = (ox * b.direction.y - oy * b.direction.x) / cross;
            const Vec3 meeting{a.origin.x + t * a.direction.x, a.origin.y + t * a.direction.y,
                               a.origin.z};
            // Only where the pointer already is. Jumping to a distant crossing
            // because two paths happen to meet somewhere would put the point
            // where nobody pointed.
            if (std::hypot(meeting.x - cursor.x, meeting.y - cursor.y) >= reach) {
                continue;
            }
            return TrackingAlignment{meeting, {a.origin, b.origin}};
        }
    }

    const Ray& nearest =
        *std::min_element(rays.begin(), rays.end(),
                          [](const Ray& a, const Ray& b) { return a.across < b.across; });
    return TrackingAlignment{nearest.point, {nearest.origin}};
}

}  // namespace cad
