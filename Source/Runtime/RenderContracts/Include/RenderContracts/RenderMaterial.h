#pragma once

/**
 * @file RenderMaterial.h
 * @brief Render-facing material routing data shared by extraction and render.
 */

#include "Core/Types.h"
#include "Core/MathTypes.h"

#include <string>
#include <string_view>
#include <vector>

namespace RVX
{
    /** @brief Pipeline domain represented by a cooked material. */
    enum class MaterialDomain : uint8
    {
        Surface = 0
    };

    /** @brief Stable shading models supported by the material contract. */
    enum class MaterialShadingModel : uint8
    {
        StandardLit = 0,
        ToonLit,
        Unlit,
        ThinTransmission
    };

    /** @brief Backend-neutral blend policy selected during pipeline resolution. */
    enum class MaterialBlendMode : uint8
    {
        Opaque = 0,
        Masked,
        AlphaBlend,
        Premultiplied,
        Additive
    };

    /** @brief Backend-neutral raster culling policy. */
    enum class MaterialCullMode : uint8
    {
        Back = 0,
        NoCull
    };

    /** @brief Shadow participation policy carried by immutable material state. */
    enum class MaterialShadowPolicy : uint8
    {
        None = 0,
        Cast,
        Receive,
        CastAndReceive
    };

    /** @brief Whether a material policy permits the primitive to cast shadows. */
    [[nodiscard]] constexpr bool MaterialShadowPolicyCasts(
        MaterialShadowPolicy policy) noexcept
    {
        return policy == MaterialShadowPolicy::Cast ||
               policy == MaterialShadowPolicy::CastAndReceive;
    }

    /** @brief Whether a material policy permits the primitive to receive shadows. */
    [[nodiscard]] constexpr bool MaterialShadowPolicyReceives(
        MaterialShadowPolicy policy) noexcept
    {
        return policy == MaterialShadowPolicy::Receive ||
               policy == MaterialShadowPolicy::CastAndReceive;
    }

    /** @brief Legal renderer phase for a surface material. */
    enum class MaterialTranslucencyPhase : uint8
    {
        Opaque = 0,
        Standard,
        AfterToneMap
    };

    /** @brief Stable texture slot names understood by material upload and shaders. */
    enum class MaterialTextureSlot : uint8
    {
        BaseColor = 0,
        Normal,
        MetallicRoughness,
        Occlusion,
        Emissive,
        ToonRamp,
        Transmission,
        Thickness
    };

    enum class MaterialSamplerWrap : uint8
    {
        Repeat = 0,
        MirrorRepeat,
        ClampToEdge,
        ClampToBorder
    };

    enum class MaterialSamplerFilter : uint8
    {
        Nearest = 0,
        Linear,
        NearestMipmapNearest,
        LinearMipmapNearest,
        NearestMipmapLinear,
        LinearMipmapLinear
    };

    /** @brief Stable schema hash; deliberately independent of C++ sizeof/padding. */
    [[nodiscard]] uint64 GetMaterialParameterLayoutHashV1() noexcept;

    /** @brief Immutable pipeline-affecting portion of a material definition. */
    struct MaterialStaticDescriptor
    {
        MaterialDomain domain = MaterialDomain::Surface;
        MaterialShadingModel shadingModel = MaterialShadingModel::StandardLit;
        MaterialBlendMode blendMode = MaterialBlendMode::Opaque;
        MaterialCullMode cullMode = MaterialCullMode::Back;
        MaterialShadowPolicy shadowPolicy = MaterialShadowPolicy::CastAndReceive;
        MaterialTranslucencyPhase translucencyPhase =
            MaterialTranslucencyPhase::Opaque;
        int32 sortPriority = 0;
        uint64 featureMask = 0;
        std::string programFamily = "standard-lit-v1";
        uint64 parameterLayoutHash = GetMaterialParameterLayoutHashV1();
    };

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

