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

#include <array>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace RVX
{
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

    /** @brief Recording-owned stable-slot GPU material parameter table. */
    struct MaterialParameterTableSnapshot
    {
        RHIBufferRef buffer;
        uint32 slotCount = 0;
        uint32 materialCount = 0;
        /**
         * CPU-only resolved material evidence, indexed by the exact stable
         * shader material slot. Published only after the table write commits.
         */
        std::vector<uint64> rasterMaterialSemanticKeysBySlot;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return buffer && slotCount != 0 && materialCount != 0;
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
        std::array<RHISamplerRef, 5> samplers;
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
         * @brief Materialize exact-generation parameters at stable handle slots.
         *
         * Duplicate slot generations or incompatible per-mesh normal-map
         * requirements fail closed instead of changing table meaning.
         */
        [[nodiscard]] bool CreateMaterialParameterTableSnapshot(
            std::span<const MaterialParameterTableEntryRequest> requests,
            ResourceViewCache* viewCache,
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
            RHISampler* baseColorSampler = nullptr;
            RHISampler* normalSampler = nullptr;
            RHISampler* metallicRoughnessSampler = nullptr;
            RHISampler* occlusionSampler = nullptr;
            RHISampler* emissiveSampler = nullptr;
            RHIBuffer* materialParameterTable = nullptr;
            uint32 textureFlags = 0;
            uint32 fallbackTextureFlags = 0;
            uint64 viewGeneration = 0;
            uint64 pageIdentity = 0;
            std::vector<MaterialBindingTextureEntry> textureEntries;
            bool usedFallback = false;
            bool normalMapDisabled = false;
        };

        struct MaterialDescriptorKey
        {
            RHITextureView* baseColor = nullptr;
            RHITextureView* normal = nullptr;
            RHITextureView* metallicRoughness = nullptr;
            RHITextureView* occlusion = nullptr;
            RHITextureView* emissive = nullptr;
            RHISampler* baseColorSampler = nullptr;
            RHISampler* normalSampler = nullptr;
            RHISampler* metallicRoughnessSampler = nullptr;
            RHISampler* occlusionSampler = nullptr;
            RHISampler* emissiveSampler = nullptr;
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
                       baseColorSampler == other.baseColorSampler &&
                       normalSampler == other.normalSampler &&
                       metallicRoughnessSampler ==
                           other.metallicRoughnessSampler &&
                       occlusionSampler == other.occlusionSampler &&
                       emissiveSampler == other.emissiveSampler &&
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
            std::array<RHISamplerRef, 5> samplers;
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
        ResolvedMaterialTextures ResolveMaterialTextures(RenderResourceHandle material,
                                                        ResourceViewCache* viewCache,
                                                        MaterialBindingOptions options) const;
        MaterialBindingResult PrepareResolvedMaterialBinding(
            RenderResourceHandle material,
            uint64 contentRevision,
            MaterialSourceData source,
            const ResolvedMaterialTextures& textures,
            std::string materialName);
        void FinalizeSemanticDescriptorEvidence(
            MaterialBindingResult& result,
            RenderResourceHandle requestedMaterial,
            const MaterialGPUConstants& constants,
            const ResolvedMaterialTextures& textures);
        [[nodiscard]] uint64 BuildSemanticDescriptorContentKey(
            RenderResourceHandle requestedMaterial,
            const MaterialGPUConstants& constants,
            const std::vector<MaterialBindingTextureEntry>& textureEntries) const noexcept;
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
