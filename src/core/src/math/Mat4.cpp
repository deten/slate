#include "cad/math/Mat4.hpp"

namespace cad {

Mat4 Mat4::rotationAboutAxis(const Vec3& axis, double angle, const Vec3& origin) {
    const Vec3 a = axis.normalized();
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    const double t = 1.0 - c;

    const Mat4 r{{t * a.x * a.x + c, t * a.x * a.y + s * a.z, t * a.x * a.z - s * a.y, 0.0,
                  t * a.x * a.y - s * a.z, t * a.y * a.y + c, t * a.y * a.z + s * a.x, 0.0,
                  t * a.x * a.z + s * a.y, t * a.y * a.z - s * a.x, t * a.z * a.z + c, 0.0,
                  0.0, 0.0, 0.0, 1.0}};

    if (origin.isZero()) {
        return r;
    }
    return translation(origin).multiply(r).multiply(translation(-origin));
}

Mat4 Mat4::mirror2(const Vec3& point, const Vec3& direction) {
    const Vec3 d = direction.flattened().normalized();
    // Householder reflection about the line through the origin, then
    // re-anchored at `point`.
    const double a = d.x * d.x - d.y * d.y;
    const double b = 2.0 * d.x * d.y;
    const Mat4 reflect{{a, b, 0, 0, b, -a, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
    return translation(point).multiply(reflect).multiply(translation(-point));
}

Mat4 Mat4::mirrorPlane(const Vec3& point, const Vec3& normal) {
    const Vec3 n = normal.normalized();
    const Mat4 reflect{{1.0 - 2.0 * n.x * n.x, -2.0 * n.x * n.y, -2.0 * n.x * n.z, 0.0,
                        -2.0 * n.x * n.y, 1.0 - 2.0 * n.y * n.y, -2.0 * n.y * n.z, 0.0,
                        -2.0 * n.x * n.z, -2.0 * n.y * n.z, 1.0 - 2.0 * n.z * n.z, 0.0,
                        0.0, 0.0, 0.0, 1.0}};
    return translation(point).multiply(reflect).multiply(translation(-point));
}

Mat4 Mat4::multiply(const Mat4& other) const noexcept {
    const auto& a = m;
    const auto& b = other.m;
    std::array<double, 16> out{};

    for (int col = 0; col < 4; ++col) {
        const double b0 = b[static_cast<std::size_t>(col) * 4 + 0];
        const double b1 = b[static_cast<std::size_t>(col) * 4 + 1];
        const double b2 = b[static_cast<std::size_t>(col) * 4 + 2];
        const double b3 = b[static_cast<std::size_t>(col) * 4 + 3];
        for (int row = 0; row < 4; ++row) {
            const auto r = static_cast<std::size_t>(row);
            out[static_cast<std::size_t>(col) * 4 + r] =
                a[r] * b0 + a[4 + r] * b1 + a[8 + r] * b2 + a[12 + r] * b3;
        }
    }
    return Mat4{out};
}

bool Mat4::isConformal(const Tolerance& tol) const noexcept {
    const Vec3 cx{m[0], m[1], m[2]};
    const Vec3 cy{m[4], m[5], m[6]};
    const Vec3 cz{m[8], m[9], m[10]};

    const double lx = cx.length();
    const double ly = cy.length();
    const double lz = cz.length();

    // All three basis vectors must have equal length.
    if (std::abs(lx - ly) > tol.point * std::max(1.0, lx)) {
        return false;
    }
    if (std::abs(lx - lz) > tol.point * std::max(1.0, lx)) {
        return false;
    }
    // And remain mutually orthogonal.
    return std::abs(cx.dot(cy)) <= tol.point * lx * ly + tol.point &&
           std::abs(cx.dot(cz)) <= tol.point * lx * lz + tol.point &&
           std::abs(cy.dot(cz)) <= tol.point * ly * lz + tol.point;
}

Mat4 Mat4::transposed() const noexcept {
    std::array<double, 16> out{};
    for (std::size_t c = 0; c < 4; ++c) {
        for (std::size_t r = 0; r < 4; ++r) {
            out[c * 4 + r] = m[r * 4 + c];
        }
    }
    return Mat4{out};
}

std::optional<Mat4> Mat4::inverted() const noexcept {
    std::array<double, 16> inv{};

    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
             m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
             m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
             m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
              m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
             m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
             m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
             m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
              m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
             m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
             m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
              m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
              m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
             m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
             m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
              m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
              m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0 || !std::isfinite(det)) {
        return std::nullopt;
    }

    const double invDet = 1.0 / det;
    for (auto& value : inv) {
        value *= invDet;
    }
    return Mat4{inv};
}

bool Mat4::isIdentity(const Tolerance& tol) const noexcept {
    const Mat4 id;
    return equals(id, tol);
}

bool Mat4::equals(const Mat4& other, const Tolerance& tol) const noexcept {
    for (std::size_t i = 0; i < 16; ++i) {
        if (std::abs(m[i] - other.m[i]) > tol.point) {
            return false;
        }
    }
    return true;
}

}  // namespace cad
