#include "Resources/RenderUploadProcessor.h"

#include "Core/Assert.h"
#include "Core/Log.h"
#include "RHI/RHIDevice.h"
#include "Runtime/RenderResourceStatusTable.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>
#include <unordered_set>

namespace RVX
{
namespace
{
    constexpr uint64 AlignUp(uint64 value, uint64 alignment)
    {
        return (value + alignment - 1U) & ~(alignment - 1U);
    }

    bool IsCompletionSatisfied(GPUCompletionStatus status)
    {
        return status == GPUCompletionStatus::Completed ||
               status == GPUCompletionStatus::CompatibilityWaitIdle;
    }

    bool IsRequestHeaderValid(const ResourceUploadRequest& request)
    {
        return request.GetSchemaId() == RVX_RESOURCE_UPLOAD_REQUEST_SCHEMA_ID &&
               request.GetSchemaVersion() ==
                   RVX_RESOURCE_UPLOAD_REQUEST_SCHEMA_VERSION &&
               request.GetSequence() != 0 && request.GetAssetId().IsValid() &&
               request.GetHandle().IsValid() &&
               request.GetKind() != RenderResourceKind::Invalid;
    }

    struct PreparedTextureSlice
    {
        uint32 subresource = 0;
        uint32 width = 0;
        uint32 height = 0;
        uint32 depthSlice = 0;
        uint32 rowCount = 0;
        uint64 sourceOffset = 0;
        uint64 sourceRowPitch = 0;
        uint64 sourceTightRowPitch = 0;
        uint64 uploadOffset = 0;
        uint64 uploadRowPitch = 0;
        bool expandRGB8 = false;
    };
} // namespace

    struct RenderUploadProcessor::LegacyUploadState
    {
        struct PendingUpload
        {
            uint64 id = 0;
            uint64 stagingBytes = 0;
            GPUCompletionPoint completion;
            RHIStagingBufferRef stagingBuffer;
            RHICommandContextRef commandContext;
            bool submissionFailed = false;
        };

        struct TextureSubresourceUploadLayout
        {
            uint32 subresource = 0;
            uint32 width = 0;
            uint32 height = 0;
            uint32 rowCount = 0;
            uint64 sourceOffset = 0;
            uint64 sourceRowPitch = 0;
            uint64 sourceSize = 0;
            uint64 uploadOffset = 0;
            uint64 uploadRowPitch = 0;
            uint64 uploadSize = 0;
        };

        struct TextureFormatUploadBlock
        {
            uint32 width = 1;
            uint32 height = 1;
            uint32 bytes = 0;
        };

        static TextureFormatUploadBlock GetTextureFormatUploadBlock(
            RHIFormat format)
        {
            const uint32 bytes = GetFormatBytesPerPixel(format);
            if (bytes == 0)
            {
                return {};
            }
            return IsCompressedFormat(format)
                       ? TextureFormatUploadBlock{4, 4, bytes}
                       : TextureFormatUploadBlock{1, 1, bytes};
        }

        static bool IsSupportedTextureUploadFormat(RHIFormat format)
        {
            return format != RHIFormat::Unknown && !IsDepthFormat(format) &&
                   GetTextureFormatUploadBlock(format).bytes != 0;
        }

        RHICommandContextRef GetOrCreateBatchCommandContext()
        {
            if (batchCommandContext)
            {
                return batchCommandContext;
            }
            batchCommandContext =
                device->CreateCommandContext(RHICommandQueueType::Copy);
            if (batchCommandContext &&
                batchCommandContext->GetQueueType() ==
                    RHICommandQueueType::Copy)
            {
                batchCommandContext->Begin();
            }
            else
            {
                batchCommandContext.Reset();
            }
            return batchCommandContext;
        }

        uint64 TrackPending(RHIStagingBufferRef staging,
                            RHICommandContextRef context,
                            uint64 stagingBytes)
        {
            PendingUpload upload;
            upload.id = nextUploadId++;
            upload.stagingBytes = stagingBytes;
            upload.stagingBuffer = std::move(staging);
            upload.commandContext = std::move(context);
            pendingUploads.push_back(std::move(upload));
            ++stats.pendingUploadCount;
            stats.stagingBytesInFlight += stagingBytes;
            stats.peakStagingBytesInFlight =
                std::max(stats.peakStagingBytesInFlight,
                         stats.stagingBytesInFlight);
            return pendingUploads.back().id;
        }

        GPUUploadBufferResult MakeBufferFailure(
            GPUUploadFailureReason reason)
        {
            ++stats.failedUploadCount;
            RVX_CORE_WARN(
                "RenderUploadProcessor legacy buffer upload failed: {}",
                static_cast<uint32>(reason));
            GPUUploadBufferResult result;
            result.failureReason = reason;
            return result;
        }

        GPUUploadTextureResult MakeTextureFailure(
            GPUUploadFailureReason reason)
        {
            ++stats.failedUploadCount;
            RVX_CORE_WARN(
                "RenderUploadProcessor legacy texture upload failed: {}",
                static_cast<uint32>(reason));
            GPUUploadTextureResult result;
            result.failureReason = reason;
            return result;
        }

        GPUUploadBufferResult UploadBufferImmediate(
            const GPUUploadBufferDesc& desc,
            const void* data,
            uint64 dataSize)
        {
            RHIBufferDesc bufferDesc;
            bufferDesc.size = desc.size;
            bufferDesc.usage = desc.usage;
            bufferDesc.memoryType = RHIMemoryType::Upload;
            bufferDesc.stride = desc.stride;
            bufferDesc.debugName = desc.debugName;
            RHIBufferRef buffer = device->CreateBuffer(bufferDesc);
            if (!buffer)
            {
                return MakeBufferFailure(
                    GPUUploadFailureReason::CreateResourceFailed);
            }
            void* mapped = buffer->Map();
            if (mapped == nullptr)
            {
                return MakeBufferFailure(GPUUploadFailureReason::MapFailed);
            }
            std::memcpy(mapped, data, static_cast<size_t>(dataSize));
            buffer->Unmap();
            ++stats.bufferUploadCount;
            ++stats.immediateUploadCount;
            stats.uploadedBytes += dataSize;

            GPUUploadBufferResult result;
            result.resource = std::move(buffer);
            result.succeeded = true;
            result.mode = GPUUploadMode::ImmediateMapped;
            result.bytesUploaded = dataSize;
            return result;
        }

