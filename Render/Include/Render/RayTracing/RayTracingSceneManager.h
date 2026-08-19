#pragma once

/**
 * @file RayTracingSceneManager.h
 * @brief Runtime cache for scene ray-tracing acceleration structures
 */

#include "Core/MathTypes.h"
#include "Render/RayTracing/RayTracingScene.h"
#include "RHI/RHIBuffer.h"
#include "RHI/RHIDevice.h"

#include <algorithm>
#include <deque>
#include <vector>

namespace RVX
{
    class RHICommandContext;
    class RenderRetirementQueue;
    struct GPUCompletionToken;

    enum class RayTracingInstanceAlphaMetadataFlags : uint32
    {
        None = 0,
        AlphaTestEnabled = 1 << 0,
        HasBaseColorTexture = 1 << 1,
        HasResolvedBaseColorTexture = 1 << 2,
        HasUVBuffer = 1 << 3,
        HasIndexBuffer = 1 << 4,
        IndexFormatUInt32 = 1 << 5,
        IndexFormatUInt16 = 1 << 6,
        HasNormalBuffer = 1 << 7,
        HasTangentBuffer = 1 << 8
    };

    inline RayTracingInstanceAlphaMetadataFlags operator|(
        RayTracingInstanceAlphaMetadataFlags a,
        RayTracingInstanceAlphaMetadataFlags b)
    {
        return static_cast<RayTracingInstanceAlphaMetadataFlags>(
            static_cast<uint32>(a) | static_cast<uint32>(b));
    }

    inline bool HasFlag(RayTracingInstanceAlphaMetadataFlags flags,
                        RayTracingInstanceAlphaMetadataFlags flag)
    {
        return (static_cast<uint32>(flags) & static_cast<uint32>(flag)) != 0;
    }

    struct RayTracingInstanceAlphaMetadata
    {
        uint32 flags = 0;
        uint32 baseColorUVSet = 0;
        float alphaCutoff = 0.5f;
        float baseColorAlpha = 1.0f;
        uint32 baseColorTextureIdLow = 0;
        uint32 baseColorTextureIdHigh = 0;
        uint32 baseColorTextureTableIndex = RVX_INVALID_INDEX;
        uint32 indexBufferTableIndex = RVX_INVALID_INDEX;
        uint32 uvBufferTableIndex = RVX_INVALID_INDEX;
        uint32 indexElementOffset = 0;
        uint32 baseVertex = 0;
        uint32 baseColorSamplerFlags = 0;
        uint32 normalBufferTableIndex = RVX_INVALID_INDEX;
        uint32 tangentBufferTableIndex = RVX_INVALID_INDEX;
        Vec2 baseColorUVOffset{0.0f, 0.0f};
        Vec2 baseColorUVScale{1.0f, 1.0f};
        float baseColorUVRotation = 0.0f;
        float reserved2 = 0.0f;
    };

    static_assert(sizeof(RayTracingInstanceAlphaMetadata) == 80,
                  "RayTracingInstanceAlphaMetadata must match the ray tracing shader layout");

    struct RayTracingInstanceTextureSamplingMetadata
    {
        Vec2 uvOffset{0.0f, 0.0f};
        Vec2 uvScale{1.0f, 1.0f};
        float uvRotation = 0.0f;
        uint32 uvSet = 0;
        uint32 samplerFlags = 0;
        uint32 reserved = 0;
    };

    static_assert(sizeof(RayTracingInstanceTextureSamplingMetadata) == 32,
                  "RayTracingInstanceTextureSamplingMetadata must match the ray tracing shader layout");

    struct RayTracingInstanceMaterialMetadata
    {
        Vec4 baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
        Vec4 emissiveFactor{0.0f, 0.0f, 0.0f, 1.0f};
        Vec4 materialFactors{1.0f, 1.0f, 0.5f, 1.0f};
        uint32 flags = 0;
        uint32 materialIdLow = 0;
        uint32 materialIdHigh = 0;
        uint32 workflow = 0;
        uint32 baseColorTextureTableIndex = RVX_INVALID_INDEX;
        uint32 metallicRoughnessTextureTableIndex = RVX_INVALID_INDEX;
        uint32 normalTextureTableIndex = RVX_INVALID_INDEX;
        uint32 emissiveTextureTableIndex = RVX_INVALID_INDEX;
        RayTracingInstanceTextureSamplingMetadata baseColorTextureSampling;
        RayTracingInstanceTextureSamplingMetadata metallicRoughnessTextureSampling;
        RayTracingInstanceTextureSamplingMetadata normalTextureSampling;
        RayTracingInstanceTextureSamplingMetadata emissiveTextureSampling;
    };

    static_assert(sizeof(RayTracingInstanceMaterialMetadata) == 208,
                  "RayTracingInstanceMaterialMetadata must match the ray tracing shader layout");

