#pragma once

/**
 * @file MaterialClassification.h
 * @brief Helpers for routing materials to render passes and pipeline variants
 */

#include "Core/Types.h"

namespace RVX
{
    class Material;

    enum class MaterialRenderMode : uint8
    {
        Opaque = 0,
        Masked,
        Transparent
    };

    enum class MaterialPipelineVariant : uint8
    {
        Opaque = 0,
        Masked,
        Transparent
    };

    MaterialRenderMode ClassifyMaterialRenderMode(const Material* material);
    MaterialPipelineVariant GetPipelineVariantForRenderMode(MaterialRenderMode mode);

} // namespace RVX
