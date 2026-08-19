#include "Render/GPUUploadService.h"

#include "Resources/RenderSubmissionTracker.h"
#include "Resources/RenderUploadProcessor.h"

namespace RVX
{
    GPUUploadService::GPUUploadService() = default;

    GPUUploadService::~GPUUploadService()
    {
        Shutdown();
    }

    void GPUUploadService::Initialize(IRHIDevice* device)
    {
        Shutdown();
        if (device == nullptr)
        {
            return;
        }
        auto tracker = std::make_unique<RenderSubmissionTracker>();
        auto processor = std::make_unique<RenderUploadProcessor>();
        if (!tracker->Initialize(device) ||
            !processor->InitializeLegacy(device, tracker.get()))
        {
            processor->ShutdownLegacy();
            tracker->Shutdown();
            return;
        }
        m_submissionTracker = std::move(tracker);
        m_processor = std::move(processor);
    }

    void GPUUploadService::Shutdown()
    {
        if (m_processor)
        {
            m_processor->ShutdownLegacy();
            m_processor.reset();
        }
        if (m_submissionTracker)
        {
            m_submissionTracker->Shutdown();
            m_submissionTracker.reset();
        }
    }

    bool GPUUploadService::IsInitialized() const
    {
        return m_processor && m_processor->IsLegacyInitialized();
    }

    RHIBufferRef GPUUploadService::UploadBufferData(
        const GPUUploadBufferDesc& desc,
        const void* data,
        uint64 dataSize)
    {
        return m_processor
                   ? m_processor->UploadLegacyBufferData(desc, data, dataSize)
                   : RHIBufferRef{};
    }

    RHITextureRef GPUUploadService::UploadTextureData(
        const GPUUploadTextureDesc& desc,
        const void* data)
    {
        return m_processor
                   ? m_processor->UploadLegacyTextureData(desc, data)
                   : RHITextureRef{};
    }

    GPUUploadBufferResult GPUUploadService::UploadBufferDataWithResult(
        const GPUUploadBufferDesc& desc,
        const void* data,
        uint64 dataSize)
    {
        return m_processor
                   ? m_processor->UploadLegacyBufferDataWithResult(
                         desc, data, dataSize)
                   : GPUUploadBufferResult{};
    }

    GPUUploadTextureResult GPUUploadService::UploadTextureDataWithResult(
        const GPUUploadTextureDesc& desc,
        const void* data)
    {
        return m_processor
                   ? m_processor->UploadLegacyTextureDataWithResult(desc, data)
                   : GPUUploadTextureResult{};
    }

    void GPUUploadService::FlushBatchUploads()
    {
        if (m_processor)
        {
            m_processor->FlushLegacyBatchUploads();
        }
    }

    uint32 GPUUploadService::FlushAndWaitForUploads()
    {
        return m_processor ? m_processor->FlushAndWaitForLegacyUploads() : 0;
    }

    uint32 GPUUploadService::ProcessCompletedUploads()
    {
        return m_processor ? m_processor->ProcessCompletedLegacyUploads() : 0;
    }

    bool GPUUploadService::IsUploadPending(uint64 uploadId) const
    {
        return m_processor && m_processor->IsLegacyUploadPending(uploadId);
    }

    bool GPUUploadService::IsUploadComplete(uint64 uploadId) const
    {
        return !m_processor || m_processor->IsLegacyUploadComplete(uploadId);
    }

    void GPUUploadService::ForgetCompletedUpload(uint64 uploadId)
    {
        if (m_processor)
        {
            m_processor->ForgetCompletedLegacyUpload(uploadId);
        }
    }

    void GPUUploadService::AbandonUpload(uint64 uploadId)
    {
        if (m_processor)
        {
            m_processor->AbandonLegacyUpload(uploadId);
        }
    }

    const GPUUploadService::Stats& GPUUploadService::GetStats() const
    {
        static const Stats empty;
        return m_processor ? m_processor->GetLegacyStats() : empty;
    }

    void GPUUploadService::ResetStats()
    {
        if (m_processor)
        {
            m_processor->ResetLegacyStats();
        }
    }
} // namespace RVX
