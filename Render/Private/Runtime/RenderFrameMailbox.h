#pragma once

/**
 * @file RenderFrameMailbox.h
 * @brief Bounded latest-complete-wins ownership transport for frame packets.
 */

#include "Core/Types.h"
#include "RenderContracts/RenderFramePacket.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace RVX
{
    enum class RenderFrameMailboxPublishCode : uint8
    {
        Accepted = 0,
        ReplacedOldest = 1,
        InvalidPacket = 2,
        OutOfOrder = 3
    };

    struct RenderFrameMailboxPublishResult
    {
        RenderFrameMailboxPublishCode code =
            RenderFrameMailboxPublishCode::InvalidPacket;
        uint64 replacedSequence = 0;
    };

    template <typename Packet>
    struct CompleteRenderFramePacketValidator final
    {
        [[nodiscard]] bool operator()(const Packet& packet) const noexcept
        {
            const RenderFrameHeader& header = packet.GetHeader();
            const RenderExtractionDiagnostics& diagnostics =
                packet.GetExtractionDiagnostics();
            const RenderFeatureSnapshotMetadata featureMetadata =
                packet.GetFeatures().GetMetadata();
            return header.schemaId == RVX_RENDER_FRAME_PACKET_SCHEMA_ID &&
                   header.schemaVersion ==
                       RVX_RENDER_FRAME_PACKET_SCHEMA_VERSION &&
                   header.sequence != 0U && diagnostics.complete &&
                   diagnostics.code == RenderExtractionCode::Complete &&
                   header.expectedPrimitiveCount ==
                       header.extractedPrimitiveCount &&
                   header.extractedPrimitiveCount ==
                       packet.GetPrimitives().size() &&
                   header.expectedLightCount == header.extractedLightCount &&
                   header.extractedLightCount == packet.GetLights().size() &&
                   header.expectedFeatureProviderCount ==
                       header.extractedFeatureProviderCount &&
                   header.extractedFeatureProviderCount ==
                       featureMetadata.providerCount &&
                   featureMetadata.schemaVersion ==
                       RVX_RENDER_FEATURE_SNAPSHOT_SCHEMA_VERSION &&
                   featureMetadata.sequence == header.sequence &&
                   featureMetadata.status ==
                       RenderFeatureSnapshotStatus::Complete &&
                   featureMetadata.complete &&
                   featureMetadata.skippedProviderCount == 0U;
        }
    };

    template <typename Packet>
    struct BasicRenderFrameAcquireResult
    {
        std::unique_ptr<const Packet> packet;
        std::array<uint64, 4> discardedSequences{};
        uint32 discardedCount = 0;
    };

    template <typename Packet, typename Validator>
    class BasicRenderFrameMailbox final
    {
    public:
        using WakeFunction = void (*)(void*) noexcept;

        explicit BasicRenderFrameMailbox(uint32 capacity,
                                         WakeFunction wakeFunction = nullptr,
                                         void* wakeContext = nullptr)
            : m_capacity(capacity),
              m_slots(capacity),
              m_wakeFunction(wakeFunction),
              m_wakeContext(wakeContext)
        {
            if (capacity < 2U || capacity > 4U)
            {
                throw std::invalid_argument(
                    "Render frame mailbox capacity must be in [2, 4]");
            }
        }

        BasicRenderFrameMailbox(const BasicRenderFrameMailbox&) = delete;
        BasicRenderFrameMailbox& operator=(
            const BasicRenderFrameMailbox&) = delete;

        RenderFrameMailboxPublishResult TryPublish(
            std::unique_ptr<const Packet> packet) noexcept
        {
            RenderFrameMailboxPublishResult result;
            if (packet == nullptr || !Validator{}(*packet))
            {
                return result;
            }

            const uint64 sequence = packet->GetHeader().sequence;
            uint64 observed =
                m_lastPublishedSequence.load(std::memory_order_acquire);
            for (;;)
            {
                if (sequence <= observed)
                {
                    result.code = RenderFrameMailboxPublishCode::OutOfOrder;
                    return result;
                }
                if (m_lastPublishedSequence.compare_exchange_weak(
                        observed,
                        sequence,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire))
                {
                    break;
                }
            }

            std::unique_ptr<const Packet> replaced;
            {
                std::lock_guard lock(m_mutex);
                if (m_count == m_capacity)
                {
                    const uint32 replaceIndex = m_head;
                    result.replacedSequence =
                        m_slots[replaceIndex]->GetHeader().sequence;
                    replaced = std::move(m_slots[replaceIndex]);
                    m_slots[replaceIndex] = std::move(packet);
                    m_head = (m_head + 1U) % m_capacity;
                    result.code =
                        RenderFrameMailboxPublishCode::ReplacedOldest;
                }
                else
                {
                    const uint32 appendIndex =
                        (m_head + m_count) % m_capacity;
                    m_slots[appendIndex] = std::move(packet);
                    ++m_count;
                    result.code = RenderFrameMailboxPublishCode::Accepted;
                }
            }

            Wake();
            return result;
        }

        [[nodiscard]] BasicRenderFrameAcquireResult<Packet>
            AcquireLatest() noexcept
        {
            BasicRenderFrameAcquireResult<Packet> result;
            std::array<std::unique_ptr<const Packet>, 4> discarded;
            {
                std::lock_guard lock(m_mutex);
                if (m_count == 0U)
                {
                    return result;
                }

                const uint32 newestIndex =
                    (m_head + m_count - 1U) % m_capacity;
                result.packet = std::move(m_slots[newestIndex]);
                for (uint32 offset = 0; offset + 1U < m_count; ++offset)
                {
                    const uint32 discardIndex =
                        (m_head + offset) % m_capacity;
                    result.discardedSequences[result.discardedCount] =
                        m_slots[discardIndex]->GetHeader().sequence;
                    discarded[result.discardedCount] =
                        std::move(m_slots[discardIndex]);
                    ++result.discardedCount;
                }
                m_head = 0U;
                m_count = 0U;
            }
            return result;
        }

        [[nodiscard]] uint32 GetPendingCount() const noexcept
        {
            std::lock_guard lock(m_mutex);
            return m_count;
        }

    private:
        void Wake() const noexcept
        {
            if (m_wakeFunction != nullptr)
            {
                m_wakeFunction(m_wakeContext);
            }
        }

        uint32 m_capacity = 0;
        mutable std::mutex m_mutex;
        std::vector<std::unique_ptr<const Packet>> m_slots;
        uint32 m_head = 0;
        uint32 m_count = 0;
        std::atomic<uint64> m_lastPublishedSequence = 0;
        WakeFunction m_wakeFunction = nullptr;
        void* m_wakeContext = nullptr;
    };

    using RenderFrameMailbox = BasicRenderFrameMailbox<
        RenderFramePacket,
        CompleteRenderFramePacketValidator<RenderFramePacket>>;
    using RenderFrameAcquireResult =
        BasicRenderFrameAcquireResult<RenderFramePacket>;
} // namespace RVX
