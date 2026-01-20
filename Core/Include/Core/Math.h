#pragma once

/**
 * @file Math.h
 * @brief DEPRECATED - Use Core/MathTypes.h instead
 * 
 * This file contains legacy math types and is kept for backward compatibility.
 * All new code should use Core/MathTypes.h which provides GLM-based types
 * with RVX namespace aliases for easy future replacement.
 * 
 * @deprecated Use Core/MathTypes.h for all new code
 */

#include <cmath>

// Emit deprecation warning when included
#ifdef _MSC_VER
#pragma message("Warning: Core/Math.h is deprecated. Use Core/MathTypes.h instead.")
#endif

namespace RVX
{
    struct Mat4;
    inline Mat4 Multiply(const Mat4& a, const Mat4& b);

    struct Vec2
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;

        Vec3() = default;
        Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    };

    inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    inline Vec3 operator*(const Vec3& v, float s) { return {v.x * s, v.y * s, v.z * s}; }

    inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline Vec3 Cross(const Vec3& a, const Vec3& b)
    {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }
    inline float Length(const Vec3& v) { return std::sqrt(Dot(v, v)); }
    inline Vec3 Normalize(const Vec3& v)
    {
        float len = Length(v);
        return len > 0.0f ? v * (1.0f / len) : Vec3{};
    }

    struct Mat4
    {
        float m[16]{};

        static Mat4 Identity()
        {
            Mat4 result;
            result.m[0] = result.m[5] = result.m[10] = result.m[15] = 1.0f;
            return result;
        }

        static Mat4 Translation(const Vec3& t)
        {
            Mat4 result = Identity();
            result.m[12] = t.x;
            result.m[13] = t.y;
            result.m[14] = t.z;
            return result;
        }

        static Mat4 RotationXYZ(const Vec3& radians)
        {
            float cx = std::cos(radians.x);
            float sx = std::sin(radians.x);
            float cy = std::cos(radians.y);
            float sy = std::sin(radians.y);
            float cz = std::cos(radians.z);
            float sz = std::sin(radians.z);

            Mat4 rx = Identity();
            rx.m[5] = cx;
            rx.m[6] = sx;
            rx.m[9] = -sx;
            rx.m[10] = cx;

            Mat4 ry = Identity();
            ry.m[0] = cy;
            ry.m[2] = -sy;
            ry.m[8] = sy;
            ry.m[10] = cy;

            Mat4 rz = Identity();
            rz.m[0] = cz;
            rz.m[1] = sz;
            rz.m[4] = -sz;
            rz.m[5] = cz;

            return Multiply(Multiply(rz, ry), rx);
        }

        static Mat4 Perspective(float fovYRadians, float aspect, float nearZ, float farZ)
        {
            Mat4 result{};
            float f = 1.0f / std::tan(fovYRadians * 0.5f);
            result.m[0] = f / aspect;
            result.m[5] = f;
            result.m[10] = farZ / (farZ - nearZ);
            result.m[11] = 1.0f;
            result.m[14] = (-nearZ * farZ) / (farZ - nearZ);
            return result;
        }

        static Mat4 Orthographic(float width, float height, float nearZ, float farZ)
        {
            Mat4 result = Identity();
            result.m[0] = 2.0f / width;
            result.m[5] = 2.0f / height;
            result.m[10] = 1.0f / (farZ - nearZ);
            result.m[14] = -nearZ / (farZ - nearZ);
            return result;
        }

        static Mat4 Transpose(const Mat4& m)
        {
            Mat4 result;
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    result.m[i * 4 + j] = m.m[j * 4 + i];
            return result;
        }
    };

    inline Mat4 Multiply(const Mat4& a, const Mat4& b)
    {
        Mat4 result{};
        for (int i = 0; i < 4; ++i)
        {
            for (int j = 0; j < 4; ++j)
            {
                result.m[i * 4 + j] =
                    a.m[i * 4 + 0] * b.m[0 * 4 + j] +
                    a.m[i * 4 + 1] * b.m[1 * 4 + j] +
                    a.m[i * 4 + 2] * b.m[2 * 4 + j] +
                    a.m[i * 4 + 3] * b.m[3 * 4 + j];
            }
        }
        return result;
    }

    inline Mat4 operator*(const Mat4& a, const Mat4& b)
    {
        return Multiply(a, b);
    }
} // namespace RVX
