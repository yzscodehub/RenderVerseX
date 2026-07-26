/**
 * @file main.cpp
 * @brief ThreadSanitizer stress for the M1 render publication core.
 */

#include "Render/RenderDiagnostics.h"
#include "Runtime/RenderControlMailbox.h"
#include "Runtime/RenderDiagnosticsPublisher.h"
#include "Runtime/RenderFrameMailbox.h"
#include "Runtime/RenderResourceGateway.h"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <memory>
#include <string>
#include <thread>

namespace RVX
{
namespace
{
    constexpr uint64 RVX_TSAN_ITERATIONS = 10000;

    struct StressFramePacket
    {
        explicit StressFramePacket(uint64 sequence)
        {
            header.sequence = sequence;
        }

        [[nodiscard]] const RenderFrameHeader& GetHeader() const noexcept
        {
            return header;
        }

        RenderFrameHeader header{};
    };

    struct StressFrameValidator
    {
        [[nodiscard]] bool operator()(
            const StressFramePacket& packet) const noexcept
        {
            return packet.GetHeader().sequence != 0U;
        }
    };

    using StressFrameMailbox =
        BasicRenderFrameMailbox<StressFramePacket, StressFrameValidator>;

    TEST(RenderConcurrencyTSAN,
         FrameReplacementAndDiagnosticsPublicationRemainRaceFree)
    {
        StressFrameMailbox mailbox(4);
        RenderDiagnosticsPublisher diagnostics;
        std::barrier start(4);
        std::atomic<bool> producerDone = false;
        std::atomic<bool> invalidObservation = false;

        std::thread producer([&]() {
            start.arrive_and_wait();
            for (uint64 sequence = 1; sequence <= RVX_TSAN_ITERATIONS;
                 ++sequence)
            {
                const RenderFrameMailboxPublishResult published =
                    mailbox.TryPublish(
                        std::make_unique<StressFramePacket>(sequence));
                if (published.code !=
                        RenderFrameMailboxPublishCode::Accepted &&
                    published.code !=
                        RenderFrameMailboxPublishCode::ReplacedOldest)
                {
                    invalidObservation.store(true,
                                             std::memory_order_relaxed);
                }

                RenderDiagnosticsSnapshot snapshot;
                snapshot.publicationSequence = sequence;
                snapshot.surfaceGeneration = sequence;
                snapshot.lastFailure.message =
                    "diagnostic-" + std::to_string(sequence);
                diagnostics.Publish(std::move(snapshot));
            }
            producerDone.store(true, std::memory_order_release);
        });

        std::thread consumer([&]() {
            start.arrive_and_wait();
            uint64 lastSequence = 0;
            while (!producerDone.load(std::memory_order_acquire) ||
                   mailbox.GetPendingCount() != 0U)
            {
                auto acquired = mailbox.AcquireLatest();
                if (acquired.packet == nullptr)
                {
                    std::this_thread::yield();
                    continue;
                }
                const uint64 sequence =
                    acquired.packet->GetHeader().sequence;
                if (sequence <= lastSequence)
                {
                    invalidObservation.store(true,
                                             std::memory_order_relaxed);
                }
                lastSequence = sequence;
            }
            if (lastSequence != RVX_TSAN_ITERATIONS)
            {
                invalidObservation.store(true, std::memory_order_relaxed);
            }
        });

        const auto readDiagnostics = [&]() {
            start.arrive_and_wait();
            uint64 lastSequence = 0;
            while (!producerDone.load(std::memory_order_acquire) ||
                   lastSequence != RVX_TSAN_ITERATIONS)
            {
                const auto snapshot = diagnostics.AcquireShared();
                if (snapshot == nullptr)
                {
                    invalidObservation.store(true,
                                             std::memory_order_relaxed);
                    return;
                }
                if (snapshot->publicationSequence < lastSequence ||
                    snapshot->surfaceGeneration !=
                        snapshot->publicationSequence)
                {
                    invalidObservation.store(true,
                                             std::memory_order_relaxed);
                }
                lastSequence = snapshot->publicationSequence;
                if (lastSequence == 0U)
                {
                    std::this_thread::yield();
                }
            }
        };

        std::thread reader(readDiagnostics);
        readDiagnostics();

        producer.join();
        consumer.join();
        reader.join();
        EXPECT_FALSE(invalidObservation.load(std::memory_order_relaxed));
    }