        GPUUploadBufferResult TryUploadBufferStaged(
            const GPUUploadBufferDesc& desc,
            const void* data,
            uint64 dataSize)
        {
            if (device->GetBackendType() == RHIBackendType::DX11)
            {
                GPUUploadBufferResult result;
                result.failureReason = GPUUploadFailureReason::Unsupported;
                return result;
            }
            RHICommandContextRef context = GetOrCreateBatchCommandContext();
            if (!context)
            {
                GPUUploadBufferResult result;
                result.failureReason = GPUUploadFailureReason::Unsupported;
                return result;
            }
            RHIStagingBufferDesc stagingDesc;
            stagingDesc.size = dataSize;
            stagingDesc.debugName = desc.debugName;
            RHIStagingBufferRef staging =
                device->CreateStagingBuffer(stagingDesc);
            if (!staging)
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
            RHIBufferRef buffer = device->CreateBuffer(bufferDesc);
            if (!buffer)
            {
                return MakeBufferFailure(
                    GPUUploadFailureReason::CreateResourceFailed);
            }
            void* mapped = staging->Map(0, dataSize);
            if (mapped == nullptr)
            {
                GPUUploadBufferResult result;
                result.failureReason = GPUUploadFailureReason::Unsupported;
                return result;
            }
            std::memcpy(mapped, data, static_cast<size_t>(dataSize));
            staging->Unmap();
            context->CopyBuffer(staging->GetBuffer(),
                                buffer.Get(),
                                0,
                                0,
                                dataSize);
            context->BufferBarrier(buffer.Get(),
                                   RHIResourceState::CopyDest,
                                   RHIResourceState::Common);
            batchCommandContextDirty = true;
            ++stats.bufferUploadCount;
            ++stats.stagedUploadCount;
            stats.uploadedBytes += dataSize;

            GPUUploadBufferResult result;
            result.resource = std::move(buffer);
            result.succeeded = true;
            result.mode = GPUUploadMode::StagedCopy;
            result.bytesUploaded = dataSize;
            result.uploadId = TrackPending(std::move(staging),
                                           context,
                                           dataSize);
            result.isPending = true;
            return result;
        }

        GPUUploadBufferResult UploadBufferWithResult(
            const GPUUploadBufferDesc& desc,
            const void* data,
            uint64 dataSize)
        {
            if (device == nullptr)
            {
                return MakeBufferFailure(GPUUploadFailureReason::InvalidDevice);
            }
            if (desc.size == 0 || dataSize > desc.size)
            {
                return MakeBufferFailure(
                    GPUUploadFailureReason::InvalidDescription);
            }
            if (data == nullptr || dataSize == 0)
            {
                return MakeBufferFailure(GPUUploadFailureReason::InvalidData);
            }
            GPUUploadBufferResult staged =
                TryUploadBufferStaged(desc, data, dataSize);
            if (staged.succeeded ||
                staged.failureReason != GPUUploadFailureReason::Unsupported)
            {
                return staged;
            }
            return UploadBufferImmediate(desc, data, dataSize);
        }

        GPUUploadTextureResult TryUploadTextureStaged(
            const GPUUploadTextureDesc& desc,
            const void* data)
        {
            const bool compressed =
                IsCompressedFormat(desc.textureDesc.format);
            const bool tightRows =
                compressed && device->GetBackendType() == RHIBackendType::OpenGL;
            const TextureFormatUploadBlock block =
                GetTextureFormatUploadBlock(desc.textureDesc.format);
            if ((desc.textureDesc.dimension !=
                     RHITextureDimension::Texture2D &&
                 desc.textureDesc.dimension !=
                     RHITextureDimension::TextureCube) ||
                block.bytes == 0)
            {
                GPUUploadTextureResult result;
                result.failureReason = GPUUploadFailureReason::Unsupported;
                return result;
            }

            const uint32 layerCount =
                GetTexturePhysicalLayerCount(desc.textureDesc);
            const uint32 mipLevels =
                std::max(1U, desc.textureDesc.mipLevels);
            std::vector<TextureSubresourceUploadLayout> layouts;
            layouts.reserve(layerCount * mipLevels);
            uint64 sourceSize = 0;
            uint64 uploadSize = 0;
            for (uint32 layer = 0; layer < layerCount; ++layer)
            {
                for (uint32 mip = 0; mip < mipLevels; ++mip)
                {
                    const uint32 width =
                        std::max(1U, desc.textureDesc.width >> mip);
                    const uint32 height =
                        std::max(1U, desc.textureDesc.height >> mip);
                    const uint32 rowBlocks =
                        (width + block.width - 1U) / block.width;
                    const uint32 rows =
                        (height + block.height - 1U) / block.height;
                    const uint64 sourceRowPitch =
                        static_cast<uint64>(rowBlocks) * block.bytes;
                    const uint64 uploadRowPitch =
                        tightRows ? sourceRowPitch
                                  : AlignUp(sourceRowPitch, 256);
                    const uint64 sourceBytes = sourceRowPitch * rows;
                    const uint64 uploadBytes = uploadRowPitch * rows;
                    const uint64 uploadOffset =
                        tightRows ? uploadSize : AlignUp(uploadSize, 512);
                    if (sourceBytes >
                            std::numeric_limits<uint64>::max() - sourceSize ||
                        uploadBytes >
                            std::numeric_limits<uint64>::max() - uploadOffset)
                    {
                        GPUUploadTextureResult result;
                        result.failureReason =
                            GPUUploadFailureReason::Unsupported;
                        return result;
                    }
                    layouts.push_back(TextureSubresourceUploadLayout{
                        EncodeTextureSubresource(mip, layer, mipLevels),
                        width,
                        height,
                        rows,
                        sourceSize,
                        sourceRowPitch,
                        sourceBytes,
                        uploadOffset,
                        uploadRowPitch,
                        uploadBytes});
                    sourceSize += sourceBytes;
                    uploadSize = uploadOffset + uploadBytes;
                }
            }
            if (desc.dataSize < sourceSize)
            {
                GPUUploadTextureResult result;
                result.failureReason = GPUUploadFailureReason::Unsupported;
                return result;
            }

            RHICommandContextRef context = GetOrCreateBatchCommandContext();
            if (!context)
            {
                GPUUploadTextureResult result;
                result.failureReason = GPUUploadFailureReason::Unsupported;
                return result;
            }
            RHIStagingBufferDesc stagingDesc;
            stagingDesc.size = uploadSize;
            stagingDesc.debugName = desc.textureDesc.debugName;
            RHIStagingBufferRef staging =
                device->CreateStagingBuffer(stagingDesc);
            if (!staging)
            {
                GPUUploadTextureResult result;
                result.failureReason = GPUUploadFailureReason::Unsupported;
                return result;
            }
            RHITextureDesc textureDesc = desc.textureDesc;
            textureDesc.usage =
                textureDesc.usage | RHITextureUsage::CopyDst;
            RHITextureRef texture = device->CreateTexture(textureDesc);
            if (!texture)
            {
                return MakeTextureFailure(
                    GPUUploadFailureReason::CreateResourceFailed);
            }
            void* mapped = staging->Map(0, uploadSize);
            if (mapped == nullptr)
            {
                GPUUploadTextureResult result;
                result.failureReason = GPUUploadFailureReason::Unsupported;
                return result;
            }
            auto* destination = static_cast<uint8*>(mapped);
            const auto* source = static_cast<const uint8*>(data);
            for (const TextureSubresourceUploadLayout& layout : layouts)
            {
                for (uint32 row = 0; row < layout.rowCount; ++row)
                {
                    std::memcpy(
                        destination + layout.uploadOffset +
                            row * layout.uploadRowPitch,
                        source + layout.sourceOffset +
                            row * layout.sourceRowPitch,
                        static_cast<size_t>(layout.sourceRowPitch));
                }
            }
            staging->Unmap();
            for (const TextureSubresourceUploadLayout& layout : layouts)
            {
                RHIBufferTextureCopyDesc copy;
                copy.bufferOffset = layout.uploadOffset;
                copy.bufferRowPitch =
                    static_cast<uint32>(layout.uploadRowPitch);
                copy.bufferImageHeight = layout.rowCount;
                copy.textureSubresource = layout.subresource;
                copy.textureRegion =
                    {0, 0, layout.width, layout.height};
                context->CopyBufferToTexture(staging->GetBuffer(),
                                             texture.Get(),
                                             copy);
            }
            context->TextureBarrier(texture.Get(),
                                    RHIResourceState::CopyDest,
                                    RHIResourceState::Common);
            batchCommandContextDirty = true;
            ++stats.textureUploadCount;
            ++stats.stagedUploadCount;
            stats.uploadedBytes += sourceSize;

            GPUUploadTextureResult result;
            result.resource = std::move(texture);
            result.succeeded = true;
            result.mode = GPUUploadMode::StagedCopy;
            result.bytesUploaded = sourceSize;
            result.uploadId = TrackPending(std::move(staging),
                                           context,
                                           uploadSize);
            result.isPending = true;
            return result;
        }

