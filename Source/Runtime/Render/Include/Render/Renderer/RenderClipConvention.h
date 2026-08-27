#pragma once

/**
 * @file RenderClipConvention.h
 * @brief The single raster clip-space convention for Render feature uploads.
 */

#include "Core/MathTypes.h"
#include "RHI/RHIDefinitions.h"

namespace RVX
{
    /**
     * Apply the backend raster convention to an engine-semantic clip matrix.
     *
     * Engine view matrices produce semantic NDC where positive Y is upward.
     * Vulkan's positive-height viewport maps positive clip Y downward, so all
     * raster uploads must negate the *output* Y row exactly once. This helper
     * is deliberately shared by the standard pipeline and feature-owned
     * pipelines so custom features cannot silently diverge from scene draws.
     *
     * The input must be the canonical ViewData matrix. Callers must never pass
     * a matrix returned by this function back into it.
     */
    [[nodiscard]] inline Mat4 ApplyBackendClipConvention(
        const Mat4& matrix,
        RHIBackendType backend) noexcept
    {
        Mat4 result = matrix;
        if (backend == RHIBackendType::Vulkan)
        {
            // GLM indexes matrices as [column][row]. Negating result[1]
            // would mirror the input/world Y column instead of output clip Y.
            for (uint32 column = 0; column < 4u; ++column)
            {
                result[column][1] = -result[column][1];
            }
        }
        return result;
    }
} // namespace RVX
