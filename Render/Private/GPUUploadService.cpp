#include "Render/GPUUploadService.h"
#include "Core/Log.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIUpload.h"

#include <algorithm>
#include <cstring>

namespace RVX
{
    GPUUploadService::~GPUUploadService()
    {
        Shutdown();
    }

    void GPUUploadService::Initialize(IRHIDevice* device)
    {
        m_device = device;
        m_stats = {};
        m_pendingUploads.clear();
        m_completedUploads.clear();
        m_abandonedUploads.clear();
        m_availableFences.clear();
        m_pendingFenceUseCounts.clear();
        m_batchCommandContext.Reset();
        m_batchCommandContextDirty = false;
        m_nextUploadId = 1;
    }

    void GPUUploadService::Shutdown()
    {
        if (m_device)
        {
            FlushBatchUploads();
            if (!m_pendingUploads.empty())
            {
                m_device->WaitIdle();
            }
        }

        m_device = nullptr;
        m_stats = {};
        m_pendingUploads.clear();
        m_completedUploads.clear();
        m_abandonedUploads.clear();
        m_availableFences.clear();
        m_pendingFenceUseCounts.clear();
        m_batchCommandContext.Reset();
        m_batchCommandContextDirty = false;
        m_nextUploadId = 1;
    }

    namespace
    {
        constexpr uint64 RVX_TEXTURE_UPLOAD_ROW_PITCH_ALIGNMENT = 256;

        uint64 AlignUp(uint64 value, uint64 alignment)
        {
            return (value + alignment - 1) & ~(alignment - 1);
        }

        bool IsSupportedTextureUploadFormat(RHIFormat format)
        {
            if (format == RHIFormat::Unknown || IsDepthFormat(format) || IsCompressedFormat(format))
                return false;

            return GetFormatBytesPerPixel(format) > 0;
        }
    } // namespace

    RHIBufferRef GPUUploadService::UploadBufferData(const GPUUploadBufferDesc& desc, const void* data, uint64 dataSize)
    {
        return UploadBufferDataWithResult(desc, data, dataSize).resource;
    }

    RHITextureRef GPUUploadService::UploadTextureData(const GPUUploadTextureDesc& desc, const void* data)
    {
        return UploadTextureDataWithResult(desc, data).resource;
    }

    void GPUUploadService::FlushBatchUploads()
    {
        if (!m_device || !m_batchCommandContext || !m_batchCommandContextDirty)
            return;

        RHICommandContextRef submittedContext = m_batchCommandContext;
        submittedContext->End();

        RHIFenceRef fence = AcquireFence();
        const uint64 fenceValue = fence ? fence->GetCompletedValue() + 1 : 0;
        m_device->SubmitCommandContext(submittedContext.Get(), fence.Get());

        if (fence)
        {
            for (auto& upload : m_pendingUploads)
            {
                if (upload.commandContext.Get() == submittedContext.Get() && !upload.fence)
                {
                    upload.fence = fence;
                    upload.fenceValue = fenceValue;
                    RetainPendingFence(fence.Get());
                }
            }
        }
        else
        {
            m_device->WaitIdle();
            CompleteBatchUploadsWithoutFence(submittedContext.Get());
        }

        m_batchCommandContext.Reset();
        m_batchCommandContextDirty = false;
    }

    uint32 GPUUploadService::FlushAndWaitForUploads()
    {
        if (!m_device)
            return 0;

        uint32 completedCount = ProcessCompletedUploads();
        FlushBatchUploads();
        completedCount += ProcessCompletedUploads();

        if (!m_pendingUploads.empty())
        {
            m_device->WaitIdle();
            completedCount += ProcessCompletedUploads();
        }

        return completedCount;
    }