        GPUUploadTextureResult UploadTextureWithResult(
            const GPUUploadTextureDesc& desc,
            const void* data)
        {
            if (device == nullptr)
            {
                return MakeTextureFailure(GPUUploadFailureReason::InvalidDevice);
            }
            if (desc.textureDesc.width == 0 ||
                desc.textureDesc.height == 0 ||
                desc.textureDesc.depth == 0 ||
                desc.textureDesc.mipLevels == 0 ||
                desc.textureDesc.arraySize == 0)
            {
                return MakeTextureFailure(
                    GPUUploadFailureReason::InvalidDescription);
            }
            if (data == nullptr || desc.dataSize == 0)
            {
                return MakeTextureFailure(GPUUploadFailureReason::InvalidData);
            }
            if (!IsSupportedTextureUploadFormat(desc.textureDesc.format))
            {
                return MakeTextureFailure(GPUUploadFailureReason::Unsupported);
            }
            GPUUploadTextureResult result =
                TryUploadTextureStaged(desc, data);
            if (result.succeeded ||
                result.failureReason != GPUUploadFailureReason::Unsupported)
            {
                return result;
            }
            return MakeTextureFailure(GPUUploadFailureReason::Unsupported);
        }

        void FlushBatch()
        {
            if (device == nullptr || tracker == nullptr ||
                !batchCommandContext || !batchCommandContextDirty)
            {
                return;
            }
            RHICommandContextRef submitted = batchCommandContext;
            submitted->End();
            const GPUCompletionPoint point = tracker->Submit(submitted.Get());
            for (PendingUpload& upload : pendingUploads)
            {
                if (upload.commandContext.Get() != submitted.Get() ||
                    upload.completion.value != 0)
                {
                    continue;
                }
                if (point.value == 0)
                {
                    upload.submissionFailed = true;
                }
                else
                {
                    upload.completion = point;
                }
            }
            batchCommandContext.Reset();
            batchCommandContextDirty = false;
        }

        uint32 ProcessCompleted()
        {
            uint32 completedCount = 0;
            auto upload = pendingUploads.begin();
            while (upload != pendingUploads.end())
            {
                if (upload->completion.value == 0 &&
                    !upload->submissionFailed)
                {
                    ++upload;
                    continue;
                }
                const GPUCompletionStatus completionStatus =
                    upload->submissionFailed
                        ? GPUCompletionStatus::Lost
                        : tracker->Query(upload->completion);
                if (completionStatus == GPUCompletionStatus::Pending)
                {
                    ++upload;
                    continue;
                }
                const bool abandoned = abandonedUploads.erase(upload->id) != 0;
                if (!abandoned)
                {
                    completedUploads.insert(upload->id);
                }
                if (completionStatus == GPUCompletionStatus::Lost)
                {
                    ++stats.failedUploadCount;
                }
                ++stats.completedUploadCount;
                --stats.pendingUploadCount;
                stats.stagingBytesInFlight -= upload->stagingBytes;
                ++completedCount;
                upload = pendingUploads.erase(upload);
            }
            return completedCount;
        }

        uint32 FlushAndWait()
        {
            FlushBatch();
            for (PendingUpload& upload : pendingUploads)
            {
                if (upload.completion.value != 0)
                {
                    static_cast<void>(tracker->Wait(upload.completion));
                }
            }
            return ProcessCompleted();
        }

        IRHIDevice* device = nullptr;
        RenderSubmissionTracker* tracker = nullptr;
        GPUUploadStats stats;
        std::vector<PendingUpload> pendingUploads;
        std::unordered_set<uint64> completedUploads;
        std::unordered_set<uint64> abandonedUploads;
        RHICommandContextRef batchCommandContext;
        bool batchCommandContextDirty = false;
        uint64 nextUploadId = 1;
    };

    RenderUploadProcessor::RenderUploadProcessor() = default;

    RenderUploadProcessor::~RenderUploadProcessor()
    {
        Shutdown();
        ShutdownLegacy();
    }

    bool RenderUploadProcessor::InitializeLegacy(
        IRHIDevice* device,
        RenderSubmissionTracker* submissionTracker)
    {
        ShutdownLegacy();
        if (device == nullptr || submissionTracker == nullptr ||
            submissionTracker->GetTopology().completionMode ==
                RHIQueueCompletionMode::None)
        {
            return false;
        }
        m_legacy = std::make_unique<LegacyUploadState>();
        m_legacy->device = device;
        m_legacy->tracker = submissionTracker;
        return true;
    }

    void RenderUploadProcessor::ShutdownLegacy()
    {
        if (!m_legacy)
        {
            return;
        }
        m_legacy->FlushAndWait();
        m_legacy.reset();
    }

    bool RenderUploadProcessor::IsLegacyInitialized() const
    {
        return m_legacy != nullptr && m_legacy->device != nullptr;
    }

    RHIBufferRef RenderUploadProcessor::UploadLegacyBufferData(
        const GPUUploadBufferDesc& desc,
        const void* data,
        uint64 dataSize)
    {
        return UploadLegacyBufferDataWithResult(desc, data, dataSize).resource;
    }

    RHITextureRef RenderUploadProcessor::UploadLegacyTextureData(
        const GPUUploadTextureDesc& desc,
        const void* data)
    {
        return UploadLegacyTextureDataWithResult(desc, data).resource;
    }

    GPUUploadBufferResult
        RenderUploadProcessor::UploadLegacyBufferDataWithResult(
            const GPUUploadBufferDesc& desc,
            const void* data,
            uint64 dataSize)
    {
        return m_legacy
                   ? m_legacy->UploadBufferWithResult(desc, data, dataSize)
                   : GPUUploadBufferResult{};
    }

