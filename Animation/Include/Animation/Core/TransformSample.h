/**
 * @file TransformSample.h
 * @brief Transform sample structure for animation poses
 * 
 * Migrated from found::animation::TransformSample
 */

#pragma once

#include "Core/MathTypes.h"
#include <cmath>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

namespace RVX::Animation
{

/**
 * @brief Decomposed transform (Translation, Rotation, Scale)
 * 
 * Used for animation sampling and blending. Storing transforms as TRS
 * allows for proper interpolation (especially rotation via Slerp).
 */
struct TransformSample
{
    Vec3 translation{0.0f, 0.0f, 0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};  // Identity quaternion (w, x, y, z)
    Vec3 scale{1.0f, 1.0f, 1.0f};
    
    TransformSample() = default;
    
    TransformSample(const Vec3& t, const Quat& r, const Vec3& s)
        : translation(t), rotation(r), scale(s) {}
    
    explicit TransformSample(const Vec3& t)
        : translation(t) {}
    
    explicit TransformSample(const Quat& r)
        : rotation(r) {}
    
    // ========================================================================
    // Matrix Conversion
    // ========================================================================
    
    /**
     * @brief Compose TRS into a 4x4 transformation matrix
     */
    Mat4 ToMatrix() const
    {
        Mat4 result = translate(Mat4(1.0f), translation);
        result *= glm::mat4_cast(rotation);
        result = glm::scale(result, scale);
        return result;
    }
    
    /**
     * @brief Decompose a matrix into TRS components
     */
    static TransformSample FromMatrix(const Mat4& matrix)
    {
        TransformSample result;
        Vec3 skew;
        Vec4 perspective;
        glm::decompose(matrix, result.scale, result.rotation,
                       result.translation, skew, perspective);
        result.rotation = normalize(result.rotation);
        return result;
    }
    
    // ========================================================================
    // Interpolation
    // ========================================================================
    
    /**
     * @brief Linear interpolation between two transforms
     */
    static TransformSample Lerp(const TransformSample& a, const TransformSample& b, float t)
    {
        TransformSample result;
        result.translation = mix(a.translation, b.translation, t);
        result.scale = mix(a.scale, b.scale, t);
        result.rotation = slerp(a.rotation, b.rotation, t);
        return result;
    }
    
    /**
     * @brief Additive blend: base + (additive * weight)
     */
    static TransformSample Additive(const TransformSample& base,
                                    const TransformSample& additive,
                                    float weight)
    {
        TransformSample result;
        result.translation = base.translation + additive.translation * weight;
        
        Quat identity{1.0f, 0.0f, 0.0f, 0.0f};
        Quat weightedRotation = slerp(identity, additive.rotation, weight);
        result.rotation = normalize(weightedRotation * base.rotation);
        
        Vec3 scaleDelta = additive.scale - Vec3(1.0f);
        result.scale = base.scale + base.scale * scaleDelta * weight;
        
        return result;
    }
    
    // ========================================================================
    // Identity and Comparison
    // ========================================================================
    
    static TransformSample Identity()
    {
        return TransformSample{Vec3(0.0f), Quat(1.0f, 0.0f, 0.0f, 0.0f), Vec3(1.0f)};
    }
    
    bool IsIdentity(float epsilon = 1e-5f) const
    {
        bool tZero = length(translation) < epsilon;
        bool rIdentity = std::abs(rotation.w - 1.0f) < epsilon &&
                         length(Vec3(rotation.x, rotation.y, rotation.z)) < epsilon;
        bool sOne = length(scale - Vec3(1.0f)) < epsilon;
        return tZero && rIdentity && sOne;
    }
    
    // ========================================================================
    // Utility Methods
    // ========================================================================
    
    Vec3 GetForward() const { return rotation * Vec3(0.0f, 0.0f, -1.0f); }
    Vec3 GetRight() const { return rotation * Vec3(1.0f, 0.0f, 0.0f); }
    Vec3 GetUp() const { return rotation * Vec3(0.0f, 1.0f, 0.0f); }
    
    TransformSample Inverse() const
    {
        TransformSample result;
        result.scale = Vec3(1.0f) / scale;
        result.rotation = glm::conjugate(rotation);
        result.translation = result.rotation * (-translation * result.scale);
        return result;
    }
    
    TransformSample operator*(const TransformSample& other) const
    {
        TransformSample result;
        result.scale = scale * other.scale;
        result.rotation = normalize(rotation * other.rotation);
        result.translation = translation + rotation * (scale * other.translation);
        return result;
    }
    
    Vec3 GetEulerAngles() const { return glm::eulerAngles(rotation); }
    void SetEulerAngles(const Vec3& euler) { rotation = Quat(euler); }
};

} // namespace RVX::Animation