    uint32 GPUUploadService::ProcessCompletedUploads()
    {
        uint32 completedCount = 0;
        std::vector<RHIFenceRef> completedFences;

        auto it = m_pendingUploads.begin();
        while (it != m_pendingUploads.end())
        {
            if (!it->fence || it->fence->GetCompletedValue() < it->fenceValue)
            {
                ++it;
                continue;
            }

            if (it->fence)
            {
                completedFences.push_back(it->fence);
            }

            const bool isAbandoned = m_abandonedUploads.erase(it->id) > 0;
            if (!isAbandoned)
            {
                m_completedUploads.insert(it->id);
            }
            m_stats.completedUploadCount++;
            m_stats.pendingUploadCount--;
            m_stats.stagingBytesInFlight -= it->stagingBytes;
            ++completedCount;
            it = m_pendingUploads.erase(it);
        }

        for (const auto& fence : completedFences)
        {
            ReleasePendingFence(fence);
        }

        return completedCount;
    }

    bool GPUUploadService::IsUploadPending(uint64 uploadId) const
    {
        return std::any_of(m_pendingUploads.begin(), m_pendingUploads.end(),
                           [uploadId](const PendingUpload& upload)
                           {
                               return upload.id == uploadId;
                           });
    }

    bool GPUUploadService::IsUploadComplete(uint64 uploadId) const
    {
        if (uploadId == 0)
            return true;

        return m_completedUploads.find(uploadId) != m_completedUploads.end();
    }

    void GPUUploadService::ForgetCompletedUpload(uint64 uploadId)
    {
        if (uploadId == 0)
            return;

        m_completedUploads.erase(uploadId);
    }

    void GPUUploadService::AbandonUpload(uint64 uploadId)
    {
        if (uploadId == 0)
            return;

        m_completedUploads.erase(uploadId);
        const bool isPending = std::any_of(m_pendingUploads.begin(), m_pendingUploads.end(),
                                           [uploadId](const PendingUpload& upload)
                                           {
                                               return upload.id == uploadId;
                                           });
        if (isPending)
        {
            m_abandonedUploads.insert(uploadId);
        }
        else
        {
            m_abandonedUploads.erase(uploadId);
        }
    }

    GPUUploadBufferResult GPUUploadService::UploadBufferDataWithResult(const GPUUploadBufferDesc& desc, const void* data, uint64 dataSize)
    {
        if (!m_device)
        {
            return MakeBufferFailure(GPUUploadFailureReason::InvalidDevice);
        }

        if (desc.size == 0 || dataSize > desc.size)
        {
            return MakeBufferFailure(GPUUploadFailureReason::InvalidDescription);
        }

        if (!data || dataSize == 0)
        {
            return MakeBufferFailure(GPUUploadFailureReason::InvalidData);
        }

        auto stagedResult = TryUploadBufferStaged(desc, data, dataSize);
        if (stagedResult.succeeded)
        {
            return stagedResult;
        }

        if (stagedResult.failureReason != GPUUploadFailureReason::Unsupported)
        {
            return stagedResult;
        }

        return UploadBufferImmediate(desc, data, dataSize);
    }

    GPUUploadTextureResult GPUUploadService::UploadTextureDataWithResult(const GPUUploadTextureDesc& desc, const void* data)
    {
        if (!m_device)
        {
            return MakeTextureFailure(GPUUploadFailureReason::InvalidDevice);
        }

        if (desc.textureDesc.width == 0 || desc.textureDesc.height == 0 ||
            desc.textureDesc.depth == 0 || desc.textureDesc.mipLevels == 0 ||
            desc.textureDesc.arraySize == 0)
        {
            return MakeTextureFailure(GPUUploadFailureReason::InvalidDescription);
        }

        if (!data || desc.dataSize == 0)
        {
            return MakeTextureFailure(GPUUploadFailureReason::InvalidData);
        }

        if (!IsSupportedTextureUploadFormat(desc.textureDesc.format))
        {
            return MakeTextureFailure(GPUUploadFailureReason::Unsupported);
        }

        auto stagedResult = TryUploadTextureStaged(desc, data);
        if (stagedResult.succeeded)
        {
            return stagedResult;
        }

        if (stagedResult.failureReason != GPUUploadFailureReason::Unsupported)
        {
            return stagedResult;
        }

        return MakeTextureFailure(GPUUploadFailureReason::Unsupported);
    }

