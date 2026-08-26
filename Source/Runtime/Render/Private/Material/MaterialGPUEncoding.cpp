/**
 * @file MaterialGPUEncoding.cpp
 * @brief Shared CPU-to-GPU encoding for the fixed material constants ABI.
 */

#include "Render/Material/MaterialGPUEncoding.h"

#include <cmath>

namespace RVX
{
    namespace
    {
        MaterialGPUShadingModel ToGPUShadingModel(
            MaterialShadingModel shadingModel) noexcept
        {
            switch (shadingModel)
            {
                case MaterialShadingModel::ToonLit:
                    return MaterialGPUShadingModel::ToonLit;
                case MaterialShadingModel::Unlit:
                    return MaterialGPUShadingModel::Unlit;
                case MaterialShadingModel::ThinTransmission:
                    return MaterialGPUShadingModel::ThinTransmission;
                case MaterialShadingModel::StandardLit:
                default:
                    return MaterialGPUShadingModel::StandardLit;
            }
        }

        MaterialGPUBlendMode ToGPUBlendMode(
            MaterialBlendMode blendMode) noexcept
        {
            switch (blendMode)
            {
                case MaterialBlendMode::Masked:
                    return MaterialGPUBlendMode::Masked;
                case MaterialBlendMode::AlphaBlend:
                    return MaterialGPUBlendMode::AlphaBlend;
                case MaterialBlendMode::Premultiplied:
                    return MaterialGPUBlendMode::Premultiplied;
                case MaterialBlendMode::Additive:
                    return MaterialGPUBlendMode::Additive;
                case MaterialBlendMode::Opaque:
                default:
                    return MaterialGPUBlendMode::Opaque;
            }
        }
    } // namespace

    MaterialGPUConstants EncodeMaterialGPUConstants(
        const MaterialSourceData& sourceData,
        std::span<const MaterialUploadTextureBinding> textureBindings) noexcept
    {
        return EncodeMaterialGPUConstants(
            MaterialStaticDescriptor{}, sourceData, textureBindings, 0);
    }

    MaterialGPUConstants EncodeMaterialGPUConstants(
        const MaterialStaticDescriptor& staticDescriptor,
        const MaterialSourceData& sourceData,
        std::span<const MaterialUploadTextureBinding> textureBindings,
        uint32 materialTableIndex) noexcept
    {
        MaterialGPUConstants constants;
        constants.baseColorFactor = sourceData.baseColorFactor;
        constants.metallicFactor = sourceData.metallicFactor;
        constants.roughnessFactor = sourceData.roughnessFactor;
        constants.normalScale = sourceData.normalScale;
        constants.occlusionStrength = sourceData.occlusionStrength;
        constants.emissiveColor = sourceData.emissiveColor;
        constants.emissiveStrength = sourceData.emissiveStrength;
        constants.textureFlags = sourceData.textureFlags;
        constants.alphaCutoff = sourceData.alphaCutoff;
        constants.doubleSided = sourceData.doubleSided ? 1U : 0U;
        constants.shadingModel = static_cast<uint32>(
            ToGPUShadingModel(staticDescriptor.shadingModel));
        constants.materialTableIndex = materialTableIndex;
        constants.blendMode = static_cast<uint32>(
            ToGPUBlendMode(staticDescriptor.blendMode));
        constants.toonBands = sourceData.toonBands;
        constants.toonSpecularThreshold = sourceData.toonSpecularThreshold;
        constants.toonRimStrength = sourceData.toonRimStrength;
        constants.toonOutlineWidth = sourceData.toonOutlineWidth;
        constants.toonShadowTint = {
            sourceData.toonShadowTint.x,
            sourceData.toonShadowTint.y,
            sourceData.toonShadowTint.z,
            0.0f};
        constants.thinTransmissionParams = {
            sourceData.transmission,
            sourceData.indexOfRefraction,
            sourceData.thickness,
            sourceData.attenuationDistance};
        constants.attenuationColor = {
            sourceData.attenuationColor.x,
            sourceData.attenuationColor.y,
            sourceData.attenuationColor.z,
            0.0f};

        switch (sourceData.alphaMode)
        {
            case MaterialSourceAlphaMode::Mask:
                constants.alphaMode = static_cast<uint32>(MaterialGPUAlphaMode::Mask);
                break;
            case MaterialSourceAlphaMode::Blend:
                constants.alphaMode = static_cast<uint32>(MaterialGPUAlphaMode::Blend);
                break;
            case MaterialSourceAlphaMode::Opaque:
            default:
                constants.alphaMode = static_cast<uint32>(MaterialGPUAlphaMode::Opaque);
                break;
        }

        switch (sourceData.workflow)
        {
            case MaterialSourceWorkflow::SpecularGlossiness:
                constants.workflow =
                    static_cast<uint32>(MaterialGPUWorkflow::SpecularGlossiness);
                break;
            case MaterialSourceWorkflow::Unlit:
                constants.workflow = static_cast<uint32>(MaterialGPUWorkflow::Unlit);
                break;
            case MaterialSourceWorkflow::MetallicRoughness:
            default:
                constants.workflow =
                    static_cast<uint32>(MaterialGPUWorkflow::MetallicRoughness);
                break;
        }

        for (const MaterialUploadTextureBinding& binding : textureBindings)
        {
            const uint32 slot = static_cast<uint32>(binding.slot);
            if (slot >= constants.uvTransforms.size() || binding.uvSet < 0 ||
                binding.uvSet > 1)
            {
                continue;
            }

            MaterialGPUUVTransform& transform = constants.uvTransforms[slot];
            transform.scaleOffset = {
                binding.scale.x,
                binding.scale.y,
                binding.offset.x,
                binding.offset.y};
            transform.rotationUVSet = {
                std::cos(binding.rotation),
                std::sin(binding.rotation),
                static_cast<float32>(binding.uvSet),
                0.0f};
        }

        return constants;
    }

} // namespace RVX
