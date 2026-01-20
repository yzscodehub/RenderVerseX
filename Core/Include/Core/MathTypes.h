#pragma once

/**
 * @file MathTypes.h
 * @brief GLM-based math type aliases for RenderVerseX
 * 
 * This file provides type aliases that wrap GLM types in the RVX namespace,
 * allowing consistent usage throughout the codebase while leveraging GLM's
 * robust implementation for quaternions, matrix operations, and more.
 */

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace RVX
{
    // =========================================================================
    // Vector Types
    // =========================================================================
    
    /// 2D floating-point vector
    using Vec2 = glm::vec2;
    
    /// 3D floating-point vector
    using Vec3 = glm::vec3;
    
    /// 4D floating-point vector
    using Vec4 = glm::vec4;
    
    /// 2D integer vector
    using IVec2 = glm::ivec2;
    
    /// 3D integer vector
    using IVec3 = glm::ivec3;
    
    /// 4D integer vector
    using IVec4 = glm::ivec4;
    
    /// 2D unsigned integer vector
    using UVec2 = glm::uvec2;
    
    /// 3D unsigned integer vector
    using UVec3 = glm::uvec3;
    
    /// 4D unsigned integer vector
    using UVec4 = glm::uvec4;
    
    // =========================================================================
    // Matrix Types
    // =========================================================================
    
    /// 3x3 matrix (row-major in memory, column-major semantics)
    using Mat3 = glm::mat3;
    
    /// 4x4 matrix (row-major in memory, column-major semantics)
    using Mat4 = glm::mat4;
    
    // =========================================================================
    // Quaternion
    // =========================================================================
    
    /// Quaternion for rotations
    using Quat = glm::quat;
    
    // =========================================================================
    // Common Math Functions (forwarded from GLM)
    // =========================================================================
    
    using glm::dot;
    using glm::cross;
    using glm::normalize;
    using glm::length;
    using glm::distance;
    using glm::inverse;
    using glm::transpose;
    using glm::determinant;
    
    // Transformations
    using glm::perspective;
    using glm::ortho;
    using glm::lookAt;
    using glm::translate;
    using glm::rotate;
    using glm::scale;
    
    // Angle conversions
    using glm::radians;
    using glm::degrees;
    
    // Interpolation
    using glm::mix;
    using glm::lerp;
    using glm::slerp;
    
    // Component-wise operations
    using glm::min;
    using glm::max;
    using glm::clamp;
    using glm::abs;
    using glm::floor;
    using glm::ceil;
    using glm::fract;
    
    // Pointer access
    using glm::value_ptr;
    
    // =========================================================================
    // Quaternion Operations
    // =========================================================================
    
    /// Create quaternion from axis-angle
    inline Quat QuatFromAxisAngle(const Vec3& axis, float angleRadians)
    {
        return glm::angleAxis(angleRadians, axis);
    }
    
    /// Create quaternion from Euler angles (pitch, yaw, roll in radians)
    inline Quat QuatFromEuler(const Vec3& eulerRadians)
    {
        return Quat(eulerRadians);
    }
    
    /// Convert quaternion to Euler angles
    inline Vec3 QuatToEuler(const Quat& q)
    {
        return glm::eulerAngles(q);
    }
    
    /// Convert quaternion to rotation matrix
    inline Mat4 QuatToMat4(const Quat& q)
    {
        return glm::mat4_cast(q);
    }
    
    /// Convert rotation matrix to quaternion
    inline Quat Mat4ToQuat(const Mat4& m)
    {
        return glm::quat_cast(m);
    }
    
    // =========================================================================
    // Matrix Construction Helpers (for Camera module compatibility)
    // =========================================================================
    
    /// Create identity matrix
    inline Mat4 Mat4Identity()
    {
        return Mat4(1.0f);
    }
    
    /// Create perspective projection matrix
    /// @param fovRadians Field of view in radians
    /// @param aspect Aspect ratio (width/height)
    /// @param nearZ Near clipping plane
    /// @param farZ Far clipping plane
    inline Mat4 MakePerspective(float fovRadians, float aspect, float nearZ, float farZ)
    {
        return glm::perspective(fovRadians, aspect, nearZ, farZ);
    }
    
    /// Create orthographic projection matrix
    /// @param width Viewport width
    /// @param height Viewport height
    /// @param nearZ Near clipping plane
    /// @param farZ Far clipping plane
    inline Mat4 MakeOrthographic(float width, float height, float nearZ, float farZ)
    {
        return glm::ortho(-width * 0.5f, width * 0.5f, -height * 0.5f, height * 0.5f, nearZ, farZ);
    }
    
    /// Create rotation matrix from Euler angles (XYZ order)
    /// @param eulerRadians Euler angles in radians (pitch, yaw, roll)
    inline Mat4 MakeRotationXYZ(const Vec3& eulerRadians)
    {
        Mat4 result(1.0f);
        result = glm::rotate(result, eulerRadians.z, Vec3(0.0f, 0.0f, 1.0f));
        result = glm::rotate(result, eulerRadians.y, Vec3(0.0f, 1.0f, 0.0f));
        result = glm::rotate(result, eulerRadians.x, Vec3(1.0f, 0.0f, 0.0f));
        return result;
    }
    
    /// Create translation matrix
    inline Mat4 MakeTranslation(const Vec3& translation)
    {
        return glm::translate(Mat4(1.0f), translation);
    }
    
    /// Create scale matrix
    inline Mat4 MakeScale(const Vec3& scale)
    {
        return glm::scale(Mat4(1.0f), scale);
    }
    
    // =========================================================================
    // Matrix Access Helpers
    // =========================================================================
    
    /// Get column from matrix (GLM uses column-major storage)
    inline Vec4 GetMatrixColumn(const Mat4& m, int col)
    {
        return m[col];
    }
    
    /// Get row from matrix
    inline Vec4 GetMatrixRow(const Mat4& m, int row)
    {
        return Vec4(m[0][row], m[1][row], m[2][row], m[3][row]);
    }
    
    /// Get forward direction from rotation matrix (-Z axis)
    inline Vec3 GetForwardFromMatrix(const Mat4& m)
    {
        return -Vec3(m[2]);  // Negative Z column
    }
    
    /// Get right direction from rotation matrix (+X axis)
    inline Vec3 GetRightFromMatrix(const Mat4& m)
    {
        return Vec3(m[0]);  // X column
    }
    
    /// Get up direction from rotation matrix (+Y axis)
    inline Vec3 GetUpFromMatrix(const Mat4& m)
    {
        return Vec3(m[1]);  // Y column
    }

} // namespace RVX