    GPUUploadBufferResult GPUUploadService::TryUploadBufferStaged(const GPUUploadBufferDesc& desc, const void* data, uint64 dataSize)
    {
        auto commandContext = GetOrCreateBatchCommandContext();
        if (!commandContext)
        {
            GPUUploadBufferResult result;
            result.failureReason = GPUUploadFailureReason::Unsupported;
            return result;
        }

        RHIStagingBufferDesc stagingDesc;
        stagingDesc.size = dataSize;
        stagingDesc.debugName = desc.debugName;

        auto stagingBuffer = m_device->CreateStagingBuffer(stagingDesc);
        if (!stagingBuffer)
        {
            GPUUploadBufferResult result;
            result.failureReason = GPUUploadFailureReason::Unsupported;
            return result;
        }

        RHIBufferDesc bufferDesc;
        bufferDesc.size = desc.size;
        bufferDesc.usage = desc.usage | RHIBufferUsage::CopyDst;
        bufferDesc.memoryType = RHIMemoryType::Default;
        bufferDesc.stride = desc.stride;
        bufferDesc.debugName = desc.debugName;

        auto buffer = m_device->CreateBuffer(bufferDesc);
        if (!buffer)
        {
            return MakeBufferFailure(GPUUploadFailureReason::CreateResourceFailed);
        }

        void* mapped = stagingBuffer->Map(0, dataSize);
        if (!mapped)
        {
            GPUUploadBufferResult result;
            result.failureReason = GPUUploadFailureReason::Unsupported;
            return result;
        }

        std::memcpy(mapped, data, static_cast<size_t>(dataSize));
        stagingBuffer->Unmap();

        commandContext->CopyBuffer(stagingBuffer->GetBuffer(), buffer.Get(), 0, 0, dataSize);
        commandContext->BufferBarrier(buffer.Get(), RHIResourceState::CopyDest, RHIResourceState::Common);
        m_batchCommandContextDirty = true;

        m_stats.bufferUploadCount++;
        m_stats.stagedUploadCount++;
        m_stats.uploadedBytes += dataSize;

        GPUUploadBufferResult result;
        result.resource = buffer;
        result.succeeded = true;
        result.mode = GPUUploadMode::StagedCopy;
        result.bytesUploaded = dataSize;
        result.uploadId = TrackPendingUpload(stagingBuffer, commandContext, nullptr, 0, dataSize);
        result.isPending = true;

        return result;
    }

    GPUUploadBufferResult GPUUploadService::UploadBufferImmediate(const GPUUploadBufferDesc& desc, const void* data, uint64 dataSize)
    {
        RHIBufferDesc bufferDesc;
        bufferDesc.size = desc.size;
        bufferDesc.usage = desc.usage;
        bufferDesc.memoryType = RHIMemoryType::Upload;
        bufferDesc.stride = desc.stride;
        bufferDesc.debugName = desc.debugName;

        auto buffer = m_device->CreateBuffer(bufferDesc);
        if (!buffer)
        {
            return MakeBufferFailure(GPUUploadFailureReason::CreateResourceFailed);
        }

        void* mapped = buffer->Map();
        if (!mapped)
        {
            return MakeBufferFailure(GPUUploadFailureReason::MapFailed);
        }

        std::memcpy(mapped, data, static_cast<size_t>(dataSize));
        buffer->Unmap();

        m_stats.bufferUploadCount++;
        m_stats.immediateUploadCount++;
        m_stats.uploadedBytes += dataSize;

        GPUUploadBufferResult result;
        result.resource = buffer;
        result.succeeded = true;
        result.mode = GPUUploadMode::ImmediateMapped;
        result.bytesUploaded = dataSize;
        return result;
    }

