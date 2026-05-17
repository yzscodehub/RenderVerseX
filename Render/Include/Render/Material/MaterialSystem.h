#pragma once

/**
 * @file MaterialSystem.h
 * @brief GPU material descriptor and constant management
 */

#include "Core/Assert.h"
#include "Core/Types.h"
#include "Render/Material/MaterialGPUData.h"
#include "RHI/RHI.h"

#include <array>
#include <limits>
#include <unordered_map>

namespace RVX
{
    class GPUResourceManager;
    class ResourceViewCache;

    namespace Resource
    {
        class MaterialResource;
        class TextureResource;
    } // namespace Resource

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
                        RHIDescriptorSetLayout* materialSetLayout);
        void Shutdown();
        bool IsInitialized() const { return m_initialized; }

        // =====================================================================
        // Per-Frame
        // =====================================================================

        void BeginFrame();

        // =====================================================================
        // Material Binding Data
        // =====================================================================

        void UpdateMaterialConstants(const Resource::MaterialResource* materialResource,
                                     ResourceViewCache* viewCache);
        RHIDescriptorSet* GetOrCreateMaterialSet(const Resource::MaterialResource* materialResource,
                                                 ResourceViewCache* viewCache);
        RHIDescriptorSet* GetDefaultMaterialSet();

        std::array<uint32, 1> GetCurrentMaterialDynamicOffset() const;

    private:
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
            uint32 textureFlags = 0;
            uint64 viewGeneration = 0;
        };

        struct MaterialDescriptorKey
        {
            RHITextureView* baseColor = nullptr;
            RHITextureView* normal = nullptr;
            RHITextureView* metallicRoughness = nullptr;
            RHITextureView* occlusion = nullptr;
            RHITextureView* emissive = nullptr;
            uint64 viewGeneration = 0;

            bool operator==(const MaterialDescriptorKey& other) const
            {
                return baseColor == other.baseColor &&
                       normal == other.normal &&
                       metallicRoughness == other.metallicRoughness &&
                       occlusion == other.occlusion &&
                       emissive == other.emissive &&
                       viewGeneration == other.viewGeneration;
            }
        };

        struct MaterialDescriptorKeyHash
        {
            size_t operator()(const MaterialDescriptorKey& key) const;
        };

        bool CreateConstantBuffer();
        bool CreateDefaultResources();
        RHITextureView* ResolveTextureView(const Resource::TextureResource* textureResource,
                                           RHITextureView* fallbackView,
                                           ResourceViewCache* viewCache,
                                           uint32 textureFlag,
                                           uint32& textureFlags) const;
        ResolvedMaterialTextures ResolveMaterialTextures(const Resource::MaterialResource* materialResource,
                                                        ResourceViewCache* viewCache) const;
        MaterialGPUConstants BuildConstants(const Resource::MaterialResource* materialResource,
                                            const ResolvedMaterialTextures& textures) const;
        RHIDescriptorSetRef CreateMaterialDescriptorSet(const ResolvedMaterialTextures& textures);
        uint64 AllocateMaterialConstantSlot();

        IRHIDevice* m_device = nullptr;
        GPUResourceManager* m_gpuResources = nullptr;
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
        RHIDescriptorSetRef m_defaultMaterialSet;
        std::unordered_map<MaterialDescriptorKey, RHIDescriptorSetRef, MaterialDescriptorKeyHash> m_materialDescriptorCache;
        uint64 m_materialDescriptorCacheGeneration = ~uint64{0};
    };

} // namespace RVX