    enum class RayTracingSceneFallbackCode : uint8
    {
        None,
        MissingDevice,
        RayTracingUnsupported,
        EmptyBuildPlan,
        BottomLevelASCreationFailed,
        TopLevelInstanceMappingFailed,
        InvalidTopLevelDescription,
        InstanceMetadataOrderMismatch,
        MissingBottomLevelASAddress,
        InstanceBufferUpdateFailed,
        MaterialMetadataBufferUpdateFailed,
        AlphaMetadataBufferUpdateFailed,
        TopLevelASCreationFailed,
    };

    struct RayTracingSceneManagerStats
    {
        bool supported = false;
        bool prepared = false;
        bool hasTopLevelAS = false;
        bool hasInstanceBuffer = false;
        bool hasMaterialMetadataBuffer = false;
        bool hasAlphaMetadataBuffer = false;
        size_t requestedBLASCount = 0;
        size_t cachedBLASCount = 0;
        size_t createdBLASCount = 0;
        size_t reusedBLASCount = 0;
        size_t evictedBLASCount = 0;
        size_t resourceBudgetEvictedBLASCount = 0;
        size_t releasedBLASScratchCount = 0;
        size_t pendingBLASScratchReleaseCount = 0;
        size_t pendingBLASBuildCount = 0;
        size_t instanceCount = 0;
        size_t alphaTestedInstanceCount = 0;
        size_t materialTextureCount = 0;
        size_t alphaTextureCount = 0;
        size_t alphaIndexBufferCount = 0;
        size_t alphaUVBufferCount = 0;
        size_t alphaNormalBufferCount = 0;
        size_t alphaTangentBufferCount = 0;
        size_t skippedCount = 0;
        size_t recordedBLASBuildCount = 0;
        uint64 cachedBLASAccelerationStructureBytes = 0;
        uint64 cachedBLASScratchBytes = 0;
        uint64 releasedBLASScratchBytes = 0;
        uint64 topLevelAccelerationStructureBytes = 0;
        uint64 topLevelScratchBytes = 0;
        uint64 instanceBufferBytes = 0;
        uint64 materialMetadataBufferBytes = 0;
        uint64 alphaMetadataBufferBytes = 0;
        uint64 totalTrackedResourceBytes = 0;
        uint64 trackedResourceBudget = 0;
        bool resourceBudgetExceeded = false;
        bool resourceBudgetEvictionAttempted = false;
        bool resourceByteAccountingOverflowed = false;
        bool recordedTLASBuild = false;
        RayTracingSceneFallbackCode fallbackCode = RayTracingSceneFallbackCode::None;
        const char* fallbackReason = "";
    };

    /**
     * @brief Owns and updates BLAS/TLAS resources for hybrid raster + ray tracing.
     *
     * The manager intentionally separates preparation from command recording:
     * - Prepare() creates/reuses AS resources and uploads native TLAS instance records.
     * - RecordBuildCommands() emits BLAS builds that are dirty plus the per-frame TLAS build.
     */
    class RayTracingSceneManager
    {
    public:
        RayTracingSceneManager() = default;
        ~RayTracingSceneManager();

        RayTracingSceneManager(const RayTracingSceneManager&) = delete;
        RayTracingSceneManager& operator=(const RayTracingSceneManager&) = delete;

        // =====================================================================
        // Lifecycle
        // =====================================================================

        void Initialize(IRHIDevice* device);
        void Shutdown();

        bool IsInitialized() const { return m_device != nullptr; }
        bool IsSupported() const;

        // =====================================================================
        // Frame Preparation
        // =====================================================================

        bool Prepare(const RayTracingSceneBuildPlan& plan);
        void RecordBuildCommands(RHICommandContext& ctx);

        /** @brief Transfer replaced AS/buffer owners using prior-submit evidence. */
        void RetireOwnerSnapshots(const GPUCompletionToken& completion,
                                  RenderRetirementQueue& retirement);

        // =====================================================================
        // Accessors
        // =====================================================================