    GPUUploadTextureResult
        RenderUploadProcessor::UploadLegacyTextureDataWithResult(
            const GPUUploadTextureDesc& desc,
            const void* data)
    {
        return m_legacy
                   ? m_legacy->UploadTextureWithResult(desc, data)
                   : GPUUploadTextureResult{};
    }

    void RenderUploadProcessor::FlushLegacyBatchUploads()
    {
        if (m_legacy)
        {
            m_legacy->FlushBatch();
        }
    }

    uint32 RenderUploadProcessor::FlushAndWaitForLegacyUploads()
    {
        return m_legacy ? m_legacy->FlushAndWait() : 0;
    }

    uint32 RenderUploadProcessor::ProcessCompletedLegacyUploads()
    {
        return m_legacy ? m_legacy->ProcessCompleted() : 0;
    }

    bool RenderUploadProcessor::IsLegacyUploadPending(uint64 uploadId) const
    {
        return m_legacy && std::any_of(
            m_legacy->pendingUploads.begin(),
            m_legacy->pendingUploads.end(),
            [uploadId](const LegacyUploadState::PendingUpload& upload)
            {
                return upload.id == uploadId;
            });
    }

    bool RenderUploadProcessor::IsLegacyUploadComplete(uint64 uploadId) const
    {
        return uploadId == 0 ||
               (m_legacy &&
                m_legacy->completedUploads.find(uploadId) !=
                    m_legacy->completedUploads.end());
    }

    void RenderUploadProcessor::ForgetCompletedLegacyUpload(uint64 uploadId)
    {
        if (m_legacy && uploadId != 0)
        {
            m_legacy->completedUploads.erase(uploadId);
        }
    }

    void RenderUploadProcessor::AbandonLegacyUpload(uint64 uploadId)
    {
        if (!m_legacy || uploadId == 0)
        {
            return;
        }
        m_legacy->completedUploads.erase(uploadId);
        if (IsLegacyUploadPending(uploadId))
        {
            m_legacy->abandonedUploads.insert(uploadId);
        }
        else
        {
            m_legacy->abandonedUploads.erase(uploadId);
        }
    }

    const GPUUploadStats& RenderUploadProcessor::GetLegacyStats() const
    {
        static const GPUUploadStats empty;
        return m_legacy ? m_legacy->stats : empty;
    }

    void RenderUploadProcessor::ResetLegacyStats()
    {
        if (m_legacy)
        {
            m_legacy->stats = {};
        }
    }

    bool RenderUploadProcessor::Initialize(
        IRHIDevice* device,
        RenderResourceStatusTable* statusTable,
        RenderResourceRegistry* registry,
        RenderSubmissionTracker* submissionTracker)
    {
        Shutdown();
        if (device == nullptr || statusTable == nullptr || registry == nullptr ||
            submissionTracker == nullptr)
        {
            return false;
        }
        m_device = device;
        m_statusTable = statusTable;
        m_registry = registry;
        m_submissionTracker = submissionTracker;
        return true;
    }

    void RenderUploadProcessor::Shutdown()
    {
        if (m_submissionTracker != nullptr)
        {
            while (!m_inFlight.empty())
            {
                const GPUCompletionStatus status =
                    m_submissionTracker->Wait(m_inFlight.back().completion);
                CompleteInFlight(m_inFlight.size() - 1U, status);
            }
            while (!m_pendingReleases.empty())
            {
                const GPUCompletionStatus status =
                    m_submissionTracker->Wait(
                        m_pendingReleases.back().completion);
                CompleteRelease(m_pendingReleases.size() - 1U, status);
            }
        }
        m_inFlight.clear();
        m_pendingReleases.clear();
        m_stats = {};
        m_submissionTracker = nullptr;
        m_registry = nullptr;
        m_statusTable = nullptr;
        m_device = nullptr;
    }

    void RenderUploadProcessor::ShutdownDeviceLost()
    {
        while (!m_inFlight.empty())
        {
            CompleteInFlight(m_inFlight.size() - 1U,
                             GPUCompletionStatus::Lost);
        }
        while (!m_pendingReleases.empty())
        {
            CompleteRelease(m_pendingReleases.size() - 1U,
                            GPUCompletionStatus::Lost);
        }
        m_stats = {};
        m_submissionTracker = nullptr;
        m_registry = nullptr;
        m_statusTable = nullptr;
        m_device = nullptr;
    }

    RenderUploadProcessCode RenderUploadProcessor::ProcessUpload(
        ResourceUploadRequestRef request)
    {
        if (m_device == nullptr || m_statusTable == nullptr ||
            m_registry == nullptr || m_submissionTracker == nullptr ||
            request == nullptr || !IsRequestHeaderValid(*request))
        {
            request.reset();
            return RenderUploadProcessCode::InvalidRequest;
        }

        const RenderResourceHandle handle = request->GetHandle();
        const RenderResourceStatus queuedStatus = m_statusTable->Query(handle);
        if (queuedStatus.code != RenderResourceStatusCode::Current ||
            queuedStatus.state != RenderResourcePublicState::UploadQueued ||
            !m_statusTable->CompareExchange(
                handle,
                PackedRenderResourceStatus{handle.generation,
                                           RenderResourcePublicState::UploadQueued,
                                           queuedStatus.failure},
                PackedRenderResourceStatus{handle.generation,
                                           RenderResourcePublicState::Uploading,
                                           RenderResourceFailureCode::None},
                RenderStatusWriter::Render))
        {
            request.reset();
            return RenderUploadProcessCode::StaleState;
        }

        if (!ValidateDependencies(*request))
        {
            return FailBeforeSubmission(
                request,
                RenderResourceFailureCode::DependencyUnavailable,
                RenderUploadProcessCode::DependencyUnavailable);
        }
        if (!m_registry->BeginPending(handle,
                                      request->GetKind(),
                                      request->GetDependencies()))
        {
            return FailBeforeSubmission(
                request,
                RenderResourceFailureCode::RuntimeFailure,
                RenderUploadProcessCode::ResourceCreationFailed);
        }

        RHICommandContextRef context =
            m_device->CreateCommandContext(RHICommandQueueType::Copy);
        if (!context || context->GetQueueType() != RHICommandQueueType::Copy)
        {
            return FailBeforeSubmission(
                request,
                RenderResourceFailureCode::ResourceCreationFailed,
                RenderUploadProcessCode::ResourceCreationFailed);
        }

        std::vector<RHIStagingBufferRef> stagingBuffers;
        context->Begin();
        bool built = false;
        switch (request->GetKind())
        {
            case RenderResourceKind::Mesh:
                if (const auto* payload =
                        std::get_if<MeshUploadPayload>(&request->GetPayload()))
                {
                    built = BuildMesh(handle,
                                      *payload,
                                      *context,
                                      stagingBuffers);
                }
                break;
            case RenderResourceKind::Texture:
                if (const auto* payload =
                        std::get_if<TextureUploadPayload>(&request->GetPayload()))
                {
                    built = BuildTexture(handle,
                                         *payload,
                                         *context,
                                         stagingBuffers);
                }
                break;
            case RenderResourceKind::Material:
                if (const auto* payload =
                        std::get_if<MaterialUploadPayload>(&request->GetPayload()))
                {
                    built = BuildMaterial(handle,
                                          *payload,
                                          *context,
                                          stagingBuffers);
                }
                break;
            case RenderResourceKind::Invalid:
            default:
                break;
        }
        if (!built)
        {
            return FailBeforeSubmission(
                request,
                RenderResourceFailureCode::ResourceCreationFailed,
                RenderUploadProcessCode::ResourceCreationFailed);
        }

        context->End();
        const GPUCompletionPoint point =
            m_submissionTracker->Submit(context.Get());
        GPUCompletionToken completion;
        if (point.value == 0 || !InsertGPUCompletionPoint(completion, point) ||
            !m_registry->SetPendingCompletion(handle, completion))
        {
            return FailBeforeSubmission(
                request,
                RenderResourceFailureCode::UploadSubmissionFailed,
                RenderUploadProcessCode::SubmissionFailed);
        }

        InFlightUpload inFlight;
        inFlight.handle = handle;
        inFlight.request = std::move(request);
        inFlight.completion = completion;
        inFlight.commandContext = std::move(context);
        inFlight.stagingBuffers = std::move(stagingBuffers);
        inFlight.retainedBytes = inFlight.request->GetDerivedPayloadBytes();
        m_stats.inFlightBytes += inFlight.retainedBytes;
        ++m_stats.accepted;
        m_inFlight.push_back(std::move(inFlight));
        return RenderUploadProcessCode::Accepted;
    }

