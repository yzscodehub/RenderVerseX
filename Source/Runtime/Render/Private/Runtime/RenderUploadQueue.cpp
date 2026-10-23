#include "Runtime/RenderUploadQueue.h"

#include <stdexcept>

namespace RVX
{
namespace
{
    [[nodiscard]] RenderUploadEnqueueResult MakeUploadResult(
        RenderUploadEnqueueCode code) noexcept
    {
        return RenderUploadEnqueueResult{code};
    }

    [[nodiscard]] bool IsRequestLocallyValid(
        const ResourceUploadRequestRef& request) noexcept
    {
        return request != nullptr &&
               request->GetSchemaId() ==
                   RVX_RESOURCE_UPLOAD_REQUEST_SCHEMA_ID &&
               request->GetSchemaVersion() ==
                   RVX_RESOURCE_UPLOAD_REQUEST_SCHEMA_VERSION &&
               request->GetSequence() != 0U &&
               request->GetAssetId().IsValid() &&
               request->GetHandle().IsValid() &&
               request->GetKind() != RenderResourceKind::Invalid &&
               (request->GetOperation() ==
                    RenderResourceContentOperation::Create ||
                request->GetOperation() ==
                    RenderResourceContentOperation::Replace) &&
               (request->GetOperation() !=
                    RenderResourceContentOperation::Replace ||
                request->GetSourceRevision() != 0);
    }

    [[nodiscard]] RenderUploadEnqueueCode MapStatusForEnqueue(
        const RenderResourceStatus& status) noexcept
    {
        if (status.code == RenderResourceStatusCode::StaleGeneration)
        {
            return RenderUploadEnqueueCode::StaleGeneration;
        }
        if (status.code != RenderResourceStatusCode::Current)
        {
            return RenderUploadEnqueueCode::InvalidRequest;
        }
        if (status.state == RenderResourcePublicState::Evicting ||
            status.state == RenderResourcePublicState::Released ||
            status.state == RenderResourcePublicState::Failed)
        {
            return RenderUploadEnqueueCode::Cancelled;
        }
        return RenderUploadEnqueueCode::InvalidRequest;
    }
} // namespace

    RenderUploadQueue::RenderUploadQueue(
        const RenderTransportConfig& config,
        RenderResourceStatusTable& statusTable,
        WakeFunction wakeFunction,
        void* wakeContext)
        : m_statusTable(statusTable),
          m_requestCapacity(config.uploadRequestCapacity),
          m_byteCapacity(config.uploadByteCapacity),
          m_slots(config.uploadRequestCapacity),
          m_wakeFunction(wakeFunction),
          m_wakeContext(wakeContext)
    {
        if (m_requestCapacity == 0U || m_byteCapacity == 0U)
        {
            throw std::invalid_argument(
                "Render upload capacities must be nonzero");
        }
    }

    RenderUploadEnqueueResult RenderUploadQueue::TryEnqueue(
        const ResourceUploadRequestRef& request,
        RenderUploadQueueSnapshot* observation,
        bool notifyConsumer) noexcept
    {
        {
            std::lock_guard lock(m_mutex);
            if (!IsRequestLocallyValid(request))
            {
                return MakeUploadResult(
                    RenderUploadEnqueueCode::InvalidRequest);
            }

            const RenderResourceHandle handle = request->GetHandle();
            const RenderResourceStatus status = m_statusTable.Query(handle);
            const bool replacement = request->GetOperation() ==
                                     RenderResourceContentOperation::Replace;
            const RenderResourcePublicState expectedState = replacement
                ? RenderResourcePublicState::GPUReady
                : RenderResourcePublicState::Reserved;
            const RenderResourcePublicState queuedState = replacement
                ? RenderResourcePublicState::ReplacementQueued
                : RenderResourcePublicState::UploadQueued;
            if (status.code != RenderResourceStatusCode::Current ||
                status.state != expectedState)
            {
                return MakeUploadResult(MapStatusForEnqueue(status));
            }
            if (m_count == m_requestCapacity)
            {
                return MakeUploadResult(
                    RenderUploadEnqueueCode::QueueFullByCount);
            }

            const uint64 requestBytes = request->GetDerivedPayloadBytes();
            if (requestBytes > m_byteCapacity ||
                m_retainedBytes > m_byteCapacity - requestBytes)
            {
                return MakeUploadResult(
                    RenderUploadEnqueueCode::QueueFullByBytes);
            }

            const PackedRenderResourceStatus expected{
                handle.generation, status.state, status.failure};
            const PackedRenderResourceStatus desired{
                handle.generation,
                queuedState,
                status.failure};
            if (!m_statusTable.CompareExchange(handle,
                                               expected,
                                               desired,
                                               RenderStatusWriter::Update))
            {
                return MakeUploadResult(
                    MapStatusForEnqueue(m_statusTable.Query(handle)));
            }

            const uint32 appendIndex =
                (m_head + m_count) % m_requestCapacity;
            m_slots[appendIndex] = request;
            ++m_count;
            m_retainedBytes += requestBytes;
            if (m_count > m_requestHighWaterMark)
            {
                m_requestHighWaterMark = m_count;
            }
            if (m_retainedBytes > m_byteHighWaterMark)
            {
                m_byteHighWaterMark = m_retainedBytes;
            }
            if (observation != nullptr)
            {
                *observation = RenderUploadQueueSnapshot{
                    m_count,
                    m_retainedBytes,
                    m_requestHighWaterMark,
                    m_byteHighWaterMark};
            }
        }

        if (notifyConsumer)
            NotifyConsumer();
        return MakeUploadResult(RenderUploadEnqueueCode::Accepted);
    }

    ResourceUploadRequestRef RenderUploadQueue::TryDequeue() noexcept
    {
        std::lock_guard lock(m_mutex);
        if (m_count == 0U)
        {
            return {};
        }

        ResourceUploadRequestRef request = std::move(m_slots[m_head]);
        m_slots[m_head].reset();
        m_head = (m_head + 1U) % m_requestCapacity;
        --m_count;
        m_retainedBytes -= request->GetDerivedPayloadBytes();
        return request;
    }

    uint32 RenderUploadQueue::GetRetainedCount() const noexcept
    {
        std::lock_guard lock(m_mutex);
        return m_count;
    }

    uint64 RenderUploadQueue::GetRetainedBytes() const noexcept
    {
        std::lock_guard lock(m_mutex);
        return m_retainedBytes;
    }

    RenderUploadQueueSnapshot RenderUploadQueue::GetSnapshot() const noexcept
    {
        std::lock_guard lock(m_mutex);
        return RenderUploadQueueSnapshot{m_count,
                                         m_retainedBytes,
                                         m_requestHighWaterMark,
                                         m_byteHighWaterMark};
    }

    void RenderUploadQueue::NotifyConsumer() const noexcept
    {
        if (m_wakeFunction != nullptr)
        {
            m_wakeFunction(m_wakeContext);
        }
    }
} // namespace RVX