        RHIAccelerationStructure* GetTopLevelAS() const { return m_topLevelAS.Get(); }
        RHIBuffer* GetInstanceBuffer() const { return m_instanceBuffer.Get(); }
        RHIBuffer* GetInstanceMaterialMetadataBuffer() const { return m_instanceMaterialMetadataBuffer.Get(); }
        RHIBuffer* GetInstanceAlphaMetadataBuffer() const { return m_instanceAlphaMetadataBuffer.Get(); }
        RHIBuffer* GetTopLevelScratchBuffer() const { return m_topLevelScratchBuffer.Get(); }
        void GatherPendingBuildScratchBuffers(std::vector<RHIBuffer*>& outScratchBuffers) const;
        const RHITopLevelASDesc& GetTopLevelBuildDesc() const { return m_topLevelBuildDesc; }
        const RayTracingSceneManagerStats& GetStats() const { return m_stats; }
        size_t GetCachedBLASCount() const { return m_blasCache.size(); }
        void SetBLASCacheEvictionFrameThreshold(uint64 frameThreshold)
        {
            m_blasCacheEvictionFrameThreshold = frameThreshold;
        }
        uint64 GetBLASCacheEvictionFrameThreshold() const { return m_blasCacheEvictionFrameThreshold; }
        void SetTrackedResourceBudget(uint64 budgetBytes)
        {
            m_trackedResourceBudget = budgetBytes;
        }
        uint64 GetTrackedResourceBudget() const { return m_trackedResourceBudget; }
        const std::vector<uint64>& GetInstanceMaterialTextureTable() const
        {
            return m_instanceMaterialTextureIds;
        }
        const std::vector<uint64>& GetInstanceAlphaTextureTable() const
        {
            return m_instanceAlphaTextureIds;
        }
        const std::vector<RHIBuffer*>& GetInstanceAlphaIndexBufferTable() const
        {
            return m_instanceAlphaIndexBuffers;
        }
        const std::vector<RHIBuffer*>& GetInstanceAlphaUVBufferTable() const
        {
            return m_instanceAlphaUVBuffers;
        }
        const std::vector<RHIBuffer*>& GetInstanceAlphaNormalBufferTable() const
        {
            return m_instanceAlphaNormalBuffers;
        }
        const std::vector<RHIBuffer*>& GetInstanceAlphaTangentBufferTable() const
        {
            return m_instanceAlphaTangentBuffers;
        }

    private:
        struct BLASCacheEntry
        {
            RayTracingBLASKey key;
            RHIBottomLevelASDesc desc;
            RHIAccelerationStructureBuildSizes sizes;
            RHIAccelerationStructureRef accelerationStructure;
            RHIBufferRef scratchBuffer;
            bool needsBuild = false;
            bool scratchReleasePending = false;
            uint64 lastUsedFrame = 0;
            uint64 scratchLastUsedFrame = 0;
        };

        BLASCacheEntry* FindBLAS(const RayTracingBLASKey& key);
        BLASCacheEntry* GetOrCreateBLAS(const RayTracingBLASBuild& build);
        void ReleaseRetiredBLASScratchBuffers();
        void EvictUnusedBLAS(const std::vector<RayTracingBLASBuild>& requestedBuilds);
        void EvictUnusedBLASForResourceBudget(const std::vector<RayTracingBLASBuild>& requestedBuilds);
        bool EnsureScratchBuffer(RHIBufferRef& buffer, uint64 size, const char* debugName);
        bool EnsureInstanceBuffer(const std::vector<RHIRayTracingInstanceRecord>& records);
        bool EnsureInstanceMaterialMetadataBuffer(const std::vector<RayTracingInstanceMaterialMetadata>& records);
        bool EnsureInstanceAlphaMetadataBuffer(const std::vector<RayTracingInstanceAlphaMetadata>& records);
        bool EnsureTopLevelAS(const RHITopLevelASDesc& desc);
        bool AreBLASDescriptionsEquivalent(const RHIBottomLevelASDesc& lhs,
                                           const RHIBottomLevelASDesc& rhs) const;
        void UpdateResourceStats();
        void ResetFrameState();
        void InvalidateFrameOutputs();
        void SetFallback(RayTracingSceneFallbackCode code, const char* reason);

        IRHIDevice* m_device = nullptr;
        std::deque<BLASCacheEntry> m_blasCache;
        std::vector<BLASCacheEntry*> m_frameBLAS;
        std::vector<BLASCacheEntry*> m_pendingBLASBuilds;
        std::vector<RHIRayTracingInstanceRecord> m_instanceRecords;
        std::vector<RayTracingInstanceMaterialMetadata> m_instanceMaterialMetadataRecords;
        std::vector<uint64> m_instanceMaterialTextureIds;
        std::vector<RayTracingInstanceAlphaMetadata> m_instanceAlphaMetadataRecords;
        std::vector<uint64> m_instanceAlphaTextureIds;
        std::vector<RHIBuffer*> m_instanceAlphaIndexBuffers;
        std::vector<RHIBuffer*> m_instanceAlphaUVBuffers;
        std::vector<RHIBuffer*> m_instanceAlphaNormalBuffers;
        std::vector<RHIBuffer*> m_instanceAlphaTangentBuffers;
        RHIBufferRef m_instanceBuffer;
        uint64 m_instanceBufferSize = 0;
        RHIBufferRef m_instanceMaterialMetadataBuffer;
        uint64 m_instanceMaterialMetadataBufferSize = 0;
        RHIBufferRef m_instanceAlphaMetadataBuffer;
        uint64 m_instanceAlphaMetadataBufferSize = 0;
        RHIAccelerationStructureRef m_topLevelAS;
        RHIAccelerationStructureBuildSizes m_topLevelSizes;
        RHIBufferRef m_topLevelScratchBuffer;
        RHITopLevelASDesc m_topLevelBuildDesc;
        RayTracingSceneManagerStats m_stats;
        uint64 m_frameCounter = 0;
        uint64 m_blasCacheEvictionFrameThreshold = 300;
        uint64 m_trackedResourceBudget = 0;
        std::vector<Ref<RefCounted>> m_pendingOwnerRetirements;
    };

} // namespace RVX