    void RenderUploadProcessor::ProcessRelease(RenderResourceHandle handle)
    {
        if (m_statusTable == nullptr || m_registry == nullptr ||
            m_submissionTracker == nullptr || !handle.IsValid())
        {
            return;
        }
        const RenderResourceStatus status = m_statusTable->Query(handle);
        if (status.code != RenderResourceStatusCode::Current ||
            status.state != RenderResourcePublicState::Evicting)
        {
            return;
        }

        for (InFlightUpload& upload : m_inFlight)
        {
            if (upload.handle == handle)
            {
                upload.cancelled = true;
                return;
            }
        }

        if (!m_registry->HasExactEntry(handle))
        {
            static_cast<void>(PublishTerminal(
                handle,
                RenderResourcePublicState::Released,
                RenderResourceFailureCode::Cancelled));
            return;
        }

        const GPUCompletionToken completion = m_registry->GetLastUse(handle);
        GPUCompletionStatus completionStatus =
            m_submissionTracker->Query(completion);
        if (completionStatus == GPUCompletionStatus::Pending &&
            m_submissionTracker->GetTopology().completionMode ==
                RHIQueueCompletionMode::CompatibilityWaitIdle)
        {
            completionStatus = m_submissionTracker->Wait(completion);
            ++m_stats.compatibilityWaits;
        }
        if (completionStatus == GPUCompletionStatus::Pending)
        {
            m_pendingReleases.push_back(PendingRelease{handle, completion});
            return;
        }

        if (m_registry->Release(handle))
        {
            static_cast<void>(PublishTerminal(
                handle,
                RenderResourcePublicState::Released,
                RenderResourceFailureCode::Cancelled));
        }
    }

    GPUCompletionStatus RenderUploadProcessor::PollCompletion()
    {
        if (m_submissionTracker == nullptr)
        {
            return GPUCompletionStatus::Lost;
        }

        GPUCompletionStatus aggregate = GPUCompletionStatus::Completed;
        size_t index = 0;
        while (index < m_inFlight.size())
        {
            GPUCompletionStatus status =
                m_submissionTracker->Query(m_inFlight[index].completion);
            if (status == GPUCompletionStatus::Pending &&
                m_submissionTracker->GetTopology().completionMode ==
                    RHIQueueCompletionMode::CompatibilityWaitIdle)
            {
                status = m_submissionTracker->Wait(
                    m_inFlight[index].completion);
                ++m_stats.compatibilityWaits;
            }
            if (status == GPUCompletionStatus::Pending)
            {
                aggregate = GPUCompletionStatus::Pending;
                ++index;
                continue;
            }
            if (status == GPUCompletionStatus::Lost)
            {
                aggregate = GPUCompletionStatus::Lost;
            }
            else if (status == GPUCompletionStatus::CompatibilityWaitIdle &&
                     aggregate == GPUCompletionStatus::Completed)
            {
                aggregate = status;
            }
            CompleteInFlight(index, status);
        }

        index = 0;
        while (index < m_pendingReleases.size())
        {
            GPUCompletionStatus status = m_submissionTracker->Query(
                m_pendingReleases[index].completion);
            if (status == GPUCompletionStatus::Pending)
            {
                aggregate = GPUCompletionStatus::Pending;
                ++index;
                continue;
            }
            CompleteRelease(index, status);
        }
        return aggregate;
    }

    uint32 RenderUploadProcessor::GetInFlightCount() const
    {
        return static_cast<uint32>(m_inFlight.size());
    }

    bool RenderUploadProcessor::ValidateDependencies(
        const ResourceUploadRequest& request) const
    {
        for (RenderResourceHandle dependency : request.GetDependencies())
        {
            if (!m_registry->IsGPUReadyExact(dependency))
            {
                return false;
            }
        }

        const auto* material =
            std::get_if<MaterialUploadPayload>(&request.GetPayload());
        if (material == nullptr)
        {
            return true;
        }
        for (const MaterialUploadTextureBinding& binding :
             material->textureBindings)
        {
            const bool declared = std::find(request.GetDependencies().begin(),
                                            request.GetDependencies().end(),
                                            binding.texture) !=
                                  request.GetDependencies().end();
            if (!declared || m_registry->ResolveTexture(binding.texture) == nullptr)
            {
                return false;
            }
        }
        return true;
    }

    bool RenderUploadProcessor::BuildMesh(
        RenderResourceHandle handle,
        const MeshUploadPayload& payload,
        RHICommandContext& context,
        std::vector<RHIStagingBufferRef>& stagingBuffers)
    {
        if (payload.positionRange.size == 0)
        {
            return false;
        }
        if (!m_registry->SetPendingMeshMetadata(handle,
                                                payload.createInfo,
                                                payload.submeshes))
        {
            return false;
        }
        const struct BufferSpec
        {
            RenderMeshBufferSemantic semantic;
            RHIBufferUsage usage;
            UploadByteRange range;
        } specs[] = {
            {RenderMeshBufferSemantic::Position, RHIBufferUsage::Vertex, payload.positionRange},
            {RenderMeshBufferSemantic::Normal, RHIBufferUsage::Vertex, payload.normalRange},
            {RenderMeshBufferSemantic::UV, RHIBufferUsage::Vertex, payload.uvRange},
            {RenderMeshBufferSemantic::Tangent, RHIBufferUsage::Vertex, payload.tangentRange},
            {RenderMeshBufferSemantic::BoneIndices, RHIBufferUsage::Vertex, payload.boneIndexRange},
            {RenderMeshBufferSemantic::BoneWeights, RHIBufferUsage::Vertex, payload.boneWeightRange},
            {RenderMeshBufferSemantic::Index, RHIBufferUsage::Index, payload.indexRange},
        };
        for (const BufferSpec& spec : specs)
        {
            if (!RecordMeshBuffer(handle,
                                  spec.semantic,
                                  spec.usage,
                                  payload.bytes,
                                  spec.range,
                                  context,
                                  stagingBuffers))
            {
                return false;
            }
        }
        return true;
    }