        // Reserved fixed-layout fields used by the declared shading models.
        float toonBands = 4.0f;
        float toonSpecularThreshold = 0.5f;
        float toonRimStrength = 0.0f;
        float toonOutlineWidth = 0.0f;
        Vec3 toonShadowTint{0.5f, 0.5f, 0.5f};
        float transmission = 0.0f;
        float indexOfRefraction = 1.5f;
        float thickness = 0.0f;
        Vec3 attenuationColor{1.0f, 1.0f, 1.0f};
        float attenuationDistance = 1.0f;
    };

    /** @brief UV transform and sampler state for one typed material slot. */
    struct MaterialTextureSlotDefinition
    {
        MaterialTextureSlot slot = MaterialTextureSlot::BaseColor;
        uint8 uvSet = 0;
        Vec2 offset{0.0f, 0.0f};
        Vec2 scale{1.0f, 1.0f};
        float rotation = 0.0f;
        MaterialSamplerWrap wrapS = MaterialSamplerWrap::Repeat;
        MaterialSamplerWrap wrapT = MaterialSamplerWrap::Repeat;
        MaterialSamplerFilter minFilter =
            MaterialSamplerFilter::LinearMipmapLinear;
        MaterialSamplerFilter magFilter = MaterialSamplerFilter::Linear;
    };

    /**
     * @brief Canonical runtime material value shared by source and cooked paths.
     *
     * Resource bindings are stored by ResourceRuntime; this value carries only
     * immutable pipeline policy, typed parameters, and binding metadata.
     */
    struct MaterialDefinition
    {
        MaterialStaticDescriptor descriptor;
        MaterialSourceData parameters;
        std::vector<MaterialTextureSlotDefinition> textureSlots;
    };

    inline constexpr std::string_view
        RVX_MATERIAL_PARAMETER_LAYOUT_SCHEMA_V1 =
            "RVX.MaterialParameters.V1:baseColorFactor=float4;metallicFactor=float;"
            "roughnessFactor=float;normalScale=float;occlusionStrength=float;"
            "emissiveColor=float3;emissiveStrength=float;textureFlags=uint;"
            "alphaMode=uint;alphaCutoff=float;workflow=uint;doubleSided=bool;"
            "toonBands=float;toonSpecularThreshold=float;toonRimStrength=float;"
            "toonOutlineWidth=float;toonShadowTint=float3;transmission=float;"
            "indexOfRefraction=float;thickness=float;attenuationColor=float3;"
            "attenuationDistance=float";

    /** @brief Required program family for one fixed shading model. */
    [[nodiscard]] std::string_view GetMaterialProgramFamily(
        MaterialShadingModel shadingModel) noexcept;

    /** @brief Validate immutable pipeline-affecting material metadata. */
    [[nodiscard]] bool ValidateMaterialStaticDescriptor(
        const MaterialStaticDescriptor& descriptor,
        std::string& outError);

    /**
     * @brief Validate the complete fixed-layout material parameter block.
     *
     * Texture feature masks are validated by the owning definition or upload
     * payload because those checks require the corresponding binding metadata.
     */
    [[nodiscard]] bool ValidateMaterialSourceData(
        const MaterialSourceData& sourceData,
        std::string& outError);

    /**
     * @brief Validate the explicit compatibility mapping to legacy source data.
     *
     * The descriptor remains the pipeline authority. Legacy values only carry
     * parameters for the declared model and must never describe contradictory
     * blend, workflow, or culling behavior.
     */
    [[nodiscard]] bool ValidateMaterialDescriptorSourceCompatibility(
        const MaterialStaticDescriptor& descriptor,
        const MaterialSourceData& sourceData,
        std::string& outError);

    /** @brief Validate the complete backend-neutral material contract. */
    [[nodiscard]] bool ValidateMaterialDefinition(
        const MaterialDefinition& definition,
        std::string& outError);

} // namespace RVX
