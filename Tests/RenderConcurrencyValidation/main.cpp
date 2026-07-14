#include "RenderContracts/IRenderResourceGateway.h"
#include "Runtime/RenderResourceReservationDirectory.h"
#include "Runtime/RenderResourceStatusTable.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <latch>
#include <limits>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace RVX
{
    struct RenderResourceStatusTableTestAccess
    {
        static uint64 Pack(PackedRenderResourceStatus status) noexcept
        {
            return (static_cast<uint64>(status.generation) << 32U) |
                   (static_cast<uint64>(status.failure) << 16U) |
                   static_cast<uint64>(status.state);
        }

        static uint64 Load(const RenderResourceStatusTable& table,
                           uint32 slot) noexcept
        {
            return table.m_words[slot].load(std::memory_order_relaxed);
        }

        static void Store(RenderResourceStatusTable& table,
                          uint32 slot,
                          PackedRenderResourceStatus status,
                          uint64 extraBits = 0) noexcept
        {
            table.m_words[slot].store(Pack(status) | extraBits,
                                      std::memory_order_relaxed);
        }
    };

    struct RenderResourceReservationDirectoryTestAccess
    {
        static void SetPendingGeneration(
            RenderResourceReservationDirectory& directory,
            size_t pendingIndex,
            uint32 generation) noexcept
        {
            directory.m_pendingReleases[pendingIndex].generation = generation;
        }
    };

namespace
{
    static_assert(std::is_same_v<
                  std::underlying_type_t<RenderResourcePublicState>, uint8>);
    static_assert(static_cast<uint8>(RenderResourcePublicState::Released) == 0);
    static_assert(static_cast<uint8>(RenderResourcePublicState::Reserved) == 1);
    static_assert(static_cast<uint8>(RenderResourcePublicState::UploadQueued) == 2);
    static_assert(static_cast<uint8>(RenderResourcePublicState::Uploading) == 3);
    static_assert(static_cast<uint8>(RenderResourcePublicState::GPUReady) == 4);
    static_assert(static_cast<uint8>(RenderResourcePublicState::Failed) == 5);
    static_assert(static_cast<uint8>(RenderResourcePublicState::Evicting) == 6);

    static_assert(std::is_same_v<
                  std::underlying_type_t<RenderResourceFailureCode>, uint16>);
    static_assert(static_cast<uint16>(RenderResourceFailureCode::None) == 0);
    static_assert(static_cast<uint16>(RenderResourceFailureCode::InvalidPayload) == 1);
    static_assert(static_cast<uint16>(RenderResourceFailureCode::DependencyUnavailable) == 2);
    static_assert(static_cast<uint16>(RenderResourceFailureCode::ResourceCreationFailed) == 3);
    static_assert(static_cast<uint16>(RenderResourceFailureCode::UploadSubmissionFailed) == 4);
    static_assert(static_cast<uint16>(RenderResourceFailureCode::DeviceLost) == 5);
    static_assert(static_cast<uint16>(RenderResourceFailureCode::Cancelled) == 6);
    static_assert(static_cast<uint16>(RenderResourceFailureCode::RuntimeFailure) == 7);

    static_assert(std::is_same_v<
                  std::underlying_type_t<RenderResourceReserveCode>, uint8>);
    static_assert(static_cast<uint8>(RenderResourceReserveCode::Reserved) == 0);
    static_assert(static_cast<uint8>(RenderResourceReserveCode::Existing) == 1);
    static_assert(static_cast<uint8>(RenderResourceReserveCode::InvalidAsset) == 2);
    static_assert(static_cast<uint8>(RenderResourceReserveCode::KindMismatch) == 3);
    static_assert(static_cast<uint8>(RenderResourceReserveCode::CapacityExceeded) == 4);
    static_assert(static_cast<uint8>(RenderResourceReserveCode::ShuttingDown) == 5);

    static_assert(std::is_same_v<
                  std::underlying_type_t<RenderUploadEnqueueCode>, uint8>);
    static_assert(static_cast<uint8>(RenderUploadEnqueueCode::Accepted) == 0);
    static_assert(static_cast<uint8>(RenderUploadEnqueueCode::QueueFullByCount) == 1);
    static_assert(static_cast<uint8>(RenderUploadEnqueueCode::QueueFullByBytes) == 2);
    static_assert(static_cast<uint8>(RenderUploadEnqueueCode::InvalidRequest) == 3);
    static_assert(static_cast<uint8>(RenderUploadEnqueueCode::StaleGeneration) == 4);
    static_assert(static_cast<uint8>(RenderUploadEnqueueCode::Cancelled) == 5);
    static_assert(static_cast<uint8>(RenderUploadEnqueueCode::ShuttingDown) == 6);

    static_assert(std::is_same_v<
                  std::underlying_type_t<RenderReleaseCode>, uint8>);
    static_assert(static_cast<uint8>(RenderReleaseCode::Accepted) == 0);
    static_assert(static_cast<uint8>(RenderReleaseCode::StaleGeneration) == 1);
    static_assert(static_cast<uint8>(RenderReleaseCode::AlreadyPending) == 2);
    static_assert(static_cast<uint8>(RenderReleaseCode::ShuttingDown) == 3);

    static_assert(std::is_same_v<
                  std::underlying_type_t<RenderResourceStatusCode>, uint8>);
    static_assert(static_cast<uint8>(RenderResourceStatusCode::Current) == 0);
    static_assert(static_cast<uint8>(RenderResourceStatusCode::InvalidHandle) == 1);
    static_assert(static_cast<uint8>(RenderResourceStatusCode::StaleGeneration) == 2);

    static_assert(std::is_same_v<
                  std::underlying_type_t<RenderStatusWriter>, uint8>);
    static_assert(static_cast<uint8>(RenderStatusWriter::Update) == 0);
    static_assert(static_cast<uint8>(RenderStatusWriter::Render) == 1);

    static_assert(noexcept(std::declval<IRenderResourceGateway&>().ReserveResource(
        AssetId{}, RenderResourceKind::Mesh)));
    static_assert(noexcept(std::declval<IRenderResourceGateway&>().TryEnqueueUpload(
        std::declval<const ResourceUploadRequestRef&>())));
    static_assert(noexcept(std::declval<IRenderResourceGateway&>().RequestRelease(
        RenderResourceHandle{})));
    static_assert(noexcept(std::declval<const IRenderResourceGateway&>()
                               .QueryResourceStatus(RenderResourceHandle{})));

    constexpr uint32 RVX_TEST_CAPACITY = 1024;

    PackedRenderResourceStatus MakePacked(
        uint32 generation,
        RenderResourcePublicState state,
        RenderResourceFailureCode failure = RenderResourceFailureCode::None)
    {
        return PackedRenderResourceStatus{generation, state, failure};
    }

    PackedRenderResourceStatus AdvanceToState(
        RenderResourceStatusTable& table,
        RenderResourcePublicState target)
    {
        const RenderResourceHandle unassigned{1, 0};
        EXPECT_TRUE(table.CompareExchange(
            unassigned,
            MakePacked(0, RenderResourcePublicState::Released),
            MakePacked(1, RenderResourcePublicState::Reserved),
            RenderStatusWriter::Update));

        if (target == RenderResourcePublicState::Reserved)
        {
            return MakePacked(1, target);
        }

        const RenderResourceHandle handle{1, 1};
        if (target == RenderResourcePublicState::Evicting ||
            target == RenderResourcePublicState::Released)
        {
            EXPECT_TRUE(table.CompareExchange(
                handle,
                MakePacked(1, RenderResourcePublicState::Reserved),
                MakePacked(1, RenderResourcePublicState::Evicting),
                RenderStatusWriter::Update));
            if (target == RenderResourcePublicState::Evicting)
            {
                return MakePacked(1, target);
            }

            EXPECT_TRUE(table.CompareExchange(
                handle,
                MakePacked(1, RenderResourcePublicState::Evicting),
                MakePacked(1, RenderResourcePublicState::Released),
                RenderStatusWriter::Render));
            return MakePacked(1, target);
        }

        EXPECT_TRUE(table.CompareExchange(
            handle,
            MakePacked(1, RenderResourcePublicState::Reserved),
            MakePacked(1, RenderResourcePublicState::UploadQueued),
            RenderStatusWriter::Update));
        if (target == RenderResourcePublicState::UploadQueued)
        {
            return MakePacked(1, target);
        }

        EXPECT_TRUE(table.CompareExchange(
            handle,
            MakePacked(1, RenderResourcePublicState::UploadQueued),
            MakePacked(1, RenderResourcePublicState::Uploading),
            RenderStatusWriter::Render));
        if (target == RenderResourcePublicState::Uploading)
        {
            return MakePacked(1, target);
        }

        const RenderResourceFailureCode failure =
            target == RenderResourcePublicState::Failed
                ? RenderResourceFailureCode::RuntimeFailure
                : RenderResourceFailureCode::None;
        EXPECT_TRUE(table.CompareExchange(
            handle,
            MakePacked(1, RenderResourcePublicState::Uploading),
            MakePacked(1, target, failure),
            RenderStatusWriter::Render));
        return MakePacked(1, target, failure);
    }

    bool IsUpdateTransitionAllowed(RenderResourcePublicState from,
                                   RenderResourcePublicState to)
    {
        if (from == RenderResourcePublicState::Released)
        {
            return to == RenderResourcePublicState::Reserved;
        }
        if (from == RenderResourcePublicState::Reserved &&
            to == RenderResourcePublicState::UploadQueued)
        {
            return true;
        }
        const bool live = from == RenderResourcePublicState::Reserved ||
                          from == RenderResourcePublicState::UploadQueued ||
                          from == RenderResourcePublicState::Uploading ||
                          from == RenderResourcePublicState::GPUReady ||
                          from == RenderResourcePublicState::Failed;
        return live && to == RenderResourcePublicState::Evicting;
    }

    bool IsRenderTransitionAllowed(RenderResourcePublicState from,
                                   RenderResourcePublicState to)
    {
        return (from == RenderResourcePublicState::UploadQueued &&
                to == RenderResourcePublicState::Uploading) ||
               (from == RenderResourcePublicState::Uploading &&
                (to == RenderResourcePublicState::GPUReady ||
                 to == RenderResourcePublicState::Failed)) ||
               (from == RenderResourcePublicState::Evicting &&
                to == RenderResourcePublicState::Released);
    }

    TEST(RenderConcurrencyValidation, DefaultResultsAreNarrowFailures)
    {
        const RenderResourceReserveResult reserveResult;
        const RenderUploadEnqueueResult uploadResult;
        const RenderReleaseResult releaseResult;
        const RenderResourceStatus status;

        EXPECT_EQ(reserveResult.code, RenderResourceReserveCode::InvalidAsset);
        EXPECT_FALSE(reserveResult.handle.IsValid());
        EXPECT_EQ(reserveResult.status.code,
                  RenderResourceStatusCode::InvalidHandle);
        EXPECT_EQ(uploadResult.code, RenderUploadEnqueueCode::InvalidRequest);
        EXPECT_EQ(releaseResult.code, RenderReleaseCode::StaleGeneration);
        EXPECT_EQ(status.code, RenderResourceStatusCode::InvalidHandle);
        EXPECT_EQ(status.state, RenderResourcePublicState::Released);
        EXPECT_EQ(status.failure, RenderResourceFailureCode::None);
    }

    TEST(RenderConcurrencyValidation, RejectsCapacityBelowMinimum)
    {
        EXPECT_THROW(RenderResourceStatusTable(RVX_TEST_CAPACITY - 1),
                     std::invalid_argument);
        EXPECT_NO_THROW(RenderResourceStatusTable{RVX_TEST_CAPACITY});
    }

    TEST(RenderConcurrencyValidation, SlotZeroAndZeroGenerationAreInvalid)
    {
        RenderResourceStatusTable table(RVX_TEST_CAPACITY);

        EXPECT_EQ(table.Query(RenderResourceHandle{0, 1}).code,
                  RenderResourceStatusCode::InvalidHandle);
        EXPECT_EQ(table.Query(RenderResourceHandle{1, 0}).code,
                  RenderResourceStatusCode::InvalidHandle);
        EXPECT_FALSE(table.CompareExchange(
            RenderResourceHandle{0, 0},
            MakePacked(0, RenderResourcePublicState::Released),
            MakePacked(1, RenderResourcePublicState::Reserved),
            RenderStatusWriter::Update));
    }

    TEST(RenderConcurrencyValidation, FirstReservationUsesGenerationOne)
    {
        RenderResourceStatusTable table(RVX_TEST_CAPACITY);
        RenderResourceReservationDirectory directory(table);

        const auto result =
            directory.ReserveResource(AssetId{11}, RenderResourceKind::Mesh);

        ASSERT_EQ(result.code, RenderResourceReserveCode::Reserved);
        EXPECT_EQ(result.handle, (RenderResourceHandle{1, 1}));
        EXPECT_EQ(result.status.code, RenderResourceStatusCode::Current);
        EXPECT_EQ(result.status.state, RenderResourcePublicState::Reserved);
        EXPECT_EQ(result.status.failure, RenderResourceFailureCode::None);
    }

    TEST(RenderConcurrencyValidation, PackedWordLayoutAndAcquireQueryAreExact)
    {
        RenderResourceStatusTable table(RVX_TEST_CAPACITY);
        const auto desired = MakePacked(1,
                                        RenderResourcePublicState::Reserved,
                                        RenderResourceFailureCode::DeviceLost);
        ASSERT_TRUE(table.CompareExchange(
            RenderResourceHandle{1, 0},
            MakePacked(0, RenderResourcePublicState::Released),
            desired,
            RenderStatusWriter::Update));

        const uint64 expectedWord =
            (uint64{1} << 32U) |
            (static_cast<uint64>(RenderResourceFailureCode::DeviceLost) << 16U) |
            static_cast<uint64>(RenderResourcePublicState::Reserved);
        EXPECT_EQ(RenderResourceStatusTableTestAccess::Load(table, 1),
                  expectedWord);

        const RenderResourceStatus status =
            table.Query(RenderResourceHandle{1, 1});
        EXPECT_EQ(status.code, RenderResourceStatusCode::Current);
        EXPECT_EQ(status.state, RenderResourcePublicState::Reserved);
        EXPECT_EQ(status.failure, RenderResourceFailureCode::DeviceLost);
    }

    TEST(RenderConcurrencyValidation, RejectsNonzeroReservedPackedBits)
    {
        RenderResourceStatusTable table(RVX_TEST_CAPACITY);
        RenderResourceStatusTableTestAccess::Store(
            table,
            1,
            MakePacked(1, RenderResourcePublicState::GPUReady),
            uint64{1} << 8U);

        const RenderResourceStatus status =
            table.Query(RenderResourceHandle{1, 1});
        EXPECT_EQ(status.code, RenderResourceStatusCode::InvalidHandle);
        EXPECT_EQ(status.state, RenderResourcePublicState::Released);
        EXPECT_EQ(status.failure, RenderResourceFailureCode::None);
    }

    TEST(RenderConcurrencyValidation, StaleQueryDoesNotDiscloseNewerState)
    {
        RenderResourceStatusTable table(RVX_TEST_CAPACITY);
        RenderResourceReservationDirectory directory(table);
        const auto first =
            directory.ReserveResource(AssetId{21}, RenderResourceKind::Texture);
        ASSERT_EQ(first.code, RenderResourceReserveCode::Reserved);
        ASSERT_EQ(directory.RequestRelease(first.handle).code,
                  RenderReleaseCode::Accepted);
        ASSERT_TRUE(table.CompareExchange(
            first.handle,
            MakePacked(1, RenderResourcePublicState::Evicting),
            MakePacked(1, RenderResourcePublicState::Released),
            RenderStatusWriter::Render));
        const auto second =
            directory.ReserveResource(AssetId{22}, RenderResourceKind::Texture);
        ASSERT_EQ(second.code, RenderResourceReserveCode::Reserved);
        ASSERT_EQ(second.handle, (RenderResourceHandle{1, 2}));

        const RenderResourceStatus stale = table.Query(first.handle);
        EXPECT_EQ(stale.code, RenderResourceStatusCode::StaleGeneration);
        EXPECT_EQ(stale.state, RenderResourcePublicState::Released);
        EXPECT_EQ(stale.failure, RenderResourceFailureCode::None);
    }

    TEST(RenderConcurrencyValidation, ExistingReturnsMatchingHandleAndObservedStatus)
    {
        RenderResourceStatusTable table(RVX_TEST_CAPACITY);
        RenderResourceReservationDirectory directory(table);
        const auto first =
            directory.ReserveResource(AssetId{31}, RenderResourceKind::Material);
        ASSERT_EQ(first.code, RenderResourceReserveCode::Reserved);
        ASSERT_TRUE(table.CompareExchange(
            first.handle,
            MakePacked(1, RenderResourcePublicState::Reserved),
            MakePacked(1, RenderResourcePublicState::UploadQueued),
            RenderStatusWriter::Update));

        const auto existing =
            directory.ReserveResource(AssetId{31}, RenderResourceKind::Material);

        EXPECT_EQ(existing.code, RenderResourceReserveCode::Existing);
        EXPECT_EQ(existing.handle, first.handle);
        EXPECT_EQ(existing.status.code, RenderResourceStatusCode::Current);
        EXPECT_EQ(existing.status.state,
                  RenderResourcePublicState::UploadQueued);
        EXPECT_EQ(existing.status.failure, RenderResourceFailureCode::None);
    }

    TEST(RenderConcurrencyValidation, ReserveFailuresNeverReturnHandles)
    {
        RenderResourceStatusTable table(RVX_TEST_CAPACITY);
        RenderResourceReservationDirectory directory(table);

        const auto invalid =
            directory.ReserveResource(AssetId{}, RenderResourceKind::Mesh);
        EXPECT_EQ(invalid.code, RenderResourceReserveCode::InvalidAsset);
        EXPECT_FALSE(invalid.handle.IsValid());

        const auto first =
            directory.ReserveResource(AssetId{41}, RenderResourceKind::Mesh);
        ASSERT_EQ(first.code, RenderResourceReserveCode::Reserved);
        const auto mismatch =
            directory.ReserveResource(AssetId{41}, RenderResourceKind::Texture);
        EXPECT_EQ(mismatch.code, RenderResourceReserveCode::KindMismatch);
        EXPECT_FALSE(mismatch.handle.IsValid());

        for (uint32 value = 42; value < RVX_TEST_CAPACITY + 40; ++value)
        {
            const auto result = directory.ReserveResource(
                AssetId{value}, RenderResourceKind::Mesh);
            ASSERT_EQ(result.code, RenderResourceReserveCode::Reserved);
        }
        const auto full = directory.ReserveResource(
            AssetId{RVX_TEST_CAPACITY + 40}, RenderResourceKind::Mesh);
        EXPECT_EQ(full.code, RenderResourceReserveCode::CapacityExceeded);
        EXPECT_FALSE(full.handle.IsValid());
    }

    TEST(RenderConcurrencyValidation, UpdateWriterAllowsOnlyDeclaredTransitions)
    {
        const std::vector<RenderResourcePublicState> states{
            RenderResourcePublicState::Released,
            RenderResourcePublicState::Reserved,
            RenderResourcePublicState::UploadQueued,
            RenderResourcePublicState::Uploading,
            RenderResourcePublicState::GPUReady,
            RenderResourcePublicState::Failed,
            RenderResourcePublicState::Evicting};

        for (const auto from : states)
        {
            for (const auto to : states)
            {
                RenderResourceStatusTable table(RVX_TEST_CAPACITY);
                const PackedRenderResourceStatus expected =
                    AdvanceToState(table, from);
                const uint32 desiredGeneration =
                    from == RenderResourcePublicState::Released &&
                            to == RenderResourcePublicState::Reserved
                        ? expected.generation + 1
                        : expected.generation;
                const RenderResourceFailureCode desiredFailure =
                    to == RenderResourcePublicState::Failed
                        ? RenderResourceFailureCode::RuntimeFailure
                        : RenderResourceFailureCode::None;

                const bool changed = table.CompareExchange(
                    RenderResourceHandle{1, expected.generation},
                    expected,
                    MakePacked(desiredGeneration, to, desiredFailure),
                    RenderStatusWriter::Update);
                EXPECT_EQ(changed, IsUpdateTransitionAllowed(from, to))
                    << "from=" << static_cast<uint32>(from)
                    << " to=" << static_cast<uint32>(to);
            }
        }
    }

    TEST(RenderConcurrencyValidation, RenderWriterAllowsOnlyDeclaredTransitions)
    {
        const std::vector<RenderResourcePublicState> states{
            RenderResourcePublicState::Released,
            RenderResourcePublicState::Reserved,
            RenderResourcePublicState::UploadQueued,
            RenderResourcePublicState::Uploading,
            RenderResourcePublicState::GPUReady,
            RenderResourcePublicState::Failed,
            RenderResourcePublicState::Evicting};

        for (const auto from : states)
        {
            for (const auto to : states)
            {
                RenderResourceStatusTable table(RVX_TEST_CAPACITY);
                const PackedRenderResourceStatus expected =
                    AdvanceToState(table, from);
                const RenderResourceFailureCode desiredFailure =
                    to == RenderResourcePublicState::Failed
                        ? RenderResourceFailureCode::RuntimeFailure
                        : RenderResourceFailureCode::None;

                const bool changed = table.CompareExchange(
                    RenderResourceHandle{1, expected.generation},
                    expected,
                    MakePacked(expected.generation, to, desiredFailure),
                    RenderStatusWriter::Render);
                EXPECT_EQ(changed, IsRenderTransitionAllowed(from, to))
                    << "from=" << static_cast<uint32>(from)
                    << " to=" << static_cast<uint32>(to);
            }
        }
    }

    TEST(RenderConcurrencyValidation, RejectsUndeclaredWriterValue)
    {
        RenderResourceStatusTable table(RVX_TEST_CAPACITY);

        EXPECT_FALSE(table.CompareExchange(
            RenderResourceHandle{1, 0},
            MakePacked(0, RenderResourcePublicState::Released),
            MakePacked(1, RenderResourcePublicState::Reserved),
            static_cast<RenderStatusWriter>(2)));
        EXPECT_EQ(table.Query(RenderResourceHandle{1, 1}).code,
                  RenderResourceStatusCode::StaleGeneration);
    }

    TEST(RenderConcurrencyValidation, StaleGenerationCannotMutateNewReservation)
    {
        RenderResourceStatusTable table(RVX_TEST_CAPACITY);
        RenderResourceReservationDirectory directory(table);
        const auto first =
            directory.ReserveResource(AssetId{51}, RenderResourceKind::Mesh);
        ASSERT_EQ(directory.RequestRelease(first.handle).code,
                  RenderReleaseCode::Accepted);
        ASSERT_TRUE(table.CompareExchange(
            first.handle,
            MakePacked(1, RenderResourcePublicState::Evicting),
            MakePacked(1, RenderResourcePublicState::Released),
            RenderStatusWriter::Render));
        const auto second =
            directory.ReserveResource(AssetId{52}, RenderResourceKind::Mesh);
        ASSERT_EQ(second.handle, (RenderResourceHandle{1, 2}));

        EXPECT_FALSE(table.CompareExchange(
            first.handle,
            MakePacked(1, RenderResourcePublicState::Released),
            MakePacked(2, RenderResourcePublicState::Reserved),
            RenderStatusWriter::Update));
        EXPECT_EQ(table.Query(second.handle).state,
                  RenderResourcePublicState::Reserved);
    }

    TEST(RenderConcurrencyValidation, ReleaseRemovalAllowsIndependentReplacement)
    {
        RenderResourceStatusTable table(RVX_TEST_CAPACITY);
        RenderResourceReservationDirectory directory(table);
        const auto first =
            directory.ReserveResource(AssetId{61}, RenderResourceKind::Texture);
        ASSERT_EQ(first.handle, (RenderResourceHandle{1, 1}));
        ASSERT_EQ(directory.RequestRelease(first.handle).code,
                  RenderReleaseCode::Accepted);

        const auto replacementWhileEvicting =
            directory.ReserveResource(AssetId{62}, RenderResourceKind::Texture);
        ASSERT_EQ(replacementWhileEvicting.code,
                  RenderResourceReserveCode::Reserved);
        EXPECT_EQ(replacementWhileEvicting.handle,
                  (RenderResourceHandle{2, 1}));

        ASSERT_TRUE(table.CompareExchange(
            first.handle,
            MakePacked(1, RenderResourcePublicState::Evicting),
            MakePacked(1, RenderResourcePublicState::Released),
            RenderStatusWriter::Render));
        const auto reused =
            directory.ReserveResource(AssetId{63}, RenderResourceKind::Texture);
        EXPECT_EQ(reused.code, RenderResourceReserveCode::Reserved);
        EXPECT_EQ(reused.handle, (RenderResourceHandle{1, 2}));
    }

    TEST(RenderConcurrencyValidation, MaximumGenerationRetiresSlot)
    {
        RenderResourceStatusTable table(RVX_TEST_CAPACITY);
        RenderResourceReservationDirectory directory(table);
        const auto first =
            directory.ReserveResource(AssetId{71}, RenderResourceKind::Material);
        ASSERT_EQ(directory.RequestRelease(first.handle).code,
                  RenderReleaseCode::Accepted);

        RenderResourceStatusTableTestAccess::Store(
            table,
            first.handle.slot,
            MakePacked(std::numeric_limits<uint32>::max(),
                       RenderResourcePublicState::Released));
        RenderResourceReservationDirectoryTestAccess::SetPendingGeneration(
            directory, 0, std::numeric_limits<uint32>::max());

        uint32 reservedCount = 0;
        for (uint32 value = 100; value < RVX_TEST_CAPACITY + 100; ++value)
        {
            const auto result = directory.ReserveResource(
                AssetId{value}, RenderResourceKind::Material);
            if (result.code == RenderResourceReserveCode::CapacityExceeded)
            {
                EXPECT_FALSE(result.handle.IsValid());
                break;
            }
            ASSERT_EQ(result.code, RenderResourceReserveCode::Reserved);
            ASSERT_NE(result.handle.slot, first.handle.slot);
            ++reservedCount;
        }

        EXPECT_EQ(reservedCount, RVX_TEST_CAPACITY - 2);
        const auto stillFull = directory.ReserveResource(
            AssetId{RVX_TEST_CAPACITY + 101}, RenderResourceKind::Material);
        EXPECT_EQ(stillFull.code, RenderResourceReserveCode::CapacityExceeded);
        EXPECT_FALSE(stillFull.handle.IsValid());
    }

    TEST(RenderConcurrencyValidation, AcquireReleasedPublishesTerminalPayload)
    {
        RenderResourceStatusTable table(RVX_TEST_CAPACITY);
        const PackedRenderResourceStatus evicting =
            AdvanceToState(table, RenderResourcePublicState::Evicting);
        const RenderResourceHandle handle{1, evicting.generation};
        std::latch start(2);
        uint32 terminalPayload = 0;
        std::atomic<bool> transitioned = false;
        std::atomic<bool> observedReleased = false;
        uint32 observedPayload = 0;

        std::thread writer([&]() {
            start.arrive_and_wait();
            terminalPayload = 0xC0FFEEU;
            transitioned.store(
                table.CompareExchange(
                    handle,
                    evicting,
                    MakePacked(evicting.generation,
                               RenderResourcePublicState::Released),
                    RenderStatusWriter::Render),
                std::memory_order_relaxed);
        });
        std::thread reader([&]() {
            start.arrive_and_wait();
            for (;;)
            {
                const RenderResourceStatus status = table.Query(handle);
                if (status.code == RenderResourceStatusCode::Current &&
                    status.state == RenderResourcePublicState::Released)
                {
                    observedPayload = terminalPayload;
                    observedReleased.store(true, std::memory_order_relaxed);
                    break;
                }
                std::this_thread::yield();
            }
        });

        writer.join();
        reader.join();

        EXPECT_TRUE(transitioned.load(std::memory_order_relaxed));
        EXPECT_TRUE(observedReleased.load(std::memory_order_relaxed));
        EXPECT_EQ(observedPayload, 0xC0FFEEU);
    }
} // namespace
} // namespace RVX