    bool RenderUploadProcessor::BuildTexture(
        RenderResourceHandle handle,
        const TextureUploadPayload& payload,
        RHICommandContext& context,
        std::vector<RHIStagingBufferRef>& stagingBuffers)
    {
        const RHIFormat format = ToRHIFormat(payload.createInfo);
        if (format == RHIFormat::Unknown || payload.bytes.empty() ||
            payload.subresources.empty())
        {
            return false;
        }

        const bool compressed = IsCompressedFormat(format);
        const bool expandRGB8 =
            payload.createInfo.format == TextureUploadFormat::RGB8;
        const uint32 blockWidth = compressed ? 4U : 1U;
        const uint32 blockHeight = compressed ? 4U : 1U;
        const uint32 sourceBytesPerBlock =
            expandRGB8 ? 3U : GetFormatBytesPerPixel(format);
        if (sourceBytesPerBlock == 0)
        {
            return false;
        }

        std::vector<PreparedTextureSlice> layouts;
        uint64 uploadSize = 0;
        for (const TextureUploadSubresource& subresource :
             payload.subresources)
        {
            if (subresource.mipLevel >= payload.createInfo.mipLevels ||
                subresource.arrayLayer >= payload.createInfo.arrayLayers ||
                subresource.bytes.offset > payload.bytes.size() ||
                subresource.bytes.size >
                    payload.bytes.size() - subresource.bytes.offset)
            {
                return false;
            }

            const uint32 width = subresource.mipLevel >= 32U
                                     ? 1U
                                     : std::max(
                                           1U,
                                           payload.createInfo.width >>
                                               subresource.mipLevel);
            const uint32 height = subresource.mipLevel >= 32U
                                      ? 1U
                                      : std::max(
                                            1U,
                                            payload.createInfo.height >>
                                                subresource.mipLevel);
            const uint32 depth = subresource.mipLevel >= 32U
                                     ? 1U
                                     : std::max(
                                           1U,
                                           payload.createInfo.depth >>
                                               subresource.mipLevel);
            const uint64 rowBlocks =
                (static_cast<uint64>(width) + blockWidth - 1U) /
                blockWidth;
            const uint64 rowCount =
                (static_cast<uint64>(height) + blockHeight - 1U) /
                blockHeight;
            const uint64 sourceTightRowPitch =
                rowBlocks * sourceBytesPerBlock;
            const uint64 destinationTightRowPitch =
                expandRGB8 ? static_cast<uint64>(width) * 4U
                           : sourceTightRowPitch;
            if (rowCount > std::numeric_limits<uint32>::max() ||
                subresource.rowPitch < sourceTightRowPitch ||
                subresource.rowPitch >
                    std::numeric_limits<uint64>::max() / rowCount ||
                subresource.slicePitch <
                    subresource.rowPitch * rowCount ||
                subresource.slicePitch >
                    std::numeric_limits<uint64>::max() / depth ||
                subresource.bytes.size < subresource.slicePitch * depth)
            {
                return false;
            }

            const bool requiresTightRows =
                compressed &&
                m_device->GetBackendType() == RHIBackendType::OpenGL;
            if (!requiresTightRows &&
                destinationTightRowPitch >
                    std::numeric_limits<uint64>::max() - 255U)
            {
                return false;
            }
            const uint64 uploadRowPitch =
                requiresTightRows ? destinationTightRowPitch
                                  : AlignUp(destinationTightRowPitch, 256U);
            if (uploadRowPitch > std::numeric_limits<uint32>::max() ||
                uploadRowPitch >
                    std::numeric_limits<uint64>::max() / rowCount)
            {
                return false;
            }
            const uint64 uploadSlicePitch = uploadRowPitch * rowCount;

            for (uint32 depthSlice = 0; depthSlice < depth; ++depthSlice)
            {
                if (uploadSize >
                    std::numeric_limits<uint64>::max() - 511U)
                {
                    return false;
                }
                const uint64 uploadOffset = AlignUp(uploadSize, 512U);
                if (uploadSlicePitch >
                    std::numeric_limits<uint64>::max() - uploadOffset)
                {
                    return false;
                }
                layouts.push_back(PreparedTextureSlice{
                    EncodeTextureSubresource(
                        subresource.mipLevel,
                        subresource.arrayLayer,
                        payload.createInfo.mipLevels),
                    width,
                    height,
                    depthSlice,
                    static_cast<uint32>(rowCount),
                    subresource.bytes.offset +
                        subresource.slicePitch * depthSlice,
                    subresource.rowPitch,
                    sourceTightRowPitch,
                    uploadOffset,
                    uploadRowPitch,
                    expandRGB8,
                });
                uploadSize = uploadOffset + uploadSlicePitch;
            }
        }
        if (layouts.empty() ||
            uploadSize > std::numeric_limits<size_t>::max())
        {
            return false;
        }

        RHITextureDesc desc;
        desc.width = payload.createInfo.width;
        desc.height = payload.createInfo.height;
        desc.depth = payload.createInfo.depth;
        desc.mipLevels = payload.createInfo.mipLevels;
        desc.arraySize = payload.createInfo.isCubemap
                             ? payload.createInfo.arrayLayers / 6U
                             : payload.createInfo.arrayLayers;
        desc.format = format;
        desc.usage = RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst;
        desc.dimension = payload.createInfo.isCubemap
                             ? RHITextureDimension::TextureCube
                             : (payload.createInfo.depth > 1
                                    ? RHITextureDimension::Texture3D
                                    : RHITextureDimension::Texture2D);
        RHITextureRef texture = m_device->CreateTexture(desc);
        if (!texture ||
            !m_registry->SetPendingTexture(handle,
                                           texture,
                                           payload.bytes.size()))
        {
            return false;
        }

        RHIStagingBufferDesc stagingDesc;
        stagingDesc.size = uploadSize;
        RHIStagingBufferRef staging =
            m_device->CreateStagingBuffer(stagingDesc);
        if (!staging)
        {
            return false;
        }
        void* mapped = staging->Map(0, uploadSize);
        if (mapped == nullptr)
        {
            return false;
        }
        auto* destination = static_cast<uint8*>(mapped);
        std::memset(destination, 0, static_cast<size_t>(uploadSize));
        for (const PreparedTextureSlice& layout : layouts)
        {
            const uint8* source =
                payload.bytes.data() + layout.sourceOffset;
            for (uint32 row = 0; row < layout.rowCount; ++row)
            {
                const uint8* sourceRow =
                    source + row * layout.sourceRowPitch;
                uint8* destinationRow =
                    destination + layout.uploadOffset +
                    row * layout.uploadRowPitch;
                if (!layout.expandRGB8)
                {
                    std::memcpy(destinationRow,
                                sourceRow,
                                static_cast<size_t>(
                                    layout.sourceTightRowPitch));
                    continue;
                }
                for (uint32 pixel = 0; pixel < layout.width; ++pixel)
                {
                    destinationRow[pixel * 4U + 0U] =
                        sourceRow[pixel * 3U + 0U];
                    destinationRow[pixel * 4U + 1U] =
                        sourceRow[pixel * 3U + 1U];
                    destinationRow[pixel * 4U + 2U] =
                        sourceRow[pixel * 3U + 2U];
                    destinationRow[pixel * 4U + 3U] = 255U;
                }
            }
        }
        staging->Unmap();

        for (const PreparedTextureSlice& layout : layouts)
        {
            RHIBufferTextureCopyDesc copy;
            copy.bufferOffset = layout.uploadOffset;
            copy.bufferRowPitch =
                static_cast<uint32>(layout.uploadRowPitch);
            copy.bufferImageHeight = layout.rowCount;
            copy.textureSubresource = layout.subresource;
            copy.textureRegion = {0, 0, layout.width, layout.height};
            copy.textureDepthSlice = layout.depthSlice;
            context.CopyBufferToTexture(staging->GetBuffer(), texture.Get(), copy);
        }
        context.TextureBarrier(texture.Get(),
                               RHIResourceState::CopyDest,
                               RHIResourceState::ShaderResource);
        stagingBuffers.push_back(std::move(staging));
        return true;
    }

