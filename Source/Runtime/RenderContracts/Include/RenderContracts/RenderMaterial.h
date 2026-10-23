#pragma once

/**
 * @file RenderMaterial.h
 * @brief Render-facing material routing data shared by extraction and render.
 */

#include "Core/Types.h"
#include "Core/MathTypes.h"

#include <string>
#include <string_view>
#include <span>
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
        Thickness,
        Specular,
        SpecularColor,
        Clearcoat,
        ClearcoatRoughness,
        ClearcoatNormal
    };

    /**
     * @brief V2 feature bits owned by immutable material routing identity.
     *
     * Texture-slot bits are derived from declared bindings. Extension bits are
     * derived from the immutable parameter block and must never be inferred by
     * a render backend from arbitrary numeric values at draw time.
     */
    inline constexpr uint64 RVX_MATERIAL_TEXTURE_SLOT_FEATURE_MASK_V2 =
        (1ull << (static_cast<uint8>(
                      MaterialTextureSlot::ClearcoatNormal) + 1u)) -
        1ull;
    inline constexpr uint64 RVX_MATERIAL_FEATURE_SPECULAR = 1ull << 32u;
    inline constexpr uint64 RVX_MATERIAL_FEATURE_CLEARCOAT = 1ull << 33u;
    inline constexpr uint64 RVX_MATERIAL_FEATURE_TRANSMISSION = 1ull << 34u;
    inline constexpr uint64 RVX_MATERIAL_FEATURE_VOLUME = 1ull << 35u;
    inline constexpr uint64 RVX_MATERIAL_FEATURE_IOR = 1ull << 36u;
    inline constexpr uint64 RVX_MATERIAL_EXTENSION_FEATURE_MASK_V2 =
        RVX_MATERIAL_FEATURE_SPECULAR |
        RVX_MATERIAL_FEATURE_CLEARCOAT |
        RVX_MATERIAL_FEATURE_TRANSMISSION |
        RVX_MATERIAL_FEATURE_VOLUME |
        RVX_MATERIAL_FEATURE_IOR;
    inline constexpr uint64 RVX_MATERIAL_FEATURE_MASK_V2 =
        RVX_MATERIAL_TEXTURE_SLOT_FEATURE_MASK_V2 |
        RVX_MATERIAL_EXTENSION_FEATURE_MASK_V2;

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

    /** @brief Stable V2 schema hash; deliberately independent of C++ padding. */
    [[nodiscard]] uint64 GetMaterialParameterLayoutHashV2() noexcept;

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
        std::string programFamily = "standard-lit-v2";
        uint64 parameterLayoutHash = GetMaterialParameterLayoutHashV2();
    };

    /**
     * @brief The only legal raster route for a validated surface descriptor.
     *
     * This is intentionally a material-contract decision rather than a
     * renderer-local heuristic. A descriptor that is malformed, contradictory,
     * or not implemented by the current graphics foundation is never routed to
     * a fallback path implicitly.
     */
    enum class SurfacePathEligibility : uint8
    {
        DeferredOpaque = 0,
        ForwardSpecialOpaque,
        ForwardTranslucency,
        Unsupported
    };

    /**
     * @brief Classify one immutable material descriptor into exactly one path.
     *
     * StandardLit and ToonLit opaque/masked surfaces are deferred eligible.
     * Unlit opaque and masked surfaces retain the forward-special path. All
     * supported translucent descriptors use the forward translucency path.
     * Toon translucency is explicitly unsupported by the current material
     * contract.
     */
    [[nodiscard]] SurfacePathEligibility ClassifySurfacePathEligibility(
        const MaterialStaticDescriptor& descriptor);

    /** @brief Stable diagnostic name for a surface-path classification. */
    [[nodiscard]] std::string_view GetSurfacePathEligibilityName(
        SurfacePathEligibility eligibility) noexcept;

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
        float specularFactor = 1.0f;
        Vec3 specularColor{1.0f, 1.0f, 1.0f};
        float clearcoatFactor = 0.0f;
        float clearcoatRoughness = 0.0f;
        float clearcoatNormalScale = 1.0f;
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
        /**
         * @brief Beer-Lambert attenuation distance in engine metres.
         *
         * Zero is the canonical finite representation of the glTF default
         * infinite attenuation distance: it disables absorption. Positive
         * values are consumed by ThinTransmission shading.
         */
        float attenuationDistance = 0.0f;
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
        RVX_MATERIAL_PARAMETER_LAYOUT_SCHEMA_V2 =
            "RVX.MaterialParameters.V2:baseColorFactor=float4;metallicFactor=float;"
            "roughnessFactor=float;normalScale=float;occlusionStrength=float;"
            "emissiveColor=float3;emissiveStrength=float;specularFactor=float;"
            "specularColor=float3;clearcoatFactor=float;clearcoatRoughness=float;"
            "clearcoatNormalScale=float;textureFlags=uint;alphaMode=uint;"
            "alphaCutoff=float;workflow=uint;doubleSided=bool;toonBands=float;"
            "toonSpecularThreshold=float;toonRimStrength=float;"
            "toonOutlineWidth=float;toonShadowTint=float3;transmission=float;"
            "indexOfRefraction=float;thickness=float;attenuationColor=float3;"
            "attenuationDistance=float";

    /**
     * @brief Return the exact immutable V2 feature identity for parameters and
     *        declared slots. The result is used to reject partial artifacts.
     */
    [[nodiscard]] uint64 GetMaterialFeatureMaskV2(
        const MaterialSourceData& sourceData,
        std::span<const MaterialTextureSlotDefinition> textureSlots) noexcept;

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
