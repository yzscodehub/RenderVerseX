/** @file RenderMaterial.cpp @brief Canonical material contract validation */

#include "RenderContracts/RenderMaterial.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace RVX
{
namespace
{
    constexpr uint64 Fnv1a64(std::string_view value) noexcept
    {
        uint64 hash = 14695981039346656037ull;
        for (const char character : value)
        {
            hash ^= static_cast<uint8>(character);
            hash *= 1099511628211ull;
        }
        return hash;
    }

    bool IsFinite(float32 value) noexcept
    {
        return std::isfinite(value);
    }

    bool IsFinite(const Vec2& value) noexcept
    {
        return IsFinite(value.x) && IsFinite(value.y);
    }

    bool IsFinite(const Vec3& value) noexcept
    {
        return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z);
    }

    bool IsFinite(const Vec4& value) noexcept
    {
        return IsFinite(value.x) && IsFinite(value.y) &&
               IsFinite(value.z) && IsFinite(value.w);
    }

    bool IsInRange(float32 value, float32 minimum, float32 maximum) noexcept
    {
        return IsFinite(value) && value >= minimum && value <= maximum;
    }

    template <typename Enum>
    bool IsEnumInRange(Enum value, Enum last) noexcept
    {
        return static_cast<uint32>(value) <= static_cast<uint32>(last);
    }
} // namespace

uint64 GetMaterialParameterLayoutHashV1() noexcept
{
    return Fnv1a64(RVX_MATERIAL_PARAMETER_LAYOUT_SCHEMA_V1);
}

std::string_view GetMaterialProgramFamily(
    MaterialShadingModel shadingModel) noexcept
{
    switch (shadingModel)
    {
        case MaterialShadingModel::StandardLit: return "standard-lit-v1";
        case MaterialShadingModel::ToonLit: return "toon-lit-v1";
        case MaterialShadingModel::Unlit: return "unlit-v1";
        case MaterialShadingModel::ThinTransmission:
            return "thin-transmission-v1";
    }
    return {};
}

