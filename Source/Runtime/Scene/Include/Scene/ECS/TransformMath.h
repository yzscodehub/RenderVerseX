#pragma once

/**
 * @file TransformMath.h
 * @brief Pure transform conversion helpers shared by Scene ECS processors.
 */

#include "Scene/ECS/Fragments.h"

#include <cmath>

namespace RVX::SceneECS
{
    /** @brief Construct a local affine matrix using the engine's T * R * S convention. */
    [[nodiscard]] inline Mat4 MakeLocalTransformMatrix(const LocalTransform& transform)
    {
        Mat4 matrix(1.0f);
        matrix = glm::translate(matrix, transform.translation);
        matrix *= glm::mat4_cast(transform.rotation);
        return glm::scale(matrix, transform.scale);
    }

    /** @brief Decompose an affine matrix using the same signed-scale convention as Scene. */
    inline void DecomposeLocalTransformMatrix(const Mat4& matrix, LocalTransform& transform)
    {
        transform.translation = Vec3(matrix[3]);
        Vec3 xAxis(matrix[0]);
        Vec3 yAxis(matrix[1]);
        Vec3 zAxis(matrix[2]);
        transform.scale = Vec3(glm::length(xAxis), glm::length(yAxis), glm::length(zAxis));

        if (transform.scale.x > 0.0f)
        {
            xAxis /= transform.scale.x;
        }
        if (transform.scale.y > 0.0f)
        {
            yAxis /= transform.scale.y;
        }
        if (transform.scale.z > 0.0f)
        {
            zAxis /= transform.scale.z;
        }

        Mat3 rotationMatrix(xAxis, yAxis, zAxis);
        if (glm::determinant(rotationMatrix) < 0.0f)
        {
            transform.scale.x = -transform.scale.x;
            rotationMatrix[0] = -rotationMatrix[0];
        }
        transform.rotation = glm::normalize(glm::quat_cast(rotationMatrix));
    }

    /**
     * @brief Derive a child-local transform that preserves childWorld under parentWorld.
     * @return False when parentWorld cannot be inverted.
     */
    [[nodiscard]] inline bool TryMakeKeepWorldLocalTransform(
        const Mat4& parentWorld,
        const Mat4& childWorld,
        LocalTransform& outLocal)
    {
        const float determinant = glm::determinant(parentWorld);
        if (!std::isfinite(determinant) || std::abs(determinant) <= 0.000001f)
        {
            return false;
        }

        DecomposeLocalTransformMatrix(glm::inverse(parentWorld) * childWorld, outLocal);
        return true;
    }
} // namespace RVX::SceneECS