    bool RenderUploadProcessor::BuildMaterial(
        RenderResourceHandle handle,
        const MaterialUploadPayload& payload,
        RHICommandContext& context,
        std::vector<RHIStagingBufferRef>& stagingBuffers)
    {
        if (!m_registry->SetPendingMaterialMetadata(handle, payload) ||
            !RecordMaterialConstants(handle,
                                     payload.sourceData,
                                     context,
                                     stagingBuffers))
        {
            return false;
        }
        for (const MaterialUploadTextureBinding& binding :
             payload.textureBindings)
        {
            RHISamplerRef sampler = m_device->CreateSampler(ToSamplerDesc(binding));
            if (!sampler ||
                !m_registry->AddPendingMaterialSampler(handle,
                                                       std::move(sampler)))
            {
                return false;
            }
        }
        return true;
    }

    bool RenderUploadProcessor::RecordMeshBuffer(
        RenderResourceHandle handle,
        RenderMeshBufferSemantic semantic,
        RHIBufferUsage usage,
        const std::vector<uint8>& bytes,
        UploadByteRange range,
        RHICommandContext& context,
        std::vector<RHIStagingBufferRef>& stagingBuffers)
    {
        if (range.size == 0)
        {
            return true;
        }
        if (range.offset > bytes.size() ||
            range.size > bytes.size() - range.offset)
        {
            return false;
        }

        RHIBufferDesc bufferDesc;
        bufferDesc.size = range.size;
        bufferDesc.usage = usage | RHIBufferUsage::CopyDst;
        bufferDesc.memoryType = RHIMemoryType::Default;
        bufferDesc.stride = range.stride;
        RHIBufferRef buffer = m_device->CreateBuffer(bufferDesc);
        if (!buffer ||
            !m_registry->AddPendingMeshBuffer(handle,
                                              semantic,
                                              buffer,
                                              range.size))
        {
            return false;
        }

        RHIStagingBufferDesc stagingDesc;
        stagingDesc.size = range.size;
        RHIStagingBufferRef staging =
            m_device->CreateStagingBuffer(stagingDesc);
        if (!staging)
        {
            return false;
        }
        void* mapped = staging->Map(0, range.size);
        if (mapped == nullptr)
        {
            return false;
        }
        std::memcpy(mapped,
                    bytes.data() + static_cast<size_t>(range.offset),
                    static_cast<size_t>(range.size));
        staging->Unmap();
        context.CopyBuffer(staging->GetBuffer(),
                           buffer.Get(),
                           0,
                           0,
                           range.size);
        context.BufferBarrier(buffer.Get(),
                              RHIResourceState::CopyDest,
                              usage == RHIBufferUsage::Index
                                  ? RHIResourceState::IndexBuffer
                                  : RHIResourceState::VertexBuffer);
        stagingBuffers.push_back(std::move(staging));
        return true;
    }

    bool RenderUploadProcessor::RecordMaterialConstants(
        RenderResourceHandle handle,
        const MaterialSourceData& sourceData,
        RHICommandContext& context,
        std::vector<RHIStagingBufferRef>& stagingBuffers)
    {
        static_assert(std::is_trivially_copyable_v<MaterialSourceData>);
        const uint64 constantBytes = AlignUp(sizeof(MaterialSourceData), 256);
        RHIBufferDesc bufferDesc;
        bufferDesc.size = constantBytes;
        bufferDesc.usage = RHIBufferUsage::Constant | RHIBufferUsage::CopyDst;
        bufferDesc.memoryType = RHIMemoryType::Default;
        RHIBufferRef buffer = m_device->CreateBuffer(bufferDesc);
        if (!buffer ||
            !m_registry->SetPendingMaterialConstants(handle,
                                                     buffer,
                                                     constantBytes))
        {
            return false;
        }

        RHIStagingBufferDesc stagingDesc;
        stagingDesc.size = sizeof(MaterialSourceData);
        RHIStagingBufferRef staging =
            m_device->CreateStagingBuffer(stagingDesc);
        if (!staging)
        {
            return false;
        }
        void* mapped = staging->Map(0, sizeof(MaterialSourceData));
        if (mapped == nullptr)
        {
            return false;
        }
        std::memcpy(mapped, &sourceData, sizeof(MaterialSourceData));
        staging->Unmap();
        context.CopyBuffer(staging->GetBuffer(),
                           buffer.Get(),
                           0,
                           0,
                           sizeof(MaterialSourceData));
        context.BufferBarrier(buffer.Get(),
                              RHIResourceState::CopyDest,
                              RHIResourceState::ConstantBuffer);
        stagingBuffers.push_back(std::move(staging));
        return true;
    }

    RenderUploadProcessCode RenderUploadProcessor::FailBeforeSubmission(
        ResourceUploadRequestRef& request,
        RenderResourceFailureCode failure,
        RenderUploadProcessCode code)
    {
        const RenderResourceHandle handle =
            request == nullptr ? RenderResourceHandle{} : request->GetHandle();
        if (handle.IsValid() && m_registry->HasPending(handle))
        {
            RVX_ASSERT_MSG(m_registry->RetirePending(handle),
                           "Failed to retire partial render resource");
        }
        request.reset();
        ++m_stats.failed;

        const RenderResourceStatus status = m_statusTable->Query(handle);
        if (status.code == RenderResourceStatusCode::Current &&
            status.state == RenderResourcePublicState::Evicting)
        {
            ++m_stats.cancelled;
            static_cast<void>(PublishTerminal(
                handle,
                RenderResourcePublicState::Released,
                RenderResourceFailureCode::Cancelled));
        }
        else
        {
            static_cast<void>(PublishTerminal(
                handle,
                RenderResourcePublicState::Failed,
                failure));
        }
        return code;
    }

