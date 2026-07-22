#pragma once

/**
 * @file MaterialSystem.h
 * @brief GPU material descriptor and constant management
 */

#include "Core/Assert.h"
#include "Core/Types.h"
#include "Render/Material/MaterialGPUData.h"
#include "RenderContracts/RenderIdentity.h"
#include "RenderContracts/RenderResource.h"
#include "RHI/RHI.h"

#include <array>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace RVX
{
    class GPUResourceManager;
    class RenderRetirementQueue;
    class RenderResourceRegistry;
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

    struct MaterialBindingResult
    {
        MaterialBindingStatus status = MaterialBindingStatus::None;
        RHIDescriptorSet* descriptorSet = nullptr;
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

        bool Initialize(IRHIDevice* device, GPUResourceManager* gpuResources,
                        RHIDescriptorSetLayout* materialSetLayout,
                        const RenderResourceRegistry* resourceRegistry = nullptr);
        void Shutdown();
        bool IsInitialized() const { return m_initialized; }

        // =====================================================================
        // Per-Frame
        // =====================================================================

        void BeginFrame();

        /** @brief Transfer descriptor replacements using the prior-submit snapshot. */
        void RetireOwnerSnapshots(const GPUCompletionToken& completion,
                                  RenderRetirementQueue& retirement);

        // =====================================================================
        // Material Binding Data
        // =====================================================================

        MaterialBindingResult PrepareMaterialBinding(const IRenderMaterialSource* materialResource,
                                                     ResourceViewCache* viewCache,
                                                     MaterialBindingOptions options = {});
        MaterialBindingResult PrepareMaterialBinding(RenderResourceHandle material,
                                                     ResourceViewCache* viewCache,
                                                     MaterialBindingOptions options = {});
        void RequestMaterialTextures(const IRenderMaterialSource* materialResource) const;
        void TransitionMaterialTextures(const IRenderMaterialSource* materialResource,
                                        RHICommandContext& ctx,
                                        MaterialBindingOptions options = {}) const;
        bool UpdateMaterialConstants(const IRenderMaterialSource* materialResource,
                                     ResourceViewCache* viewCache,
                                     MaterialBindingOptions options = {});
        RHIDescriptorSet* GetOrCreateMaterialSet(const IRenderMaterialSource* materialResource,
                                                 ResourceViewCache* viewCache,
                                                 MaterialBindingOptions options = {});
        RHIDescriptorSet* GetDefaultMaterialSet();

        std::array<uint32, 1> GetCurrentMaterialDynamicOffset() const;
        const MaterialBindingResult& GetLastBindingResult() const { return m_lastBindingResult; }
        const std::string& GetLastBindingMessage() const { return m_lastBindingResult.message; }

        struct EnvironmentIBLResources
        {
            IRenderTextureUploadSource* irradianceMap = nullptr;
            IRenderTextureUploadSource* prefilteredMap = nullptr;
            IRenderTextureUploadSource* brdfLUT = nullptr;
            RenderResourceHandle irradianceHandle;
            RenderResourceHandle prefilteredHandle;
            RenderResourceHandle brdfLUTHandle;
            uint32 prefilteredMipLevels = 1;
            float intensity = 1.0f;
            bool textureIBLEnabled = false;
        };

        void SetEnvironmentIBLResources(const EnvironmentIBLResources& resources);
        void SetEnvironmentIBLResources(RenderResourceHandle irradiance,
                                        RenderResourceHandle prefiltered,
                                        RenderResourceHandle brdfLUT,
                                        float intensity);
        void ClearEnvironmentIBLResources();
        const EnvironmentIBLResources& GetEnvironmentIBLResources() const { return m_environmentIBL; }

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
            RHITextureView* irradiance = nullptr;
            RHITextureView* prefilteredEnvironment = nullptr;
            RHITextureView* brdfLUT = nullptr;
            uint32 textureFlags = 0;
            uint32 fallbackTextureFlags = 0;
            uint64 viewGeneration = 0;
            bool textureIBLEnabled = false;
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
            RHITextureView* irradiance = nullptr;
            RHITextureView* prefilteredEnvironment = nullptr;
            RHITextureView* brdfLUT = nullptr;
            uint64 viewGeneration = 0;
            bool textureIBLEnabled = false;

            bool operator==(const MaterialDescriptorKey& other) const
            {
                return baseColor == other.baseColor &&
                       normal == other.normal &&
                       metallicRoughness == other.metallicRoughness &&
                       occlusion == other.occlusion &&
                       emissive == other.emissive &&
                       irradiance == other.irradiance &&
                       prefilteredEnvironment == other.prefilteredEnvironment &&
                       brdfLUT == other.brdfLUT &&
                       viewGeneration == other.viewGeneration &&
                       textureIBLEnabled == other.textureIBLEnabled;
            }
        };

        struct MaterialDescriptorKeyHash
        {
            size_t operator()(const MaterialDescriptorKey& key) const;
        };

        struct MaterialSetResolveResult
        {
            RHIDescriptorSet* descriptorSet = nullptr;
            bool usedFallback = false;
            MaterialBindingStatus status = MaterialBindingStatus::None;
            std::string message;
        };

        bool CreateConstantBuffer();
        bool CreateDefaultResources();
        RHITextureView* ResolveTextureView(IRenderTextureUploadSource* textureResource,
                                           RHITextureView* fallbackView,
                                           ResourceViewCache* viewCache,
                                           uint32 textureFlag,
                                           uint32& textureFlags,
                                           uint32& fallbackTextureFlags,
                                           bool& usedFallback) const;
        ResolvedMaterialTextures ResolveMaterialTextures(const IRenderMaterialSource* materialResource,
                                                        ResourceViewCache* viewCache,
                                                        MaterialBindingOptions options) const;
        ResolvedMaterialTextures ResolveMaterialTextures(RenderResourceHandle material,
                                                        ResourceViewCache* viewCache,
                                                        MaterialBindingOptions options) const;
        MaterialGPUConstants BuildConstants(const IRenderMaterialSource* materialResource,
                                            const ResolvedMaterialTextures& textures) const;
        MaterialBindingResult PrepareResolvedMaterialBinding(
            MaterialSourceData source,
            const ResolvedMaterialTextures& textures,
            std::string materialName);
        MaterialSetResolveResult GetOrCreateMaterialSetForResolved(const ResolvedMaterialTextures& textures);
        RHIDescriptorSetRef CreateMaterialDescriptorSet(const ResolvedMaterialTextures& textures);
        const MaterialBindingResult& SetLastBindingResult(MaterialBindingResult result);
        uint64 AllocateMaterialConstantSlot();

        IRHIDevice* m_device = nullptr;
        GPUResourceManager* m_gpuResources = nullptr;
        const RenderResourceRegistry* m_resourceRegistry = nullptr;
        RHIDescriptorSetLayout* m_materialSetLayout = nullptr;
        bool m_initialized = false;

        RHIBufferRef m_materialConstantBuffer;
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
        RHITextureRef m_defaultBlackCubemap;
        RHITextureViewRef m_defaultBlackCubemapView;
        RHIDescriptorSetRef m_defaultMaterialSet;
        std::unordered_map<MaterialDescriptorKey, RHIDescriptorSetRef, MaterialDescriptorKeyHash> m_materialDescriptorCache;
        std::vector<Ref<RefCounted>> m_pendingOwnerRetirements;
        uint64 m_materialDescriptorCacheGeneration = ~uint64{0};
        EnvironmentIBLResources m_environmentIBL;
        MaterialBindingResult m_lastBindingResult;
    };

} // namespace RVX