bool ValidateMaterialDefinition(
    const MaterialDefinition& definition,
    std::string& outError)
{
    const MaterialStaticDescriptor& descriptor = definition.descriptor;
    const MaterialSourceData& parameters = definition.parameters;
    if (!IsEnumInRange(descriptor.domain, MaterialDomain::Surface) ||
        !IsEnumInRange(descriptor.shadingModel,
                       MaterialShadingModel::ThinTransmission) ||
        !IsEnumInRange(descriptor.blendMode, MaterialBlendMode::Additive) ||
        !IsEnumInRange(descriptor.cullMode, MaterialCullMode::NoCull) ||
        !IsEnumInRange(descriptor.shadowPolicy,
                       MaterialShadowPolicy::CastAndReceive) ||
        !IsEnumInRange(descriptor.translucencyPhase,
                       MaterialTranslucencyPhase::AfterToneMap))
    {
        outError = "Material definition contains an unknown static enum";
        return false;
    }
    if (descriptor.programFamily !=
            GetMaterialProgramFamily(descriptor.shadingModel) ||
        descriptor.parameterLayoutHash != GetMaterialParameterLayoutHashV1())
    {
        outError = "Material program family or parameter layout hash is invalid";
        return false;
    }
    const bool opaquePhase =
        descriptor.blendMode == MaterialBlendMode::Opaque ||
        descriptor.blendMode == MaterialBlendMode::Masked;
    if (opaquePhase !=
        (descriptor.translucencyPhase == MaterialTranslucencyPhase::Opaque))
    {
        outError = "Material blend mode and translucency phase disagree";
        return false;
    }
    if (descriptor.shadingModel == MaterialShadingModel::ThinTransmission &&
        opaquePhase)
    {
        outError = "Thin transmission requires a translucent blend mode";
        return false;
    }
    if (!IsFinite(parameters.baseColorFactor) ||
        !IsInRange(parameters.metallicFactor, 0.0f, 1.0f) ||
        !IsInRange(parameters.roughnessFactor, 0.0f, 1.0f) ||
        !IsInRange(parameters.normalScale, 0.0f, 8.0f) ||
        !IsInRange(parameters.occlusionStrength, 0.0f, 1.0f) ||
        !IsFinite(parameters.emissiveColor) ||
        !IsInRange(parameters.emissiveStrength, 0.0f, 65504.0f) ||
        !IsInRange(parameters.alphaCutoff, 0.0f, 1.0f) ||
        !IsInRange(parameters.toonBands, 1.0f, 64.0f) ||
        !IsInRange(parameters.toonSpecularThreshold, 0.0f, 1.0f) ||
        !IsInRange(parameters.toonRimStrength, 0.0f, 64.0f) ||
        !IsInRange(parameters.toonOutlineWidth, 0.0f, 64.0f) ||
        !IsFinite(parameters.toonShadowTint) ||
        !IsInRange(parameters.transmission, 0.0f, 1.0f) ||
        !IsInRange(parameters.indexOfRefraction, 1.0f, 3.0f) ||
        !IsInRange(parameters.thickness, 0.0f, 100000.0f) ||
        !IsFinite(parameters.attenuationColor) ||
        !IsInRange(parameters.attenuationDistance,
                   std::numeric_limits<float32>::min(), 1000000.0f))
    {
        outError = "Material parameter block contains an invalid value";
        return false;
    }
    if (parameters.alphaMode >
            MaterialSourceAlphaMode::Blend ||
        parameters.workflow > MaterialSourceWorkflow::Unlit)
    {
        outError = "Material parameter block contains an unknown legacy enum";
        return false;
    }

    std::vector<MaterialTextureSlot> observedSlots;
    uint64 observedTextureFeatures = 0;
    observedSlots.reserve(definition.textureSlots.size());
    for (const MaterialTextureSlotDefinition& slot : definition.textureSlots)
    {
        if (!IsEnumInRange(slot.slot, MaterialTextureSlot::Thickness) ||
            slot.uvSet > 1u || !IsFinite(slot.offset) || !IsFinite(slot.scale) ||
            slot.scale.x <= 0.0f || slot.scale.y <= 0.0f ||
            !IsFinite(slot.rotation) ||
            !IsEnumInRange(slot.wrapS, MaterialSamplerWrap::ClampToBorder) ||
            !IsEnumInRange(slot.wrapT, MaterialSamplerWrap::ClampToBorder) ||
            !IsEnumInRange(slot.minFilter,
                           MaterialSamplerFilter::LinearMipmapLinear) ||
            !IsEnumInRange(slot.magFilter,
                           MaterialSamplerFilter::LinearMipmapLinear))
        {
            outError = "Material texture slot metadata is invalid";
            return false;
        }
        if (std::find(observedSlots.begin(), observedSlots.end(), slot.slot) !=
            observedSlots.end())
        {
            outError = "Material definition contains a duplicate texture slot";
            return false;
        }
        observedSlots.push_back(slot.slot);
        observedTextureFeatures |= 1ull << static_cast<uint8>(slot.slot);
    }
    constexpr uint64 textureFeatureBits =
        (1ull << (static_cast<uint8>(MaterialTextureSlot::Thickness) + 1u)) - 1u;
    constexpr uint32 legacyTextureFeatureBits =
        (1u << (static_cast<uint8>(MaterialTextureSlot::Emissive) + 1u)) - 1u;
    if ((descriptor.featureMask & textureFeatureBits) !=
            observedTextureFeatures ||
        parameters.textureFlags !=
            (static_cast<uint32>(observedTextureFeatures) &
             legacyTextureFeatureBits))
    {
        outError =
            "Material feature masks do not match the declared texture slots";
        return false;
    }

    outError.clear();
    return true;
}
} // namespace RVX
