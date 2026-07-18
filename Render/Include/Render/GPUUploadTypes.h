#pragma once

/** @file GPUUploadTypes.h @brief Transitional upload values shared with Render internals */

#include "RHI/RHIBuffer.h"
#include "RHI/RHITexture.h"

namespace RVX
{
    enum class GPUUploadMode : uint8
    {
        None = 0,
        ImmediateMapped,
        StagedCopy
    };

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

    struct GPUUploadStats
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
} // namespace RVX
