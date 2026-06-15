#ifndef TPVCAMERA_MATH_UTILS_HPP
#define TPVCAMERA_MATH_UTILS_HPP

#include <cmath>
#include <DirectXMath.h>

namespace TPVCamera
{

    /**
     * @brief Minimal 3-component vector used by the camera math.
     * @details Plain value type with the usual arithmetic and a DirectXMath bridge. Mirrors the engine's
     *          contiguous {x, y, z} float layout so a world position or basis column can be reinterpreted in
     *          place from a Matrix34 column.
     */
    struct Vector3
    {
        float x, y, z;

        explicit Vector3(float _x = 0.0f, float _y = 0.0f, float _z = 0.0f) noexcept : x(_x), y(_y), z(_z) {}

        Vector3 operator+(const Vector3 &other) const noexcept
        {
            return Vector3(x + other.x, y + other.y, z + other.z);
        }
        Vector3 operator-(const Vector3 &other) const noexcept
        {
            return Vector3(x - other.x, y - other.y, z - other.z);
        }
        Vector3 operator*(float scalar) const noexcept { return Vector3(x * scalar, y * scalar, z * scalar); }
        // Returns the zero vector when scalar is zero to avoid a division by zero.
        Vector3 operator/(float scalar) const noexcept
        {
            return (scalar != 0.0f) ? Vector3(x / scalar, y / scalar, z / scalar) : Vector3();
        }

        /// Squared length (cheaper than magnitude(); avoids the sqrt).
        [[nodiscard]] float magnitude_squared() const noexcept { return x * x + y * y + z * z; }
        /// Euclidean length.
        [[nodiscard]] float magnitude() const noexcept { return std::sqrt(magnitude_squared()); }

        /** @brief Returns the unit vector, or the zero vector if this vector has zero length. */
        [[nodiscard]] Vector3 normalized() const noexcept
        {
            const float mag = magnitude();
            if (mag == 0.0f)
                return Vector3();
            return Vector3(x / mag, y / mag, z / mag);
        }

        /** @brief Returns the cross product (this x other). */
        [[nodiscard]] Vector3 cross(const Vector3 &other) const noexcept
        {
            return Vector3(y * other.z - z * other.y, z * other.x - x * other.z, x * other.y - y * other.x);
        }

        /// Loads x, y, z into an XMVECTOR (w = 0).
        [[nodiscard]] DirectX::XMVECTOR to_xm_vector() const noexcept { return DirectX::XMVectorSet(x, y, z, 0.0f); }
        /// Extracts x, y, z from an XMVECTOR (w discarded).
        [[nodiscard]] static Vector3 from_xm_vector(DirectX::FXMVECTOR v) noexcept
        {
            return Vector3(DirectX::XMVectorGetX(v), DirectX::XMVectorGetY(v), DirectX::XMVectorGetZ(v));
        }
    };

    /**
     * @brief Minimal quaternion (x, y, z, w) used to read the engine's eye orientation.
     * @details Mirrors the engine's contiguous float layout so a rotation can be reinterpreted in place from
     *          the view params, then applied through DirectXMath.
     */
    struct Quaternion
    {
        float x, y, z, w;

        explicit Quaternion(float _x = 0.0f, float _y = 0.0f, float _z = 0.0f, float _w = 1.0f) noexcept
            : x(_x), y(_y), z(_z), w(_w)
        {
        }

        /// Loads x, y, z, w into an XMVECTOR.
        [[nodiscard]] DirectX::XMVECTOR to_xm_vector() const noexcept { return DirectX::XMVectorSet(x, y, z, w); }

        /** @brief Rotates a vector by this quaternion. */
        [[nodiscard]] Vector3 rotate(const Vector3 &v) const noexcept
        {
            DirectX::XMVECTOR q = to_xm_vector();
            DirectX::XMVECTOR vec = v.to_xm_vector();
            DirectX::XMVECTOR rotated_vec = DirectX::XMVector3Rotate(vec, q);
            return Vector3::from_xm_vector(rotated_vec);
        }
    };

} // namespace TPVCamera

#endif // TPVCAMERA_MATH_UTILS_HPP
