#pragma once

/**
 * @file GPUUploadService.h
 * @brief First-stage CPU-to-GPU upload helper for render resources.
 */

#include "RHI/RHIBuffer.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"
#include "RHI/RHISynchronization.h"
#include "RHI/RHITexture.h"
#include "RHI/RHIUpload.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace RVX
{
    /**
     * @brief CPU-to-GPU upload path used for a request.
     */
    enum class GPUUploadMode : uint8
    {
        None = 0,
        ImmediateMapped,
        StagedCopy
    };

    /**
     * @brief Diagnostic reason for an upload failure.
     */
    enum class GPUUploadFailureReason : uint8
    {
        None = 0,
        InvalidDevice,
        InvalidData,
        InvalidDescription,
        CreateResourceFailed,
        MapFailed,
        CopyFailed,
        Unsupported
    };

    template <typename ResourceRef>
    struct GPUUploadResult
    {
        ResourceRef resource;
        bool succeeded = false;
        GPUUploadMode mode = GPUUploadMode::None;
        GPUUploadFailureReason failureReason = GPUUploadFailureReason::None;
        uint64 bytesUploaded = 0;
        uint64 uploadId = 0;
        bool isPending = false;

        explicit operator bool() const { return succeeded; }
    };

    using GPUUploadBufferResult = GPUUploadResult<RHIBufferRef>;
    using GPUUploadTextureResult = GPUUploadResult<RHITextureRef>;

    struct GPUUploadBufferDesc
    {
        uint64 size = 0;
        RHIBufferUsage usage = RHIBufferUsage::None;
        uint32 stride = 0;
        const char* debugName = nullptr;
    };

    struct GPUUploadTextureDesc
    {
        RHITextureDesc textureDesc;
        uint64 dataSize = 0;
    };

    class GPUUploadService
    {
    public:
        struct Stats
        {
            uint32 bufferUploadCount = 0;
            uint32 textureUploadCount = 0;
            uint32 failedUploadCount = 0;
            uint32 immediateUploadCount = 0;
            uint32 stagedUploadCount = 0;
            uint32 pendingUploadCount = 0;
            uint32 completedUploadCount = 0;
            uint64 uploadedBytes = 0;
            uint64 stagingBytesInFlight = 0;
            uint64 peakStagingBytesInFlight = 0;
        };

        GPUUploadService() = default;
        ~GPUUploadService();

        void Initialize(IRHIDevice* device);
        void Shutdown();
        bool IsInitialized() const { return m_device != nullptr; }

        RHIBufferRef UploadBufferData(const GPUUploadBufferDesc& desc, const void* data, uint64 dataSize);
        RHITextureRef UploadTextureData(const GPUUploadTextureDesc& desc, const void* data);
        GPUUploadBufferResult UploadBufferDataWithResult(const GPUUploadBufferDesc& desc, const void* data, uint64 dataSize);
        GPUUploadTextureResult UploadTextureDataWithResult(const GPUUploadTextureDesc& desc, const void* data);
        void FlushBatchUploads();

        /// Submit queued staged uploads, wait for GPU completion, and publish completed markers.
        uint32 FlushAndWaitForUploads();

        uint32 ProcessCompletedUploads();
        bool IsUploadPending(uint64 uploadId) const;
        bool IsUploadComplete(uint64 uploadId) const;

        /// Remove a completed upload marker after the owning system has consumed it.
        void ForgetCompletedUpload(uint64 uploadId);

        /// Keep a pending upload alive until GPU completion, but suppress its completed marker.
        void AbandonUpload(uint64 uploadId);

        const Stats& GetStats() const { return m_stats; }
        void ResetStats() { m_stats = {}; }

    private:
        struct PendingUpload
        {
            uint64 id = 0;
            uint64 fenceValue = 0;
            uint64 stagingBytes = 0;
            RHIStagingBufferRef stagingBuffer;
            RHICommandContextRef commandContext;
            RHIFenceRef fence;
        };

        GPUUploadBufferResult TryUploadBufferStaged(const GPUUploadBufferDesc& desc, const void* data, uint64 dataSize);
        GPUUploadBufferResult UploadBufferImmediate(const GPUUploadBufferDesc& desc, const void* data, uint64 dataSize);
        GPUUploadTextureResult TryUploadTextureStaged(const GPUUploadTextureDesc& desc, const void* data);
        RHICommandContextRef GetOrCreateBatchCommandContext();
        RHIFenceRef AcquireFence();
        void RetainPendingFence(RHIFence* fence);
        void ReleasePendingFence(const RHIFenceRef& fence);
        uint64 TrackPendingUpload(RHIStagingBufferRef stagingBuffer, RHICommandContextRef commandContext,
                                  RHIFenceRef fence, uint64 fenceValue, uint64 stagingBytes);
        uint32 CompleteBatchUploadsWithoutFence(RHICommandContext* commandContext);
        GPUUploadBufferResult MakeBufferFailure(GPUUploadFailureReason reason);
        GPUUploadTextureResult MakeTextureFailure(GPUUploadFailureReason reason);
        void RecordFailure(GPUUploadFailureReason reason);

        IRHIDevice* m_device = nullptr;
        Stats m_stats;
        std::vector<PendingUpload> m_pendingUploads;
        std::unordered_set<uint64> m_completedUploads;
        std::unordered_set<uint64> m_abandonedUploads;
        std::vector<RHIFenceRef> m_availableFences;
        std::unordered_map<RHIFence*, uint32> m_pendingFenceUseCounts;
        RHICommandContextRef m_batchCommandContext;
        bool m_batchCommandContextDirty = false;
        uint64 m_nextUploadId = 1;
    };

} // namespace RVX
