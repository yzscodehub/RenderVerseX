#include "Render/Material/MaterialClassification.h"
#include "Scene/Material.h"

namespace RVX
{
    MaterialRenderMode ClassifyMaterialRenderMode(const Material* material)
    {
        if (!material)
            return MaterialRenderMode::Opaque;

        switch (material->GetAlphaMode())
        {
            case Material::AlphaMode::Mask:
                return MaterialRenderMode::Masked;
            case Material::AlphaMode::Blend:
                return MaterialRenderMode::Transparent;
            case Material::AlphaMode::Opaque:
            default:
                return MaterialRenderMode::Opaque;
        }
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