    GPUUploadTextureResult GPUUploadService::TryUploadTextureStaged(const GPUUploadTextureDesc& desc, const void* data)
    {
        const uint32 bytesPerPixel = GetFormatBytesPerPixel(desc.textureDesc.format);
        if (desc.textureDesc.dimension != RHITextureDimension::Texture2D ||
            desc.textureDesc.mipLevels != 1 || desc.textureDesc.arraySize != 1 ||
            bytesPerPixel == 0)
        {
            GPUUploadTextureResult result;
            result.failureReason = GPUUploadFailureReason::Unsupported;
            return result;
        }

        const uint64 sourceRowPitch = static_cast<uint64>(desc.textureDesc.width) * bytesPerPixel;
        const uint64 uploadRowPitch = AlignUp(sourceRowPitch, RVX_TEXTURE_UPLOAD_ROW_PITCH_ALIGNMENT);
        const uint64 sourceSize = sourceRowPitch * desc.textureDesc.height;
        const uint64 uploadSize = uploadRowPitch * desc.textureDesc.height;
        if (desc.dataSize < sourceSize)
        {
            GPUUploadTextureResult result;
            result.failureReason = GPUUploadFailureReason::Unsupported;
            return result;
        }

        auto commandContext = GetOrCreateBatchCommandContext();
        if (!commandContext)
        {
            GPUUploadTextureResult result;
            result.failureReason = GPUUploadFailureReason::Unsupported;
            return result;
        }

        RHIStagingBufferDesc stagingDesc;
        stagingDesc.size = uploadSize;
        stagingDesc.debugName = desc.textureDesc.debugName;

        auto stagingBuffer = m_device->CreateStagingBuffer(stagingDesc);
        if (!stagingBuffer)
        {
            GPUUploadTextureResult result;
            result.failureReason = GPUUploadFailureReason::Unsupported;
            return result;
        }

        auto texture = m_device->CreateTexture(desc.textureDesc);
        if (!texture)
        {
            return MakeTextureFailure(GPUUploadFailureReason::CreateResourceFailed);
        }

        void* mapped = stagingBuffer->Map(0, uploadSize);
        if (!mapped)
        {
            GPUUploadTextureResult result;
            result.failureReason = GPUUploadFailureReason::Unsupported;
            return result;
        }

        auto* dstRows = static_cast<uint8*>(mapped);
        const auto* srcRows = static_cast<const uint8*>(data);
        for (uint32 row = 0; row < desc.textureDesc.height; ++row)
        {
            std::memcpy(dstRows + row * uploadRowPitch,
                        srcRows + row * sourceRowPitch,
                        static_cast<size_t>(sourceRowPitch));
        }
        stagingBuffer->Unmap();

        RHIBufferTextureCopyDesc copyDesc;
        copyDesc.bufferOffset = 0;
        copyDesc.bufferRowPitch = static_cast<uint32>(uploadRowPitch);
        copyDesc.bufferImageHeight = desc.textureDesc.height;
        copyDesc.textureSubresource = 0;
        copyDesc.textureRegion = {0, 0, desc.textureDesc.width, desc.textureDesc.height};
        copyDesc.textureDepthSlice = 0;

        commandContext->CopyBufferToTexture(stagingBuffer->GetBuffer(), texture.Get(), copyDesc);
        commandContext->TextureBarrier(texture.Get(), RHIResourceState::CopyDest, RHIResourceState::Common);
        m_batchCommandContextDirty = true;

        m_stats.textureUploadCount++;
        m_stats.stagedUploadCount++;
        m_stats.uploadedBytes += sourceSize;

        GPUUploadTextureResult result;
        result.resource = texture;
        result.succeeded = true;
        result.mode = GPUUploadMode::StagedCopy;
        result.bytesUploaded = sourceSize;
        result.uploadId = TrackPendingUpload(stagingBuffer, commandContext, nullptr, 0, uploadSize);
        result.isPending = true;

        return result;
    }

