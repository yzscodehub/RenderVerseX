#pragma once

/**
 * @file MaterialSystem.h
 * @brief GPU material descriptor and constant management
 */

#include "Core/Assert.h"
#include "Core/Types.h"
#include "Render/Material/MaterialGPUData.h"
#include "RenderContracts/RenderIdentity.h"
#include "RenderContracts/RenderMaterial.h"
#include "RenderContracts/ResourceUploadRequest.h"
#include "RHI/RHI.h"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace RVX
{
    /** ABI revision of MaterialGPUConstants-backed structured material tables. */
    inline constexpr uint32 RVX_MATERIAL_PARAMETER_TABLE_SCHEMA_VERSION = 4;

    /**
     * Fixed set-2 descriptor ABI shared by every DefaultLit material draw.
     *
     * Texture bindings are deliberately derived from the upload slot ordinal:
     * slots 0..12 map to t1..13. DX12/Vulkan/OpenGL use samplers s14..26,
     * while DX11 and Metal use the documented compact primary range s14..21
     * and validate extension aliases before recording. The material parameter
     * table remains the final SRV at t27. Keep these
     * names at the public MaterialSystem boundary so recording paths and
     * validation cannot silently retain the legacy five-texture layout.
     */
    inline constexpr uint32 RVX_MATERIAL_DESCRIPTOR_CONSTANT_BUFFER_BINDING = 0;
    inline constexpr uint32 RVX_MATERIAL_DESCRIPTOR_FIRST_TEXTURE_BINDING = 1;
    inline constexpr uint32 RVX_MATERIAL_DESCRIPTOR_FIRST_SAMPLER_BINDING =
        RVX_MATERIAL_DESCRIPTOR_FIRST_TEXTURE_BINDING +
        RVX_MATERIAL_GPU_TEXTURE_SLOT_COUNT;
    inline constexpr uint32 RVX_MATERIAL_DESCRIPTOR_PARAMETER_TABLE_BINDING =
        RVX_MATERIAL_DESCRIPTOR_FIRST_SAMPLER_BINDING +
        RVX_MATERIAL_GPU_TEXTURE_SLOT_COUNT;

    [[nodiscard]] constexpr uint32 GetMaterialTextureDescriptorBinding(
        MaterialUploadTextureSlot slot) noexcept
    {
        return RVX_MATERIAL_DESCRIPTOR_FIRST_TEXTURE_BINDING +
            static_cast<uint32>(slot);
    }

    [[nodiscard]] constexpr uint32 GetMaterialSamplerDescriptorBinding(
        MaterialUploadTextureSlot slot) noexcept
    {
        return RVX_MATERIAL_DESCRIPTOR_FIRST_SAMPLER_BINDING +
            static_cast<uint32>(slot);
    }

    static_assert(RVX_MATERIAL_DESCRIPTOR_PARAMETER_TABLE_BINDING == 27);

    class RenderRetirementQueue;
    class FrameConstantUploadArena;
    class RenderResourceRegistry;
    class RenderSubmissionTracker;
    class ResourceViewCache;
    class RHICommandContext;
    struct GPUCompletionToken;

    enum class MaterialBindingStatus : uint8
    {
        None = 0,
        Ready,
        Fallback,
        NotInitialized,
        Unavailable,
        Error
    };

    /** @brief Value-only sampler semantics included in descriptor evidence. */
    struct MaterialBindingSamplerPolicy
    {
        MaterialUploadWrapMode wrapS = MaterialUploadWrapMode::Repeat;
        MaterialUploadWrapMode wrapT = MaterialUploadWrapMode::Repeat;
        MaterialUploadFilterMode minFilter =
            MaterialUploadFilterMode::LinearMipmapLinear;
        MaterialUploadFilterMode magFilter = MaterialUploadFilterMode::Linear;

        [[nodiscard]] bool operator==(
            const MaterialBindingSamplerPolicy&) const = default;
    };

    /**
     * @brief Exact texture generation selected for one material descriptor slot.
     *
     * This is deliberately independent of descriptor heap pages, RHI views,
     * and native sampler objects so it can be retained as presentation proof.
     */
    struct MaterialBindingTextureEntry
    {
        MaterialUploadTextureSlot slot = MaterialUploadTextureSlot::BaseColor;
        RenderResourceHandle texture{};
        uint64 contentRevision = 0;
        bool fallbackUsed = false;
        MaterialBindingSamplerPolicy sampler{};

        [[nodiscard]] bool IsValid() const noexcept
        {
            return texture.IsValid() && contentRevision != 0;
        }
    };

    struct MaterialBindingResult
    {
        MaterialBindingStatus status = MaterialBindingStatus::None;
        /** Exact committed Render material identity selected by this draw. */
        RenderResourceHandle material{};
        uint64 contentRevision = 0;
        /** Deterministic value key for the material + exact texture semantics. */
        uint64 descriptorContentKey = 0;
        /** Owner-monotonic revision advanced only when that semantic key changes. */
        uint64 descriptorRevision = 0;
        /** Frozen V2 material ABI identity consumed by this recorded draw. */
        uint64 parameterLayoutHash = GetMaterialParameterLayoutHashV2();
        uint64 featureMask = 0;
        MaterialShadingModel shadingModel = MaterialShadingModel::StandardLit;
        MaterialBlendMode blendMode = MaterialBlendMode::Opaque;
        SurfacePathEligibility surfacePathEligibility =
            SurfacePathEligibility::DeferredOpaque;
        /** Sorted by MaterialUploadTextureSlot. */
        std::vector<MaterialBindingTextureEntry> textureEntries{};
        RHIDescriptorSet* descriptorSet = nullptr;
        // Keep the exact page resources alive through planned-draw ownership.
        RHIBufferRef constantBuffer;
        RHIDescriptorSetRef descriptorSetRef;
        std::array<uint32, 1> dynamicOffsets = {0};
        uint32 textureFlags = 0;
        uint32 fallbackTextureFlags = 0;
        bool constantsUpdated = false;
        bool usedFallback = false;
        std::string materialName;
        std::string message;

        bool IsDrawable() const
        {
            return constantsUpdated &&
                   descriptorSet &&
                   parameterLayoutHash == GetMaterialParameterLayoutHashV2() &&
                   surfacePathEligibility != SurfacePathEligibility::Unsupported &&
                   (status == MaterialBindingStatus::Ready ||
                    status == MaterialBindingStatus::Fallback);
        }

        bool IsError() const
        {
            return status == MaterialBindingStatus::NotInitialized ||
                   status == MaterialBindingStatus::Unavailable ||
                   status == MaterialBindingStatus::Error;
        }
    };

    struct MaterialBindingOptions
    {
        bool allowNormalMap = true;
        /** Optional frame-owned table used by an instanced material draw. */
        RHIBuffer* materialParameterTable = nullptr;
    };

    struct MaterialParameterTableEntryRequest
    {
        RenderResourceHandle material;
        bool allowNormalMap = true;
    };

    /**
     * @brief CPU-only proof for one material consumed through a parameter table.
     *
     * Instanced and GPU-driven draws bind one descriptor set for a whole
     * material table. This value preserves every exact material generation
     * that the shader may index, without manufacturing per-material RHI
     * bindings solely for diagnostics.
     */
    struct MaterialParameterTableBindingEvidence
    {
        RenderResourceHandle material{};
        uint64 contentRevision = 0;
        uint64 descriptorContentKey = 0;
        uint64 descriptorRevision = 0;
        std::vector<MaterialBindingTextureEntry> textureEntries{};
        uint64 parameterLayoutHash = GetMaterialParameterLayoutHashV2();
        uint64 featureMask = 0;
        MaterialShadingModel shadingModel = MaterialShadingModel::StandardLit;
        MaterialBlendMode blendMode = MaterialBlendMode::Opaque;
        SurfacePathEligibility surfacePathEligibility =
            SurfacePathEligibility::DeferredOpaque;
        bool fallbackUsed = false;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return material.IsValid() && contentRevision != 0 &&
                   descriptorContentKey != 0 && descriptorRevision != 0 &&
                   parameterLayoutHash == GetMaterialParameterLayoutHashV2() &&
                   (featureMask & ~RVX_MATERIAL_FEATURE_MASK_V2) == 0 &&
                   shadingModel <= MaterialShadingModel::ThinTransmission &&
                   surfacePathEligibility != SurfacePathEligibility::Unsupported &&
                   !fallbackUsed &&
                   std::all_of(textureEntries.begin(), textureEntries.end(),
                       [](const MaterialBindingTextureEntry& entry)
                       {
                           return entry.IsValid() && !entry.fallbackUsed;
                       });
        }
    };

    /**
     * @brief CPU-only, value-only proof for one stable shader table slot.
     *
     * Texture descriptors and mesh-dependent fallback decisions are
     * intentionally excluded. This lets Deferred/outline consumers prove the
     * exact shading-model contract without conflating it with raster binding
     * compatibility. A non-asset row is an explicit zero-fill/default row;
     * the sole empty-scene sentinel is additionally marked on the snapshot.
     */
    struct MaterialParameterTableRowEvidence
    {
        uint32 stableSlot = 0;
        bool isAsset = false;
        MaterialShadingModel shadingModel =
            MaterialShadingModel::StandardLit;
        uint64 programFamilyHash = 0;
        uint64 parameterLayoutHash = 0;
        float32 toonBands = 0.0f;
        float32 toonSpecularThreshold = 0.0f;
        float32 toonRimStrength = 0.0f;
        float32 toonOutlineWidth = 0.0f;
        Vec3 toonShadowTint{0.0f, 0.0f, 0.0f};
        bool toonParametersValid = false;

        [[nodiscard]] bool IsStructurallyValid() const noexcept
        {
            if (programFamilyHash == 0 || parameterLayoutHash == 0)
            {
                return false;
            }
            if (!isAsset)
            {
                return shadingModel == MaterialShadingModel::StandardLit &&
                       !toonParametersValid;
            }
            return shadingModel != MaterialShadingModel::ToonLit
                ? !toonParametersValid
                : toonParametersValid;
        }
    };

    /**
     * @brief Stable, process-independent value identity for a program family.
     *
     * The caller must still validate the public family string through the
     * material contract. This identity is evidence, never a substitute for
     * the typed MaterialShadingModel.
     */
    [[nodiscard]] uint64 GetMaterialProgramFamilyValueIdentity(
        std::string_view programFamily) noexcept;

    /** @brief Recording-owned stable-slot GPU material parameter table. */
    struct MaterialParameterTableSnapshot
    {
        RHIBufferRef buffer;
        uint32 schemaVersion = 0;
        uint32 stride = 0;
        uint32 slotCount = 0;
        uint32 materialCount = 0;
        /**
         * True only when a classification snapshot represents an empty
         * Deferred draw set with its required StandardLit sentinel at slot 0.
         * The sentinel is a GPU safety row, never a registered asset.
         */
        bool usesDefaultClassificationSentinel = false;
        /** Deterministic FNV-1a transcript of table bytes and slot identities. */
        uint64 contentHash = 0;
        /** Value-only evidence indexed by stable shader material slot. */
        std::vector<MaterialParameterTableRowEvidence> valueOnlyRowEvidence;
        /** True only for the texture-resolved raster parameter-table path. */
        bool containsResolvedRasterBindingEvidence = false;
        /** Sorted exact-generation receipts for every resolved table asset. */
        std::vector<MaterialParameterTableBindingEvidence>
            resolvedRasterBindingEvidence;
        /**
         * CPU-only resolved material evidence, indexed by the exact stable
         * shader material slot. Published only after the table write commits.
         */
        std::vector<uint64> rasterMaterialSemanticKeysBySlot;

        [[nodiscard]] bool IsValid() const noexcept
        {
            if (!buffer ||
                schemaVersion != RVX_MATERIAL_PARAMETER_TABLE_SCHEMA_VERSION ||
                stride != sizeof(MaterialGPUConstants) || slotCount == 0 ||
                materialCount == 0 || contentHash == 0 ||
                valueOnlyRowEvidence.size() != slotCount ||
                rasterMaterialSemanticKeysBySlot.size() != slotCount ||
                (containsResolvedRasterBindingEvidence
                     ? resolvedRasterBindingEvidence.size() != materialCount
                     : !resolvedRasterBindingEvidence.empty()))
            {
                return false;
            }
            if (!std::all_of(valueOnlyRowEvidence.begin(),
                             valueOnlyRowEvidence.end(),
                             [](const MaterialParameterTableRowEvidence& row)
                             {
                                 return row.IsStructurallyValid();
                             }))
            {
                return false;
            }
            const uint32 assetRowCount = static_cast<uint32>(std::count_if(
                valueOnlyRowEvidence.begin(),
                valueOnlyRowEvidence.end(),
                [](const MaterialParameterTableRowEvidence& row)
                {
                    return row.isAsset;
                }));
            if (usesDefaultClassificationSentinel)
            {
                return slotCount == 1 && materialCount == 1 &&
                       !containsResolvedRasterBindingEvidence &&
                       !valueOnlyRowEvidence.front().isAsset;
            }
            if (assetRowCount != materialCount)
            {
                return false;
            }
            return !containsResolvedRasterBindingEvidence ||
                   (std::is_sorted(
                        resolvedRasterBindingEvidence.begin(),
                        resolvedRasterBindingEvidence.end(),
                        [](const MaterialParameterTableBindingEvidence& lhs,
                           const MaterialParameterTableBindingEvidence& rhs)
                        {
                            return lhs.material < rhs.material;
                        }) &&
                    std::adjacent_find(
                        resolvedRasterBindingEvidence.begin(),
                        resolvedRasterBindingEvidence.end(),
                        [](const MaterialParameterTableBindingEvidence& lhs,
                           const MaterialParameterTableBindingEvidence& rhs)
                        {
                            return lhs.material == rhs.material;
                        }) == resolvedRasterBindingEvidence.end() &&
                    std::all_of(
                        resolvedRasterBindingEvidence.begin(),
                        resolvedRasterBindingEvidence.end(),
                        [](const MaterialParameterTableBindingEvidence& value)
                        {
                            return value.IsValid();
                        }));
        }
    };

    /** @brief Recording-owned material constants and descriptor snapshot. */
    struct MaterialBindingSnapshot
    {
        RHIBufferRef constantBuffer;
        RHIDescriptorSetRef descriptorSet;
        RHIDescriptorSetLayoutRef layout;
        std::vector<RHITextureViewRef> textureViews;
        // Descriptor views do not necessarily own their parent textures on
        // every backend (notably Vulkan).  Keep the textures alongside the
        // views so a graph recording remains valid until submission retires.
        std::vector<RHITextureRef> textures;
        std::array<RHISamplerRef, RVX_MATERIAL_GPU_TEXTURE_SLOT_COUNT> samplers;
        MaterialBindingResult binding;

        [[nodiscard]] bool IsDrawable() const noexcept
        {
            return constantBuffer && descriptorSet && layout && binding.IsDrawable();
        }
    };

    /**
     * @brief Owns material GPU constants, fallback textures, and set 2 descriptors.
     */
    class MaterialSystem
    {
    public:
        MaterialSystem();
        ~MaterialSystem();

        MaterialSystem(const MaterialSystem&) = delete;
        MaterialSystem& operator=(const MaterialSystem&) = delete;

        // =====================================================================
        // Lifecycle
        // =====================================================================

        bool Initialize(IRHIDevice* device,
                        RHIDescriptorSetLayout* materialSetLayout,
                        RenderResourceRegistry* resourceRegistry,
                        ResourceViewCache* resourceViewCache = nullptr);
        void Shutdown();
        bool IsInitialized() const { return m_initialized; }

        // =====================================================================
        // Per-Frame
        // =====================================================================

        void BeginFrame();

        /** @brief Transfer descriptor replacements using the prior-submit snapshot. */
        void RetireOwnerSnapshots(const GPUCompletionToken& completion,
                                  RenderRetirementQueue& retirement);
        void SetMaterialConstantSubmissionTracker(RenderSubmissionTracker* tracker) noexcept;
        [[nodiscard]] bool NotifyMaterialConstantSubmission(const GPUCompletionToken& completion) noexcept;
        void ReleaseUnsubmittedMaterialConstants() noexcept;

        // =====================================================================
        // Material Binding Data
        // =====================================================================

        MaterialBindingResult PrepareMaterialBinding(RenderResourceHandle material,
                                                     ResourceViewCache* viewCache,
                                                     MaterialBindingOptions options = {});

        /**
         * @brief Resolve one material into recording-owned constants/descriptors.
         *
         * This path never allocates from the mutable frame material ring.  The
         * returned binding has a fixed zero dynamic offset and owns its
         * constant buffer/descriptor for graph execution and submission.
         */
        bool CreateMaterialBindingSnapshot(RenderResourceHandle material,
                                           ResourceViewCache* viewCache,
                                           MaterialBindingOptions options,
                                           MaterialBindingSnapshot& outSnapshot);
        RHIDescriptorSet* GetDefaultMaterialSet();

        std::array<uint32, 1> GetCurrentMaterialDynamicOffset() const;
        const MaterialBindingResult& GetLastBindingResult() const { return m_lastBindingResult; }
        const std::string& GetLastBindingMessage() const { return m_lastBindingResult.message; }

        /** Resolve value-only texture/sampler compatibility for batch planning. */
        [[nodiscard]] MaterialInstanceBindingKey ResolveInstanceBindingKey(
            RenderResourceHandle material) const noexcept;

        /**
         * @brief Resolve immutable raster policy from the canonical material descriptor.
         *
         * A valid material with absent/invalid metadata returns false. Callers
         * must not substitute a permissive raster state in that case.
         */
        [[nodiscard]] bool ResolveSurfacePipelinePolicy(
            RenderResourceHandle material,
            MaterialCullMode& outCullMode,
            MaterialShadowPolicy& outShadowPolicy,
            bool& outRequiresUV1) const noexcept;

        /**
         * Resolve the complete immutable raster policy, including the single
         * forward/deferred route selected from MaterialStaticDescriptor.
         *
         * A failed result always leaves @p outSurfacePathEligibility as
         * Unsupported, so callers cannot accidentally retain a prior route.
         */
        [[nodiscard]] bool ResolveSurfacePipelinePolicy(
            RenderResourceHandle material,
            MaterialCullMode& outCullMode,
            MaterialShadowPolicy& outShadowPolicy,
            bool& outRequiresUV1,
            SurfacePathEligibility& outSurfacePathEligibility) const noexcept;

        /**
         * Resolve the complete draw-owned material classification.
         *
         * Blend, shading model, and sort priority are copied only from a
         * validated descriptor. Failure restores explicit safe defaults so a
         * caller can never retain a prior translucent classification.
         */
        [[nodiscard]] bool ResolveSurfacePipelinePolicy(
            RenderResourceHandle material,
            MaterialCullMode& outCullMode,
            MaterialShadowPolicy& outShadowPolicy,
            bool& outRequiresUV1,
            SurfacePathEligibility& outSurfacePathEligibility,
            MaterialBlendMode& outBlendMode,
            MaterialShadingModel& outShadingModel,
            int32& outSortPriority) const noexcept;

        /**
         * @brief Materialize exact-generation parameters at stable handle slots.
         *
         * Duplicate slot generations or incompatible per-mesh normal-map
         * requirements fail closed instead of changing table meaning.
         */
        [[nodiscard]] bool CreateMaterialParameterTableSnapshot(
            std::span<const MaterialParameterTableEntryRequest> requests,
            ResourceViewCache* viewCache,
            MaterialParameterTableSnapshot& outSnapshot);

        /**
         * @brief Materialize descriptor-only constants at stable handle slots.
         *
         * This deliberately does not resolve textures, samplers, mesh tangent
         * requirements, or descriptor fallbacks. Deferred lighting and
         * screen-space classification consume this table as an immutable
         * shading-model contract, while raster draws continue to use
         * CreateMaterialParameterTableSnapshot for texture bindings. An empty
         * draw set yields one explicit StandardLit sentinel row at slot zero,
         * so a Deferred pass never binds an invalid structured buffer.
         */
        [[nodiscard]] bool CreateMaterialClassificationTableSnapshot(
            std::span<const RenderResourceHandle> materials,
            MaterialParameterTableSnapshot& outSnapshot) const;

    private:
        void QueueMaterialDescriptorCacheRetirement();
        static uint64 AlignConstantBufferSize(uint64 size);
        static uint32 ToRHIConstantDynamicOffset(uint64 offset)
        {
            constexpr uint64 maxDynamicOffset = static_cast<uint64>(std::numeric_limits<uint32>::max());
            if (offset > maxDynamicOffset)
            {
                RVX_VERIFY(false, "MaterialSystem: dynamic constant offset {} exceeds the RHI uint32 offset limit",
                           offset);
                return 0;
            }

            return static_cast<uint32>(offset);
        }

        struct ResolvedMaterialTextures
        {
            RHITextureView* baseColor = nullptr;
            RHITextureView* normal = nullptr;
            RHITextureView* metallicRoughness = nullptr;
            RHITextureView* occlusion = nullptr;
            RHITextureView* emissive = nullptr;
            RHITextureView* toonRamp = nullptr;
            RHITextureView* transmission = nullptr;
            RHITextureView* thickness = nullptr;
            RHITextureView* specular = nullptr;
            RHITextureView* specularColor = nullptr;
            RHITextureView* clearcoat = nullptr;
            RHITextureView* clearcoatRoughness = nullptr;
            RHITextureView* clearcoatNormal = nullptr;
            RHISampler* baseColorSampler = nullptr;
            RHISampler* normalSampler = nullptr;
            RHISampler* metallicRoughnessSampler = nullptr;
            RHISampler* occlusionSampler = nullptr;
            RHISampler* emissiveSampler = nullptr;
            RHISampler* toonRampSampler = nullptr;
            RHISampler* transmissionSampler = nullptr;
            RHISampler* thicknessSampler = nullptr;
            RHISampler* specularSampler = nullptr;
            RHISampler* specularColorSampler = nullptr;
            RHISampler* clearcoatSampler = nullptr;
            RHISampler* clearcoatRoughnessSampler = nullptr;
            RHISampler* clearcoatNormalSampler = nullptr;
            RHIBuffer* materialParameterTable = nullptr;
            uint32 textureFlags = 0;
            uint32 fallbackTextureFlags = 0;
            uint64 viewGeneration = 0;
            uint64 pageIdentity = 0;
            std::vector<MaterialBindingTextureEntry> textureEntries;
            std::vector<MaterialUploadTextureBinding> textureBindings;
            bool usedFallback = false;
            bool normalMapDisabled = false;
            bool compactSamplerPolicyUnsupported = false;
            std::string compactSamplerPolicyMessage;
        };

        struct MaterialDescriptorKey
        {
            RHITextureView* baseColor = nullptr;
            RHITextureView* normal = nullptr;
            RHITextureView* metallicRoughness = nullptr;
            RHITextureView* occlusion = nullptr;
            RHITextureView* emissive = nullptr;
            RHITextureView* toonRamp = nullptr;
            RHITextureView* transmission = nullptr;
            RHITextureView* thickness = nullptr;
            RHITextureView* specular = nullptr;
            RHITextureView* specularColor = nullptr;
            RHITextureView* clearcoat = nullptr;
            RHITextureView* clearcoatRoughness = nullptr;
            RHITextureView* clearcoatNormal = nullptr;
            RHISampler* baseColorSampler = nullptr;
            RHISampler* normalSampler = nullptr;
            RHISampler* metallicRoughnessSampler = nullptr;
            RHISampler* occlusionSampler = nullptr;
            RHISampler* emissiveSampler = nullptr;
            RHISampler* toonRampSampler = nullptr;
            RHISampler* transmissionSampler = nullptr;
            RHISampler* thicknessSampler = nullptr;
            RHISampler* specularSampler = nullptr;
            RHISampler* specularColorSampler = nullptr;
            RHISampler* clearcoatSampler = nullptr;
            RHISampler* clearcoatRoughnessSampler = nullptr;
            RHISampler* clearcoatNormalSampler = nullptr;
            RHIBuffer* materialParameterTable = nullptr;
            uint64 viewGeneration = 0;
            uint64 pageIdentity = 0;

            bool operator==(const MaterialDescriptorKey& other) const
            {
                return baseColor == other.baseColor &&
                       normal == other.normal &&
                       metallicRoughness == other.metallicRoughness &&
                       occlusion == other.occlusion &&
                       emissive == other.emissive &&
                       toonRamp == other.toonRamp &&
                       transmission == other.transmission &&
                       thickness == other.thickness &&
                       specular == other.specular &&
                       specularColor == other.specularColor &&
                       clearcoat == other.clearcoat &&
                       clearcoatRoughness == other.clearcoatRoughness &&
                       clearcoatNormal == other.clearcoatNormal &&
                       baseColorSampler == other.baseColorSampler &&
                       normalSampler == other.normalSampler &&
                       metallicRoughnessSampler ==
                           other.metallicRoughnessSampler &&
                       occlusionSampler == other.occlusionSampler &&
                       emissiveSampler == other.emissiveSampler &&
                       toonRampSampler == other.toonRampSampler &&
                       transmissionSampler == other.transmissionSampler &&
                       thicknessSampler == other.thicknessSampler &&
                       specularSampler == other.specularSampler &&
                       specularColorSampler == other.specularColorSampler &&
                       clearcoatSampler == other.clearcoatSampler &&
                       clearcoatRoughnessSampler ==
                           other.clearcoatRoughnessSampler &&
                       clearcoatNormalSampler == other.clearcoatNormalSampler &&
                       materialParameterTable ==
                           other.materialParameterTable &&
                       viewGeneration == other.viewGeneration &&
                       pageIdentity == other.pageIdentity;
            }
        };

        struct MaterialDescriptorKeyHash
        {
            size_t operator()(const MaterialDescriptorKey& key) const;
        };

        struct MaterialDescriptorCacheEntry
        {
            RHIDescriptorSetRef descriptorSet;
            std::array<RHISamplerRef, RVX_MATERIAL_GPU_TEXTURE_SLOT_COUNT> samplers;
            // A descriptor set only stores backend descriptor addresses. Keep
            // a custom per-frame parameter table alive for as long as the
            // cached descriptor can be reused.
            RHIBufferRef materialParameterTable;
        };

        struct MaterialSetResolveResult
        {
            RHIDescriptorSet* descriptorSet = nullptr;
            RHIDescriptorSetRef descriptorSetRef;
            bool usedFallback = false;
            MaterialBindingStatus status = MaterialBindingStatus::None;
            std::string message;
        };

        bool CreateConstantBuffer();
        bool CreateDefaultResources();
        [[nodiscard]] bool UsesCompactMaterialSamplerABI() const noexcept;
        [[nodiscard]] bool ValidateCompactMaterialSamplerPolicies(
            std::span<const MaterialUploadTextureBinding> bindings,
            std::string& outError) const;
        [[nodiscard]] bool ValidateResolvedCompactMaterialSamplerPolicies(
            std::span<const MaterialBindingTextureEntry> entries,
            std::string& outError) const;
        ResolvedMaterialTextures ResolveMaterialTextures(RenderResourceHandle material,
                                                        ResourceViewCache* viewCache,
                                                        MaterialBindingOptions options) const;
        MaterialBindingResult PrepareResolvedMaterialBinding(
            RenderResourceHandle material,
            uint64 contentRevision,
            MaterialStaticDescriptor staticDescriptor,
            MaterialSourceData source,
            const ResolvedMaterialTextures& textures,
            std::string materialName);
        void FinalizeSemanticDescriptorEvidence(
            MaterialBindingResult& result,
            RenderResourceHandle requestedMaterial,
            const MaterialGPUConstants& constants,
            const ResolvedMaterialTextures& textures);
        [[nodiscard]] uint64 ResolveSemanticDescriptorRevision(
            RenderResourceHandle material,
            uint64 descriptorContentKey);
        [[nodiscard]] uint64 BuildSemanticDescriptorContentKey(
            RenderResourceHandle requestedMaterial,
            const MaterialGPUConstants& constants,
            const std::vector<MaterialBindingTextureEntry>& textureEntries) const noexcept;
        [[nodiscard]] uint64 BuildMaterialClassificationSemanticKey(
            RenderResourceHandle material,
            const MaterialGPUConstants& constants) const noexcept;
        MaterialSetResolveResult GetOrCreateMaterialSetForResolved(
            const ResolvedMaterialTextures& textures,
            const RHIBufferRef& constantBuffer);
        RHIDescriptorSetRef CreateMaterialDescriptorSet(
            const ResolvedMaterialTextures& textures,
            RHIBuffer* constantBuffer);
        const MaterialBindingResult& SetLastBindingResult(MaterialBindingResult result);
        uint64 AllocateMaterialConstantSlot();

        IRHIDevice* m_device = nullptr;
        RenderResourceRegistry* m_resourceRegistry = nullptr;
        ResourceViewCache* m_resourceViewCache = nullptr;
        RHIDescriptorSetLayout* m_materialSetLayout = nullptr;
        bool m_initialized = false;

        RHIBufferRef m_materialConstantBuffer;
        RHIBufferRef m_defaultMaterialParameterTable;
        std::unique_ptr<FrameConstantUploadArena> m_materialConstantUploadArena;
        uint64 m_materialConstantStride = 0;
        uint64 m_materialConstantCursor = 0;
        uint64 m_currentMaterialConstantOffset = 0;

        RHITextureRef m_defaultWhiteTexture;
        RHITextureRef m_defaultNormalTexture;
        RHITextureRef m_defaultBlackTexture;
        RHITextureViewRef m_defaultWhiteTextureView;
        RHITextureViewRef m_defaultNormalTextureView;
        RHITextureViewRef m_defaultBlackTextureView;
        RHISamplerRef m_defaultSampler;
        RHIDescriptorSetRef m_defaultMaterialSet;
        std::unordered_map<MaterialDescriptorKey,
                           MaterialDescriptorCacheEntry,
                           MaterialDescriptorKeyHash>
            m_materialDescriptorCache;
        std::vector<Ref<RefCounted>> m_pendingOwnerRetirements;
        uint64 m_materialDescriptorCacheGeneration = ~uint64{0};
        struct SemanticDescriptorState
        {
            uint64 contentKey = 0;
            uint64 revision = 0;
        };
        std::unordered_map<RenderResourceHandle,
                           SemanticDescriptorState,
                           RenderResourceHandleHash>
            m_semanticDescriptorStates;
        MaterialBindingResult m_lastBindingResult;
    };

} // namespace RVX
