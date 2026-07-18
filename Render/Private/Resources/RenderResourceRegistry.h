#pragma once

/** @file RenderResourceRegistry.h @brief Exact-generation Render-owned RHI resources */

#include "RenderContracts/IRenderResourceGateway.h"
#include "Resources/RenderSubmissionTracker.h"
#include "RHI/RHIBuffer.h"
#include "RHI/RHISampler.h"
#include "RHI/RHITexture.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <variant>
#include <vector>

namespace RVX
{
    class RenderResourceStatusTable;
    class RenderRetirementQueue;
    class IRenderMeshUploadSource;
    class IRenderTextureUploadSource;
    class RHICommandContext;
    struct MeshGPUBuffers;
    enum class UploadPriority : uint8_t;
    enum class GPUResourceState : uint8_t;

    struct LegacyGPUResourceStats
    {
        size_t residentMeshCount = 0;
        size_t residentTextureCount = 0;
        size_t pendingUploadCount = 0;
        size_t queuedUploadCount = 0;
        size_t uploadingCount = 0;
        size_t failedUploadCount = 0;
        size_t usedMemory = 0;
        size_t memoryBudget = 0;
    };

    enum class RenderMeshBufferSemantic : uint8
    {
        Position = 0,
        Normal,
        UV,
        Tangent,
        BoneIndices,
        BoneWeights,
        Index,
    };

    struct RenderOwnedBuffer
    {
        RenderMeshBufferSemantic semantic = RenderMeshBufferSemantic::Position;
        RHIBufferRef buffer;
        uint64 estimatedBytes = 0;
    };

    struct RenderMeshResourceData
    {
        std::vector<RenderOwnedBuffer> buffers;
    };

    struct RenderTextureResourceData
    {
        RHITextureRef texture;
        uint64 estimatedBytes = 0;
    };

    struct RenderMaterialResourceData
    {
        RHIBufferRef constants;
        uint64 constantBytes = 0;
        std::vector<RHISamplerRef> samplers;
    };

    using RenderResourceGPUData = std::variant<
        RenderMeshResourceData,
        RenderTextureResourceData,
        RenderMaterialResourceData>;

    /** @brief Sole owner of pending and committed resource RHI references. */
    class RenderResourceRegistry final
    {
    public:
        RenderResourceRegistry() = default;
        ~RenderResourceRegistry();

        RenderResourceRegistry(const RenderResourceRegistry&) = delete;
        RenderResourceRegistry& operator=(const RenderResourceRegistry&) = delete;

        [[nodiscard]] bool Initialize(RenderResourceStatusTable* statusTable,
                                      RenderRetirementQueue* retirementQueue);
        void Shutdown();

        [[nodiscard]] bool BeginPending(
            RenderResourceHandle handle,
            RenderResourceKind kind,
            const std::vector<RenderResourceHandle>& dependencies);
        [[nodiscard]] bool AddPendingMeshBuffer(
            RenderResourceHandle handle,
            RenderMeshBufferSemantic semantic,
            RHIBufferRef buffer,
            uint64 estimatedBytes);
        [[nodiscard]] bool SetPendingTexture(
            RenderResourceHandle handle,
            RHITextureRef texture,
            uint64 estimatedBytes);
        [[nodiscard]] bool SetPendingMaterialConstants(
            RenderResourceHandle handle,
            RHIBufferRef constants,
            uint64 estimatedBytes);
        [[nodiscard]] bool AddPendingMaterialSampler(
            RenderResourceHandle handle,
            RHISamplerRef sampler);
        [[nodiscard]] bool SetPendingCompletion(
            RenderResourceHandle handle,
            const GPUCompletionToken& completion);
        [[nodiscard]] bool Commit(RenderResourceHandle handle);
        [[nodiscard]] bool RetirePending(RenderResourceHandle handle);
        [[nodiscard]] bool Release(RenderResourceHandle handle);
        [[nodiscard]] bool MergeLastUse(
            RenderResourceHandle handle,
            const GPUCompletionToken& completion);

