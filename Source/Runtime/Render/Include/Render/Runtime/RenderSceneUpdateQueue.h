#pragma once

/**
 * @file RenderSceneUpdateQueue.h
 * @brief Bounded reliable FIFO transport for render-scene revisions.
 */

#include "Core/Types.h"
#include "RenderContracts/RenderSceneUpdate.h"

#include <memory>
#include <mutex>
#include <vector>

namespace RVX
{
    enum class RenderSceneUpdatePublishCode : uint8
    {
        Accepted = 0,
        Full,
        InvalidBatch,
        RevisionMismatch
    };

    struct RenderSceneUpdateQueueSnapshot
    {
        uint32 pendingCount = 0;
        uint32 capacity = 0;
        uint32 highWaterMark = 0;
        uint64 lastPublishedRevision = 0;
        uint64 lastAcquiredRevision = 0;
    };

    struct RenderSceneUpdatePublishResult
    {
        RenderSceneUpdatePublishCode code =
            RenderSceneUpdatePublishCode::InvalidBatch;
        std::unique_ptr<const RenderSceneUpdateBatch> rejectedBatch;
        RenderSceneUpdateQueueSnapshot snapshot{};

        [[nodiscard]] bool IsAccepted() const noexcept
        {
            return code == RenderSceneUpdatePublishCode::Accepted;
        }
    };

    /**
     * @brief Single-producer/single-consumer reliable ordered update queue.
     *
     * Unlike the frame mailbox, a full queue never replaces an entry. Ownership
     * is returned to the producer so it can retain/coalesce pending mutations.
     */
    class RenderSceneUpdateQueue final
    {
    public:
        using WakeFunction = void (*)(void*) noexcept;

        explicit RenderSceneUpdateQueue(uint32 capacity = 4,
                                        WakeFunction wakeFunction = nullptr,
                                        void* wakeContext = nullptr);

        RenderSceneUpdateQueue(const RenderSceneUpdateQueue&) = delete;
        RenderSceneUpdateQueue& operator=(const RenderSceneUpdateQueue&) = delete;

        [[nodiscard]] RenderSceneUpdatePublishResult TryPublish(
            std::unique_ptr<const RenderSceneUpdateBatch> batch) noexcept;
        [[nodiscard]] std::unique_ptr<const RenderSceneUpdateBatch>
            AcquireNext() noexcept;
        void Clear() noexcept;

        [[nodiscard]] RenderSceneUpdateQueueSnapshot GetSnapshot() const noexcept;

    private:
        [[nodiscard]] RenderSceneUpdateQueueSnapshot GetSnapshotLocked() const noexcept;
        void Wake() const noexcept;

        uint32 m_capacity = 0;
        mutable std::mutex m_mutex;
        std::vector<std::unique_ptr<const RenderSceneUpdateBatch>> m_slots;
        uint32 m_head = 0;
        uint32 m_count = 0;
        uint32 m_highWaterMark = 0;
        uint64 m_lastPublishedRevision = 0;
        uint64 m_lastAcquiredRevision = 0;
        WakeFunction m_wakeFunction = nullptr;
        void* m_wakeContext = nullptr;
    };
} // namespace RVX