    void RenderUploadProcessor::CompleteInFlight(
        size_t index,
        GPUCompletionStatus completionStatus)
    {
        InFlightUpload& upload = m_inFlight[index];
        const RenderResourceHandle handle = upload.handle;
        const RenderResourceStatus status = m_statusTable->Query(handle);
        const bool cancelled = upload.cancelled ||
                               (status.code == RenderResourceStatusCode::Current &&
                                status.state == RenderResourcePublicState::Evicting);

        RenderResourcePublicState terminalState =
            RenderResourcePublicState::Failed;
        RenderResourceFailureCode failure =
            RenderResourceFailureCode::RuntimeFailure;
        if (cancelled)
        {
            RVX_ASSERT_MSG(m_registry->RetirePending(handle),
                           "Failed to retire cancelled render upload");
            terminalState = RenderResourcePublicState::Released;
            failure = RenderResourceFailureCode::Cancelled;
            ++m_stats.cancelled;
        }
        else if (completionStatus == GPUCompletionStatus::Lost)
        {
            RVX_ASSERT_MSG(m_registry->RetirePending(handle),
                           "Failed to retire device-lost render resource");
            failure = RenderResourceFailureCode::DeviceLost;
            ++m_stats.failed;
        }
        else if (IsCompletionSatisfied(completionStatus) &&
                 m_registry->Commit(handle))
        {
            terminalState = RenderResourcePublicState::GPUReady;
            failure = RenderResourceFailureCode::None;
            ++m_stats.completed;
        }
        else
        {
            RVX_ASSERT_MSG(m_registry->RetirePending(handle),
                           "Failed to retire failed render upload");
            ++m_stats.failed;
        }

        m_stats.inFlightBytes -= upload.retainedBytes;
        upload.request.reset();
        if (index + 1U != m_inFlight.size())
        {
            m_inFlight[index] = std::move(m_inFlight.back());
        }
        m_inFlight.pop_back();
        static_cast<void>(PublishTerminal(handle, terminalState, failure));
    }

    void RenderUploadProcessor::CompleteRelease(
        size_t index,
        GPUCompletionStatus)
    {
        const RenderResourceHandle handle = m_pendingReleases[index].handle;
        const bool released = m_registry->Release(handle);
        RVX_ASSERT_MSG(released,
                       "Failed to retire released render resource");
        if (index + 1U != m_pendingReleases.size())
        {
            m_pendingReleases[index] = m_pendingReleases.back();
        }
        m_pendingReleases.pop_back();
        if (released)
        {
            static_cast<void>(PublishTerminal(
                handle,
                RenderResourcePublicState::Released,
                RenderResourceFailureCode::Cancelled));
        }
    }

    bool RenderUploadProcessor::PublishTerminal(
        RenderResourceHandle handle,
        RenderResourcePublicState state,
        RenderResourceFailureCode failure)
    {
        const RenderResourceStatus observed = m_statusTable->Query(handle);
        if (observed.code != RenderResourceStatusCode::Current)
        {
            return false;
        }
        if ((state == RenderResourcePublicState::Released &&
             observed.state != RenderResourcePublicState::Evicting) ||
            ((state == RenderResourcePublicState::GPUReady ||
              state == RenderResourcePublicState::Failed) &&
             observed.state != RenderResourcePublicState::Uploading))
        {
            return false;
        }
        return m_statusTable->CompareExchange(
            handle,
            PackedRenderResourceStatus{handle.generation,
                                       observed.state,
                                       observed.failure},
            PackedRenderResourceStatus{handle.generation, state, failure},
            RenderStatusWriter::Render);
    }

    RHIFormat RenderUploadProcessor::ToRHIFormat(
        const TextureUploadCreateInfo& info)
    {
        switch (info.format)
        {
            case TextureUploadFormat::RGBA8:
                return info.isSRGB ? RHIFormat::RGBA8_UNORM_SRGB
                                   : RHIFormat::RGBA8_UNORM;
            case TextureUploadFormat::RGBA16F:
                return RHIFormat::RGBA16_FLOAT;
            case TextureUploadFormat::RGBA32F:
                return RHIFormat::RGBA32_FLOAT;
            case TextureUploadFormat::RGB8:
                return info.isSRGB ? RHIFormat::RGBA8_UNORM_SRGB
                                   : RHIFormat::RGBA8_UNORM;
            case TextureUploadFormat::RG8:
                return RHIFormat::RG8_UNORM;
            case TextureUploadFormat::R8:
                return RHIFormat::R8_UNORM;
            case TextureUploadFormat::BC1:
                return info.isSRGB ? RHIFormat::BC1_UNORM_SRGB
                                   : RHIFormat::BC1_UNORM;
            case TextureUploadFormat::BC3:
                return info.isSRGB ? RHIFormat::BC3_UNORM_SRGB
                                   : RHIFormat::BC3_UNORM;
            case TextureUploadFormat::BC5:
                return RHIFormat::BC5_UNORM;
            case TextureUploadFormat::BC7:
                return info.isSRGB ? RHIFormat::BC7_UNORM_SRGB
                                   : RHIFormat::BC7_UNORM;
            case TextureUploadFormat::Unknown:
            default:
                return RHIFormat::Unknown;
        }
    }

    RHISamplerDesc RenderUploadProcessor::ToSamplerDesc(
        const MaterialUploadTextureBinding& binding)
    {
        auto filter = [](MaterialUploadFilterMode value)
        {
            return value == MaterialUploadFilterMode::Nearest ||
                           value ==
                               MaterialUploadFilterMode::NearestMipmapNearest ||
                           value ==
                               MaterialUploadFilterMode::NearestMipmapLinear
                       ? RHIFilterMode::Nearest
                       : RHIFilterMode::Linear;
        };
        auto address = [](MaterialUploadWrapMode value)
        {
            switch (value)
            {
                case MaterialUploadWrapMode::MirrorRepeat:
                    return RHIAddressMode::MirrorRepeat;
                case MaterialUploadWrapMode::ClampToEdge:
                    return RHIAddressMode::ClampToEdge;
                case MaterialUploadWrapMode::ClampToBorder:
                    return RHIAddressMode::ClampToBorder;
                case MaterialUploadWrapMode::Repeat:
                default:
                    return RHIAddressMode::Repeat;
            }
        };

        RHISamplerDesc desc;
        desc.minFilter = filter(binding.minFilter);
        desc.magFilter = filter(binding.magFilter);
        desc.mipFilter = filter(binding.minFilter);
        desc.addressU = address(binding.wrapS);
        desc.addressV = address(binding.wrapT);
        return desc;
    }
} // namespace RVX