        [[nodiscard]] const RenderMeshResourceData* ResolveMesh(
            RenderResourceHandle handle) const;
        [[nodiscard]] const RenderTextureResourceData* ResolveTexture(
            RenderResourceHandle handle) const;
        [[nodiscard]] const RenderMaterialResourceData* ResolveMaterial(
            RenderResourceHandle handle) const;
        [[nodiscard]] const std::vector<RenderResourceHandle>*
            GetDependencies(RenderResourceHandle handle) const;
        [[nodiscard]] GPUCompletionToken GetLastUse(
            RenderResourceHandle handle) const;
        [[nodiscard]] bool HasExactEntry(RenderResourceHandle handle) const;
        [[nodiscard]] bool IsGPUReadyExact(RenderResourceHandle handle) const;
        [[nodiscard]] bool HasPending(RenderResourceHandle handle) const;
        [[nodiscard]] uint32 GetEntryCount() const;

        // Task-18 compatibility API. New code must use exact handles above.
        void InitializeLegacy(IRHIDevice* device);
        void ShutdownLegacy();
        [[nodiscard]] bool IsLegacyInitialized() const;
        void RequestLegacyUpload(
            IRenderMeshUploadSource* mesh,
            UploadPriority priority);
        void RequestLegacyUpload(
            IRenderTextureUploadSource* texture,
            UploadPriority priority);
        void UploadLegacyImmediate(IRenderMeshUploadSource* mesh);
        void UploadLegacyImmediate(IRenderTextureUploadSource* texture);
        void SetLegacyTextureInvalidatedCallback(
            std::function<void(RHITexture*)> callback);
        [[nodiscard]] MeshGPUBuffers GetLegacyMeshBuffers(uint64 meshId) const;
        [[nodiscard]] RHITexture* GetLegacyTexture(uint64 textureId) const;
        [[nodiscard]] RHITexture* GetLegacyTexture(
            IRenderTextureUploadSource* texture) const;
        [[nodiscard]] bool TransitionLegacyTexture(
            uint64 textureId,
            RHICommandContext& context,
            RHIResourceState desiredState);
        [[nodiscard]] bool IsLegacyResident(uint64 id) const;
        [[nodiscard]] bool IsLegacyResident(
            IRenderTextureUploadSource* texture) const;
        [[nodiscard]] GPUResourceState GetLegacyResourceState(uint64 id) const;
        [[nodiscard]] bool IsLegacyGPUReady(
            IRenderTextureUploadSource* texture) const;
        void ProcessLegacyPendingUploads(float timeBudgetMs);
        void MarkLegacyUsed(uint64 id);
        void MarkLegacyUsed(IRenderTextureUploadSource* texture);
        void EvictLegacyUnused(uint64 currentFrame, uint64 frameThreshold);
        void SetLegacyMemoryBudget(size_t bytes);
        [[nodiscard]] size_t GetLegacyUsedMemory() const;
        [[nodiscard]] size_t GetLegacyMemoryBudget() const;
        [[nodiscard]] LegacyGPUResourceStats GetLegacyStats() const;

    private:
        struct LegacyResourceState;
        struct Entry
        {
            uint32 generation = 0;
            RenderResourceKind kind = RenderResourceKind::Invalid;
            std::vector<RenderResourceHandle> dependencies;
            std::optional<RenderResourceGPUData> pending;
            std::optional<RenderResourceGPUData> committed;
            GPUCompletionToken lastUse;
        };

        Entry* FindExact(RenderResourceHandle handle);
        const Entry* FindExact(RenderResourceHandle handle) const;
        [[nodiscard]] bool RetireData(RenderResourceGPUData& data,
                                      const GPUCompletionToken& completion);
        [[nodiscard]] bool IsReady(RenderResourceHandle handle,
                                   RenderResourceKind kind) const;

        RenderResourceStatusTable* m_statusTable = nullptr;
        RenderRetirementQueue* m_retirementQueue = nullptr;
        std::unordered_map<uint32, Entry> m_entries;
        LegacyResourceState* m_legacy = nullptr;
    };
} // namespace RVX
