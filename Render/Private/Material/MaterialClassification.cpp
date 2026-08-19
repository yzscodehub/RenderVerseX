#include "Render/Material/MaterialClassification.h"

namespace RVX
{
    MaterialRenderMode ClassifyMaterialRenderMode(MaterialSourceAlphaMode alphaMode)
    {
        switch (alphaMode)
        {
            case MaterialSourceAlphaMode::Mask:
                return MaterialRenderMode::Masked;
            case MaterialSourceAlphaMode::Blend:
                return MaterialRenderMode::Transparent;
            case MaterialSourceAlphaMode::Opaque:
            default:
                return MaterialRenderMode::Opaque;
        }
    }

    MaterialRenderMode ClassifyMaterialRenderMode(const MaterialSourceData& materialSource)
    {
        return ClassifyMaterialRenderMode(materialSource.alphaMode);
    }

    MaterialPipelineVariant GetPipelineVariantForRenderMode(MaterialRenderMode mode)
    {
        switch (mode)
        {
            case MaterialRenderMode::Masked:
                return MaterialPipelineVariant::Masked;
            case MaterialRenderMode::Transparent:
                return MaterialPipelineVariant::Transparent;
            case MaterialRenderMode::Opaque:
            default:
                return MaterialPipelineVariant::Opaque;
        }
    }

} // namespace RVX