    RHICommandContextRef GPUUploadService::GetOrCreateBatchCommandContext()
    {
        if (m_batchCommandContext)
            return m_batchCommandContext;

        m_batchCommandContext = m_device->CreateCommandContext(RHICommandQueueType::Copy);
        if (m_batchCommandContext)
        {
            m_batchCommandContext->Begin();
        }

        return m_batchCommandContext;
    }

    RHIFenceRef GPUUploadService::AcquireFence()
    {
        if (!m_availableFences.empty())
        {
            RHIFenceRef fence = m_availableFences.back();
            m_availableFences.pop_back();
            return fence;
        }

        return m_device ? m_device->CreateFence(0) : nullptr;
    }

    void GPUUploadService::RetainPendingFence(RHIFence* fence)
    {
        if (!fence)
            return;

        m_pendingFenceUseCounts[fence]++;
    }

    void GPUUploadService::ReleasePendingFence(const RHIFenceRef& fence)
    {
        if (!fence)
            return;

        RHIFence* rawFence = fence.Get();
        auto useCountIt = m_pendingFenceUseCounts.find(rawFence);
        if (useCountIt == m_pendingFenceUseCounts.end())
            return;

        if (--useCountIt->second > 0)
            return;

        m_pendingFenceUseCounts.erase(useCountIt);

        const bool alreadyAvailable = std::any_of(m_availableFences.begin(), m_availableFences.end(),
                                                  [rawFence](const RHIFenceRef& availableFence)
                                                  {
                                                      return availableFence.Get() == rawFence;
                                                  });
        if (!alreadyAvailable)
        {
            m_availableFences.push_back(fence);
        }
    }

    uint64 GPUUploadService::TrackPendingUpload(RHIStagingBufferRef stagingBuffer, RHICommandContextRef commandContext,
                                                RHIFenceRef fence, uint64 fenceValue, uint64 stagingBytes)
    {
        PendingUpload upload;
        upload.id = m_nextUploadId++;
        upload.fenceValue = fenceValue;
        upload.stagingBytes = stagingBytes;
        upload.stagingBuffer = std::move(stagingBuffer);
        upload.commandContext = std::move(commandContext);
        upload.fence = std::move(fence);

        m_pendingUploads.push_back(std::move(upload));
        m_stats.pendingUploadCount++;
        m_stats.stagingBytesInFlight += stagingBytes;
        m_stats.peakStagingBytesInFlight = std::max(m_stats.peakStagingBytesInFlight, m_stats.stagingBytesInFlight);
        return m_pendingUploads.back().id;
    }

    uint32 GPUUploadService::CompleteBatchUploadsWithoutFence(RHICommandContext* commandContext)
    {
        uint32 completedCount = 0;

        auto it = m_pendingUploads.begin();
        while (it != m_pendingUploads.end())
        {
            if (it->commandContext.Get() != commandContext || it->fence)
            {
                ++it;
                continue;
            }

            const bool isAbandoned = m_abandonedUploads.erase(it->id) > 0;
            if (!isAbandoned)
            {
                m_completedUploads.insert(it->id);
            }
            m_stats.completedUploadCount++;
            m_stats.pendingUploadCount--;
            m_stats.stagingBytesInFlight -= it->stagingBytes;
            ++completedCount;
            it = m_pendingUploads.erase(it);
        }

        return completedCount;
    }

    GPUUploadBufferResult GPUUploadService::MakeBufferFailure(GPUUploadFailureReason reason)
    {
        RecordFailure(reason);

        GPUUploadBufferResult result;
        result.failureReason = reason;
        return result;
    }

    GPUUploadTextureResult GPUUploadService::MakeTextureFailure(GPUUploadFailureReason reason)
    {
        RecordFailure(reason);

        GPUUploadTextureResult result;
        result.failureReason = reason;
        return result;
    }

    void GPUUploadService::RecordFailure(GPUUploadFailureReason reason)
    {
        m_stats.failedUploadCount++;
        RVX_CORE_WARN("GPUUploadService: upload request failed with reason {}", static_cast<uint32>(reason));
    }

} // namespace RVX
