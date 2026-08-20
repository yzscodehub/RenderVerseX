#pragma once

/**
 * @file GPUUploadService.h
 * @brief First-stage CPU-to-GPU upload helper for render resources.
 */

#include "Render/GPUUploadTypes.h"
#include "RHI/RHIDevice.h"

#include <memory>

namespace RVX
{
    class RenderSubmissionTracker;
    class RenderUploadProcessor;

    /**
     * @brief Temporary upload facade removed by task 18 after all callers use
     * Render upload requests and exact-generation registry resolution.
     */
    class GPUUploadService
    {
    public:
        using Stats = GPUUploadStats;

        GPUUploadService();
        ~GPUUploadService();

        void Initialize(IRHIDevice* device);
        void Shutdown();
        bool IsInitialized() const;

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

        const Stats& GetStats() const;
        void ResetStats();

    private:
        // Temporary task-18 migration adapter. Ownership and completion live in
        // RenderUploadProcessor/RenderSubmissionTracker, never in this facade.
        std::unique_ptr<RenderSubmissionTracker> m_submissionTracker;
        std::unique_ptr<RenderUploadProcessor> m_processor;
    };

} // namespace RVX
