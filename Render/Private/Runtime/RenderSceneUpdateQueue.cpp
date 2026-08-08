#include "Render/Runtime/RenderSceneUpdateQueue.h"

#include <stdexcept>
#include <utility>

namespace RVX
{

RenderSceneUpdateQueue::RenderSceneUpdateQueue(uint32 capacity,
                                               WakeFunction wakeFunction,
                                               void* wakeContext)
    : m_capacity(capacity)
    , m_slots(capacity)
    , m_wakeFunction(wakeFunction)
    , m_wakeContext(wakeContext)
{
    if (capacity < 2U || capacity > 16U)
    {
        throw std::invalid_argument(
            "Render scene update queue capacity must be in [2, 16]");
    }
}

RenderSceneUpdatePublishResult RenderSceneUpdateQueue::TryPublish(
    std::unique_ptr<const RenderSceneUpdateBatch> batch) noexcept
{
    RenderSceneUpdatePublishResult result;
    if (batch == nullptr || !batch->IsStructurallyValid())
    {
        result.rejectedBatch = std::move(batch);
        result.snapshot = GetSnapshot();
        return result;
    }

    bool accepted = false;
    {
        std::lock_guard lock(m_mutex);
        const bool isInitialCheckpoint = m_lastPublishedRevision == 0;
        const bool revisionMatches = batch->fullReset
            ? batch->baseSceneRevision == 0 &&
                  batch->targetSceneRevision > m_lastPublishedRevision
            : !isInitialCheckpoint &&
                  batch->baseSceneRevision == m_lastPublishedRevision;
        if (!revisionMatches)
        {
            result.code = RenderSceneUpdatePublishCode::RevisionMismatch;
            result.rejectedBatch = std::move(batch);
        }
        else if (m_count == m_capacity)
        {
            result.code = RenderSceneUpdatePublishCode::Full;
            result.rejectedBatch = std::move(batch);
        }
        else
        {
            const uint32 index = (m_head + m_count) % m_capacity;
            m_lastPublishedRevision = batch->targetSceneRevision;
            m_slots[index] = std::move(batch);
            ++m_count;
            if (m_count > m_highWaterMark)
                m_highWaterMark = m_count;
            result.code = RenderSceneUpdatePublishCode::Accepted;
            accepted = true;
        }
        result.snapshot = GetSnapshotLocked();
    }

    if (accepted)
        Wake();
    return result;
}

std::unique_ptr<const RenderSceneUpdateBatch>
RenderSceneUpdateQueue::AcquireNext() noexcept
{
    std::lock_guard lock(m_mutex);
    if (m_count == 0)
        return {};

    std::unique_ptr<const RenderSceneUpdateBatch> batch =
        std::move(m_slots[m_head]);
    m_head = (m_head + 1U) % m_capacity;
    --m_count;
    m_lastAcquiredRevision = batch->targetSceneRevision;
    return batch;
}

void RenderSceneUpdateQueue::Clear() noexcept
{
    std::lock_guard lock(m_mutex);
    for (auto& slot : m_slots)
        slot.reset();
    m_head = 0;
    m_count = 0;
    m_highWaterMark = 0;
    m_lastPublishedRevision = 0;
    m_lastAcquiredRevision = 0;
}

RenderSceneUpdateQueueSnapshot RenderSceneUpdateQueue::GetSnapshot() const noexcept
{
    std::lock_guard lock(m_mutex);
    return GetSnapshotLocked();
}

RenderSceneUpdateQueueSnapshot
RenderSceneUpdateQueue::GetSnapshotLocked() const noexcept
{
    return RenderSceneUpdateQueueSnapshot{
        m_count,
        m_capacity,
        m_highWaterMark,
        m_lastPublishedRevision,
        m_lastAcquiredRevision};
}

void RenderSceneUpdateQueue::Wake() const noexcept
{
    if (m_wakeFunction != nullptr)
        m_wakeFunction(m_wakeContext);
}

} // namespace RVX