    TEST(RenderConcurrencyTSAN,
         ReleasePublicationAndStatusReuseRemainRaceFree)
    {
        RenderTransportConfig config;
        config.statusSlotCapacity = 1024;
        RenderResourceGateway gateway(config);
        std::barrier start(3);
        std::atomic<bool> producerDone = false;
        std::atomic<bool> invalidObservation = false;
        std::atomic<uint64> completed = 0;
        std::atomic<uint64> observedHandle = 0;

        std::thread producer([&]() {
            start.arrive_and_wait();
            for (uint64 index = 1; index <= RVX_TSAN_ITERATIONS; ++index)
            {
                RenderResourceReserveResult reserved;
                do
                {
                    reserved = gateway.ReserveResource(
                        AssetId{index}, RenderResourceKind::Texture);
                    if (reserved.code ==
                        RenderResourceReserveCode::CapacityExceeded)
                    {
                        std::this_thread::yield();
                    }
                } while (reserved.code ==
                         RenderResourceReserveCode::CapacityExceeded);

                if (reserved.code != RenderResourceReserveCode::Reserved)
                {
                    invalidObservation.store(true,
                                             std::memory_order_relaxed);
                    break;
                }
                observedHandle.store(
                    (static_cast<uint64>(reserved.handle.generation) << 32U) |
                        reserved.handle.slot,
                    std::memory_order_release);
                if (gateway.RequestRelease(reserved.handle).code !=
                    RenderReleaseCode::Accepted)
                {
                    invalidObservation.store(true,
                                             std::memory_order_relaxed);
                    break;
                }
            }
            producerDone.store(true, std::memory_order_release);
        });

        std::thread consumer([&]() {
            start.arrive_and_wait();
            for (;;)
            {
                const RenderResourceHandle handle =
                    gateway.TryDequeueRelease();
                if (!handle.IsValid())
                {
                    if (producerDone.load(std::memory_order_acquire) &&
                        gateway.GetReleaseQueueSnapshot().pendingCount == 0U)
                    {
                        break;
                    }
                    std::this_thread::yield();
                    continue;
                }
                const PackedRenderResourceStatus expected{
                    handle.generation,
                    RenderResourcePublicState::Evicting,
                    RenderResourceFailureCode::None};
                const PackedRenderResourceStatus released{
                    handle.generation,
                    RenderResourcePublicState::Released,
                    RenderResourceFailureCode::None};
                if (!gateway.GetStatusTable().CompareExchange(
                        handle,
                        expected,
                        released,
                        RenderStatusWriter::Render))
                {
                    invalidObservation.store(true,
                                             std::memory_order_relaxed);
                }
                completed.fetch_add(1U, std::memory_order_relaxed);
            }
        });

        start.arrive_and_wait();
        while (!producerDone.load(std::memory_order_acquire))
        {
            const uint64 packed =
                observedHandle.load(std::memory_order_acquire);
            const RenderResourceHandle handle{
                static_cast<uint32>(packed),
                static_cast<uint32>(packed >> 32U)};
            if (handle.IsValid())
            {
                const RenderResourceStatus status =
                    gateway.QueryResourceStatus(handle);
                if (status.code == RenderResourceStatusCode::Current &&
                    status.state != RenderResourcePublicState::Reserved &&
                    status.state != RenderResourcePublicState::Evicting &&
                    status.state != RenderResourcePublicState::Released)
                {
                    invalidObservation.store(true,
                                             std::memory_order_relaxed);
                }
            }
            std::this_thread::yield();
        }

        producer.join();
        consumer.join();
        EXPECT_EQ(completed.load(std::memory_order_relaxed),
                  RVX_TSAN_ITERATIONS);
        EXPECT_FALSE(invalidObservation.load(std::memory_order_relaxed));
    }

    TEST(RenderConcurrencyTSAN,
         ControlCoalescingAndStopPreemptionRemainRaceFree)
    {
        BasicRenderControlMailbox<uint64, uint64> mailbox;
        std::barrier start(2);
        std::atomic<bool> invalidObservation = false;

        std::thread consumer([&]() {
            start.arrive_and_wait();
            uint64 lastSurface = 0;
            uint64 lastResize = 0;
            for (;;)
            {
                auto batch = mailbox.AcquireLatest();
                if (batch.stopRequested)
                {
                    break;
                }
                if (batch.surface.has_value())
                {
                    if (batch.surface->generation <= lastSurface ||
                        batch.surface->value !=
                            batch.surface->generation)
                    {
                        invalidObservation.store(true,
                                                 std::memory_order_relaxed);
                    }
                    lastSurface = batch.surface->generation;
                }
                if (batch.resize.has_value())
                {
                    if (batch.resize->generation <= lastResize ||
                        batch.resize->value !=
                            batch.resize->generation)
                    {
                        invalidObservation.store(true,
                                                 std::memory_order_relaxed);
                    }
                    lastResize = batch.resize->generation;
                }
                std::this_thread::yield();
            }
        });

        start.arrive_and_wait();
        for (uint64 generation = 1; generation <= RVX_TSAN_ITERATIONS;
             ++generation)
        {
            if (!mailbox.TryPublishSurface(generation, generation) ||
                !mailbox.TryPublishResize(generation, generation))
            {
                invalidObservation.store(true,
                                         std::memory_order_relaxed);
            }
        }
        mailbox.RequestStop();
        consumer.join();

        EXPECT_TRUE(mailbox.IsStopRequested());
        EXPECT_FALSE(invalidObservation.load(std::memory_order_relaxed));
    }
} // namespace
} // namespace RVX
