#pragma once

/**
 * @file RenderMaterial.h
 * @brief Render-facing material routing data shared by extraction and render.
 */

#include "Core/Types.h"
#include "Core/MathTypes.h"

namespace RVX
{
    enum class RenderMaterialMode : uint8
    {
        Opaque = 0,
        Masked,
        Transparent
    };

    enum class MaterialSourceAlphaMode : uint8
    {
        Opaque = 0,
        Mask,
        Blend
    };

    enum class MaterialSourceWorkflow : uint8
    {
        MetallicRoughness = 0,
        SpecularGlossiness,
        Unlit
    };

    /**
     * @brief Render-facing material properties after asset/runtime adaptation.
     */
    struct MaterialSourceData
    {
        Vec4 baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
        float metallicFactor = 1.0f;
        float roughnessFactor = 1.0f;
        float normalScale = 1.0f;
        float occlusionStrength = 1.0f;
        Vec3 emissiveColor{0.0f, 0.0f, 0.0f};
        float emissiveStrength = 1.0f;
        uint32 textureFlags = 0;
        MaterialSourceAlphaMode alphaMode = MaterialSourceAlphaMode::Opaque;
        float alphaCutoff = 0.5f;
        MaterialSourceWorkflow workflow = MaterialSourceWorkflow::MetallicRoughness;
        bool doubleSided = false;
    };

} // namespace RVX
