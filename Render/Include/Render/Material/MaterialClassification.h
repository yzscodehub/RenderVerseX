#pragma once

/**
 * @file MaterialClassification.h
 * @brief Helpers for routing materials to render passes and pipeline variants
 */

#include "Render/Material/MaterialSourceData.h"
#include "RenderContracts/RenderMaterial.h"

namespace RVX
{
    using MaterialRenderMode = RenderMaterialMode;

    enum class MaterialPipelineVariant : uint8
    {
        Opaque = 0,
        Masked,
        Transparent
    };

    MaterialRenderMode ClassifyMaterialRenderMode(MaterialSourceAlphaMode alphaMode);
    MaterialRenderMode ClassifyMaterialRenderMode(const MaterialSourceData& materialSource);
    MaterialPipelineVariant GetPipelineVariantForRenderMode(MaterialRenderMode mode);

} // namespace RVX
