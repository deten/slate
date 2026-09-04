#pragma once

#include <array>
#include <optional>

#include "cad/math/Tolerance.hpp"
#include "cad/math/Vec3.hpp"

namespace cad {

/// An immutable 4x4 affine transformation matrix, stored column-major (the
/// layout graphics APIs expect, so render code can upload it directly).
///
/// Index layout — `m[column * 4 + row]`:
/// ```
///   m0  m4  m8  m12      Xx Yx Zx Tx
///   m1  m5  m9  m13  =   Xy Yy Zy Ty
///   m2  m6  m10 m14      Xz Yz Zz Tz
///   m3  m7  m11 m15      0  0  0  1
/// ```
///
/// 4x4 rather than a 2D 3x3: block insert transforms, UCS/ECS conversion and
/// later 3D all need the full form, and DWG's OCS extrusion handling is a 3D
/// rotation. Using 3x3 here would be a structural dead end.
class Mat4 {
public:
    /// Column-major elements.
    std::array<double, 16> m{};

    /// Constructs the identity.
    constexpr Mat4() noexcept
        : m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1} {}

    explicit constexpr Mat4(const std::array<double, 16>& columnMajor) noexcept
        : m(columnMajor) {}

    [[nodiscard]] static constexpr Mat4 identity() noexcept { return Mat4{}; }

    /// Build from three basis vectors and an origin.
    [[nodiscard]] static constexpr Mat4 fromBasis(const Vec3& xAxis, const Vec3& yAxis,
                                                  const Vec3& zAxis,
                                                  const Vec3& origin = Vec3::zero()) noexcept {
        return Mat4{{xAxis.x, xAxis.y, xAxis.z, 0.0,
                     yAxis.x, yAxis.y, yAxis.z, 0.0,
                     zAxis.x, zAxis.y, zAxis.z, 0.0,
                     origin.x, origin.y, origin.z, 1.0}};
    }

    [[nodiscard]] static constexpr Mat4 translation(const Vec3& t) noexcept {
        return Mat4{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, t.x, t.y, t.z, 1}};
    }

    /// Non-uniform scale about `center`.
    [[nodiscard]] static constexpr Mat4 scaling(const Vec3& s,
                                                const Vec3& center = Vec3::zero()) noexcept {
        return Mat4{{s.x, 0, 0, 0,
                     0, s.y, 0, 0,
                     0, 0, s.z, 0,
                     center.x * (1.0 - s.x), center.y * (1.0 - s.y), center.z * (1.0 - s.z), 1.0}};
    }

    /// Uniform scale about `center`.
    [[nodiscard]] static constexpr Mat4 scaling(double factor,
                                                const Vec3& center = Vec3::zero()) noexcept {
        return scaling(Vec3{factor, factor, factor}, center);
    }

    /// Rotation about the Z axis through `center` — the 2D drafting rotation.
    [[nodiscard]] static Mat4 rotationZ(double angle,
                                        const Vec3& center = Vec3::zero()) noexcept {
        const double c = std::cos(angle);
        const double s = std::sin(angle);
        return Mat4{{c, s, 0, 0,
                     -s, c, 0, 0,
                     0, 0, 1, 0,
                     center.x - center.x * c + center.y * s,
                     center.y - center.x * s - center.y * c,
                     0, 1}};
    }

    /// Rotation of `angle` radians about an arbitrary axis through `origin`.
    [[nodiscard]] static Mat4 rotationAboutAxis(const Vec3& axis, double angle,
                                                const Vec3& origin = Vec3::zero());

    /// Mirror across the line through `point` with direction `direction`,
    /// in the XY plane.
    [[nodiscard]] static Mat4 mirror2(const Vec3& point, const Vec3& direction);

    /// Mirror across an arbitrary plane defined by a point and a normal.
    [[nodiscard]] static Mat4 mirrorPlane(const Vec3& point, const Vec3& normal);

    /// The composite transform used by block references: scale, then rotate
    /// about Z, then translate. Order is fixed by DWG INSERT semantics.
    [[nodiscard]] static Mat4 blockTransform(const Vec3& insertion, const Vec3& scale,
                                             double rotation) noexcept {
        return translation(insertion).multiply(rotationZ(rotation)).multiply(scaling(scale));
    }

    /// `*this * other` — applies `other` first, then `*this`.
    [[nodiscard]] Mat4 multiply(const Mat4& other) const noexcept;

    [[nodiscard]] Mat4 operator*(const Mat4& other) const noexcept { return multiply(other); }

    /// Transform a position; translation applies.
    [[nodiscard]] constexpr Vec3 transformPoint(const Vec3& p) const noexcept {
        return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
                m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
    }

    /// Transform a direction; translation is ignored.
    [[nodiscard]] constexpr Vec3 transformVector(const Vec3& v) const noexcept {
        return {m[0] * v.x + m[4] * v.y + m[8] * v.z,
                m[1] * v.x + m[5] * v.y + m[9] * v.z,
                m[2] * v.x + m[6] * v.y + m[10] * v.z};
    }

    [[nodiscard]] constexpr Vec3 translationPart() const noexcept { return {m[12], m[13], m[14]}; }

    /// Determinant of the upper-left 3x3 (the linear part).
    [[nodiscard]] constexpr double determinant3() const noexcept {
        return m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) +
               m[8] * (m[1] * m[6] - m[5] * m[2]);
    }

    /// True when the transform flips handedness (an odd number of mirrors).
    [[nodiscard]] constexpr bool isMirrored() const noexcept { return determinant3() < 0.0; }

    /// True when the linear part is a uniform scale times a rotation, i.e. the
    /// transform maps circles to circles. Non-conformal transforms must convert
    /// circles and arcs to ellipses, so this predicate gates that path.
    [[nodiscard]] bool isConformal(const Tolerance& tol = defaultTolerance()) const noexcept;

    /// Uniform scale factor. For non-conformal transforms this is the geometric
    /// mean, which is the right value for scaling scalar properties such as
    /// linetype scale.
    [[nodiscard]] double scaleFactor() const noexcept { return std::cbrt(std::abs(determinant3())); }

    /// Scale factor within the XY plane.
    [[nodiscard]] double scaleFactor2() const noexcept {
        return std::sqrt(std::abs(m[0] * m[5] - m[4] * m[1]));
    }

    /// Rotation of the transformed X axis within the XY plane, in radians.
    [[nodiscard]] double rotation2() const noexcept { return std::atan2(m[1], m[0]); }

    [[nodiscard]] Mat4 transposed() const noexcept;

    /// General 4x4 inverse.
    /// @returns the inverse, or nullopt when the matrix is singular. Callers
    ///   working with user geometry must handle the empty case — a zero scale
    ///   factor is an ordinary input error, not an exceptional condition.
    [[nodiscard]] std::optional<Mat4> inverted() const noexcept;

    [[nodiscard]] bool isIdentity(const Tolerance& tol = defaultTolerance()) const noexcept;

    [[nodiscard]] bool equals(const Mat4& other,
                              const Tolerance& tol = defaultTolerance()) const noexcept;
};

}  // namespace cad
