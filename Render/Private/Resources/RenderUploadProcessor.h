#pragma once

/** @file RenderUploadProcessor.h @brief Two-phase immutable Render upload processing */

#include "RenderContracts/ResourceUploadRequest.h"
#include "Render/GPUUploadTypes.h"
#include "Resources/RenderResourceRegistry.h"
#include "Resources/RenderSubmissionTracker.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHISampler.h"
#include "RHI/RHIUpload.h"

#include <span>
#include <vector>

namespace RVX
{
    class IRHIDevice;
    class RenderResourceRegistry;
    class RenderResourceStatusTable;

    struct UploadOwnershipTransfers
    {
        std::vector<RHIBufferBarrier> bufferBarriers;
        std::vector<RHITextureBarrier> textureBarriers;

        [[nodiscard]] bool Empty() const
        {
            return bufferBarriers.empty() && textureBarriers.empty();
        }
    };

    enum class RenderUploadProcessCode : uint8
    {
        Accepted = 0,
        InvalidRequest,
        StaleState,
        DependencyUnavailable,
        ResourceCreationFailed,
        SubmissionFailed,
    };

    struct RenderUploadProcessorStats
    {
        uint64 accepted = 0;
        uint64 completed = 0;
        uint64 failed = 0;
        uint64 cancelled = 0;
        uint64 compatibilityWaits = 0;
        uint64 inFlightBytes = 0;
    };

    /** @brief Validates, records, completes, and terminally publishes uploads. */
    class RenderUploadProcessor final
    {
    public:
        RenderUploadProcessor();
        ~RenderUploadProcessor();

        RenderUploadProcessor(const RenderUploadProcessor&) = delete;
        RenderUploadProcessor& operator=(const RenderUploadProcessor&) = delete;

        [[nodiscard]] bool Initialize(
            IRHIDevice* device,
            RenderResourceStatusTable* statusTable,
            RenderResourceRegistry* registry,
            RenderSubmissionTracker* submissionTracker);
        void Shutdown();
        /** @brief Classify all submitted work as lost without waiting for GPU progress. */
        void ShutdownDeviceLost();

        /** @brief Task-18 compatibility path used only by GPUUploadService. */
        [[nodiscard]] bool InitializeLegacy(
            IRHIDevice* device,
            RenderSubmissionTracker* submissionTracker);
        void ShutdownLegacy();
        [[nodiscard]] bool IsLegacyInitialized() const;
        RHIBufferRef UploadLegacyBufferData(
            const GPUUploadBufferDesc& desc,
            const void* data,
            uint64 dataSize);
        RHITextureRef UploadLegacyTextureData(
            const GPUUploadTextureDesc& desc,
            const void* data);
        GPUUploadBufferResult UploadLegacyBufferDataWithResult(
            const GPUUploadBufferDesc& desc,
            const void* data,
            uint64 dataSize);
        GPUUploadTextureResult UploadLegacyTextureDataWithResult(
            const GPUUploadTextureDesc& desc,
            const void* data);
        void FlushLegacyBatchUploads();
        uint32 FlushAndWaitForLegacyUploads();
        uint32 ProcessCompletedLegacyUploads();
        [[nodiscard]] bool IsLegacyUploadPending(uint64 uploadId) const;
        [[nodiscard]] bool IsLegacyUploadComplete(uint64 uploadId) const;
        void ForgetCompletedLegacyUpload(uint64 uploadId);
        void AbandonLegacyUpload(uint64 uploadId);
        [[nodiscard]] const GPUUploadStats& GetLegacyStats() const;
        void ResetLegacyStats();

        [[nodiscard]] RenderUploadProcessCode ProcessUpload(
            ResourceUploadRequestRef request);
        void ProcessRelease(RenderResourceHandle handle);
        [[nodiscard]] GPUCompletionStatus PollCompletion();

        [[nodiscard]] uint32 GetInFlightCount() const;
        [[nodiscard]] const RenderUploadProcessorStats& GetStats() const
        {
            return m_stats;
        }

    private:
        struct LegacyUploadState;
        struct InFlightUpload
        {
            RenderResourceHandle handle;
            ResourceUploadRequestRef request;
            GPUCompletionToken completion;
            RHICommandContextRef commandContext;
            RHICommandContextRef acquireCommandContext;
            std::vector<RHIStagingBufferRef> stagingBuffers;
            uint64 retainedBytes = 0;
            bool cancelled = false;
        };

        struct PendingRelease
        {
            RenderResourceHandle handle;
            GPUCompletionToken completion;
        };

        [[nodiscard]] bool ValidateDependencies(
            const ResourceUploadRequest& request) const;
        [[nodiscard]] bool BuildMesh(
            RenderResourceHandle handle,
            const MeshUploadPayload& payload,
            RHICommandContext& context,
            std::vector<RHIStagingBufferRef>& stagingBuffers,
            UploadOwnershipTransfers& ownershipTransfers);
        [[nodiscard]] bool BuildTexture(
            RenderResourceHandle handle,
            const TextureUploadPayload& payload,
            RHICommandContext& context,
            std::vector<RHIStagingBufferRef>& stagingBuffers,
            UploadOwnershipTransfers& ownershipTransfers);
        [[nodiscard]] bool BuildMaterial(
            RenderResourceHandle handle,
            const MaterialUploadPayload& payload,
            RHICommandContext& context,
            std::vector<RHIStagingBufferRef>& stagingBuffers,
            UploadOwnershipTransfers& ownershipTransfers);
        [[nodiscard]] bool RecordMeshBuffer(
            RenderResourceHandle handle,
            RenderMeshBufferSemantic semantic,
            RHIBufferUsage usage,
            std::span<const uint8> bytes,
            UploadByteRange range,
            RHICommandContext& context,
            std::vector<RHIStagingBufferRef>& stagingBuffers,
            UploadOwnershipTransfers& ownershipTransfers);
        [[nodiscard]] bool RecordMaterialConstants(
            RenderResourceHandle handle,
            const MaterialSourceData& sourceData,
            RHICommandContext& context,
            std::vector<RHIStagingBufferRef>& stagingBuffers,
            UploadOwnershipTransfers& ownershipTransfers);
        [[nodiscard]] RenderUploadProcessCode FailBeforeSubmission(
            ResourceUploadRequestRef& request,
            RenderResourceFailureCode failure,
            RenderUploadProcessCode code);
        void CompleteInFlight(size_t index, GPUCompletionStatus status);
        void CompleteRelease(size_t index, GPUCompletionStatus status);
        [[nodiscard]] bool PublishTerminal(
            RenderResourceHandle handle,
            RenderResourcePublicState state,
            RenderResourceFailureCode failure);
        [[nodiscard]] static RHIFormat ToRHIFormat(
            const TextureUploadCreateInfo& info);
        [[nodiscard]] static RHISamplerDesc ToSamplerDesc(
            const MaterialUploadTextureBinding& binding);

        IRHIDevice* m_device = nullptr;
        RenderResourceStatusTable* m_statusTable = nullptr;
        RenderResourceRegistry* m_registry = nullptr;
        RenderSubmissionTracker* m_submissionTracker = nullptr;
        std::vector<InFlightUpload> m_inFlight;
        std::vector<PendingRelease> m_pendingReleases;
        RenderUploadProcessorStats m_stats;
        std::unique_ptr<LegacyUploadState> m_legacy;
    };
} // namespace RVX
