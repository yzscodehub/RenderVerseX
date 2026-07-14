#include "Render/RenderTransportTypes.h"
#include "RenderContracts/IRenderResourceGateway.h"
#include "Runtime/RenderControlMailbox.h"
#include "Runtime/RenderFrameMailbox.h"
#include "Runtime/RenderReleaseQueue.h"
#include "Runtime/RenderResourceGateway.h"
#include "Runtime/RenderResourceReservationDirectory.h"
#include "Runtime/RenderResourceStatusTable.h"
#include "Runtime/RenderUploadQueue.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <latch>
#include <limits>
#include <memory>
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

    struct RenderReleaseQueueTestAccess
    {
        static void ForcePublicationInvariantFailure(
            RenderReleaseQueue& queue) noexcept
        {
            queue.m_count = queue.m_usableCapacity;
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

    void CountWake(void* context) noexcept
    {
        static_cast<std::atomic<uint32>*>(context)->fetch_add(
            1, std::memory_order_relaxed);
    }

    void CountFatal(void* context, const char*) noexcept
    {
        static_cast<std::atomic<uint32>*>(context)->fetch_add(
            1, std::memory_order_relaxed);
    }

    struct DestructionProbe
    {
        std::atomic<bool> armed = false;
        std::latch entered{1};
        std::latch release{1};
    };

    class TestFramePacket final
    {
    public:
        explicit TestFramePacket(uint64 sequence,
                                 DestructionProbe* destructionProbe = nullptr)
            : m_destructionProbe(destructionProbe)
        {
            m_header.sequence = sequence;
            m_features.BeginBuild(sequence);
            m_features.MarkComplete();
            m_diagnostics.complete = true;
        }

        ~TestFramePacket()
        {
            if (m_destructionProbe != nullptr &&
                m_destructionProbe->armed.load(std::memory_order_acquire))
            {
                m_destructionProbe->entered.count_down();
                m_destructionProbe->release.wait();
            }
        }

        TestFramePacket(const TestFramePacket&) = delete;
        TestFramePacket& operator=(const TestFramePacket&) = delete;

        [[nodiscard]] const RenderFrameHeader& GetHeader() const noexcept
        {
            return m_header;
        }

        [[nodiscard]] const std::vector<RenderPrimitiveSnapshot>&
            GetPrimitives() const noexcept
        {
            return m_primitives;
        }

        [[nodiscard]] const std::vector<RenderLightSnapshot>&
            GetLights() const noexcept
        {
            return m_lights;
        }

        [[nodiscard]] const RenderFeatureSnapshot& GetFeatures() const noexcept
        {
            return m_features;
        }

        [[nodiscard]] const RenderExtractionDiagnostics&
            GetExtractionDiagnostics() const noexcept
        {
            return m_diagnostics;
        }

        void SetComplete(bool complete) noexcept
        {
            m_diagnostics.complete = complete;
        }

    private:
        RenderFrameHeader m_header{};
        std::vector<RenderPrimitiveSnapshot> m_primitives{};
        std::vector<RenderLightSnapshot> m_lights{};
        RenderFeatureSnapshot m_features{};
        RenderExtractionDiagnostics m_diagnostics{};
        DestructionProbe* m_destructionProbe = nullptr;
    };

    using TestFrameMailbox = BasicRenderFrameMailbox<
        TestFramePacket,
        CompleteRenderFramePacketValidator<TestFramePacket>>;

    std::unique_ptr<const TestFramePacket> MakeTestFrame(
        uint64 sequence,
        DestructionProbe* destructionProbe = nullptr)
    {
        return std::make_unique<const TestFramePacket>(sequence,
                                                       destructionProbe);
    }

    ResourceUploadRequestRef MakeMeshUploadRequest(
        RenderResourceHandle handle,
        AssetId assetId,
        uint64 sequence,
        uint64 byteCount = 36)
    {
        MeshUploadPayload payload;
        payload.createInfo.vertexCount = byteCount / 12U;
        payload.createInfo.boundsMin = Vec3(-1.0f);
        payload.createInfo.boundsMax = Vec3(1.0f);
        payload.bytes.resize(static_cast<size_t>(byteCount), 7U);
        payload.positionRange = UploadByteRange{0, byteCount, 12};

        ResourceUploadRequestCreateInfo info;
        info.sequence = sequence;
        info.assetId = assetId;
        info.handle = handle;
        info.kind = RenderResourceKind::Mesh;
        info.payload = std::move(payload);
        info.declaredPayloadBytes = byteCount;
        const ResourceUploadRequestCreateResult result =
            ResourceUploadRequest::Create(std::move(info));
        EXPECT_EQ(result.code, ResourceUploadRequestCreateCode::Created);
        return result.request;
    }

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

    TEST(RenderConcurrencyValidation, TransportConfigurationRejectsUnsafeValues)
    {
        RenderTransportConfig config;
        EXPECT_TRUE(config.IsValid());

        config.frameCapacity = 1;
        EXPECT_FALSE(config.IsValid());
        config.frameCapacity = 5;
        EXPECT_FALSE(config.IsValid());
        config.frameCapacity = 3;
        config.uploadRequestCapacity = 0;
        EXPECT_FALSE(config.IsValid());
        config.uploadRequestCapacity = 1;
        config.uploadByteCapacity = 0;
        EXPECT_FALSE(config.IsValid());
        config.uploadByteCapacity = 1;
        config.statusSlotCapacity = RVX_TEST_CAPACITY - 1U;
        EXPECT_FALSE(config.IsValid());

        RenderIterationBudgets budgets;
        EXPECT_TRUE(budgets.IsValid());
        budgets.uploadRequestCount = 0;
        EXPECT_FALSE(budgets.IsValid());
        budgets.uploadRequestCount = 1;
        budgets.uploadBytes = 0;
        EXPECT_FALSE(budgets.IsValid());
        budgets.uploadBytes = 1;
        budgets.uploadTime = std::chrono::milliseconds::zero();
        EXPECT_FALSE(budgets.IsValid());
        budgets.uploadTime = std::chrono::milliseconds{1};
        budgets.releaseCount = 0;
        EXPECT_FALSE(budgets.IsValid());
        budgets.releaseCount = 1;
        budgets.releaseTime = std::chrono::milliseconds::zero();
        EXPECT_FALSE(budgets.IsValid());
    }

    class RenderFrameMailboxCapacityTest :
        public testing::TestWithParam<uint32>
    {
    };

    TEST_P(RenderFrameMailboxCapacityTest,
           FullMailboxReplacesOldestAndConsumerAcquiresLatest)
    {
        const uint32 capacity = GetParam();
        std::atomic<uint32> wakeCount = 0;
        TestFrameMailbox mailbox(capacity, &CountWake, &wakeCount);

        for (uint64 sequence = 1; sequence <= capacity; ++sequence)
        {
            const auto result = mailbox.TryPublish(MakeTestFrame(sequence));
            EXPECT_EQ(result.code, RenderFrameMailboxPublishCode::Accepted);
            EXPECT_EQ(result.replacedSequence, 0);
        }

        const auto replacement =
            mailbox.TryPublish(MakeTestFrame(capacity + 1U));
        EXPECT_EQ(replacement.code,
                  RenderFrameMailboxPublishCode::ReplacedOldest);
        EXPECT_EQ(replacement.replacedSequence, 1);
        EXPECT_EQ(mailbox.GetPendingCount(), capacity);

        auto acquired = mailbox.AcquireLatest();
        ASSERT_NE(acquired.packet, nullptr);
        EXPECT_EQ(acquired.packet->GetHeader().sequence, capacity + 1U);
        ASSERT_EQ(acquired.discardedCount, capacity - 1U);
        for (uint32 index = 0; index < acquired.discardedCount; ++index)
        {
            EXPECT_EQ(acquired.discardedSequences[index], index + 2U);
        }
        EXPECT_EQ(mailbox.GetPendingCount(), 0U);
        EXPECT_EQ(wakeCount.load(std::memory_order_relaxed), capacity + 1U);
    }

    INSTANTIATE_TEST_SUITE_P(RenderConcurrencyValidationCapacitiesTwoThroughFour,
                             RenderFrameMailboxCapacityTest,
                             testing::Values(2U, 3U, 4U));

    TEST(RenderConcurrencyValidation,
         FrameMailboxRejectsInvalidAndOutOfOrderPacketsWithoutWake)
    {
        std::atomic<uint32> wakeCount = 0;
        TestFrameMailbox mailbox(3, &CountWake, &wakeCount);
        auto incomplete = std::make_unique<TestFramePacket>(1);
        incomplete->SetComplete(false);

        EXPECT_EQ(mailbox.TryPublish(std::move(incomplete)).code,
                  RenderFrameMailboxPublishCode::InvalidPacket);
        EXPECT_EQ(mailbox.TryPublish(MakeTestFrame(2)).code,
                  RenderFrameMailboxPublishCode::Accepted);
        EXPECT_EQ(mailbox.TryPublish(MakeTestFrame(1)).code,
                  RenderFrameMailboxPublishCode::OutOfOrder);
        EXPECT_EQ(mailbox.TryPublish(nullptr).code,
                  RenderFrameMailboxPublishCode::InvalidPacket);
        EXPECT_EQ(mailbox.GetPendingCount(), 1U);
        EXPECT_EQ(wakeCount.load(std::memory_order_relaxed), 1U);
    }

    TEST(RenderConcurrencyValidation,
         AcquiredFrameOwnershipIsIndependentOfLaterReplacement)
    {
        TestFrameMailbox mailbox(2);
        ASSERT_EQ(mailbox.TryPublish(MakeTestFrame(1)).code,
                  RenderFrameMailboxPublishCode::Accepted);
        auto acquired = mailbox.AcquireLatest();
        ASSERT_NE(acquired.packet, nullptr);

        ASSERT_EQ(mailbox.TryPublish(MakeTestFrame(2)).code,
                  RenderFrameMailboxPublishCode::Accepted);
        ASSERT_EQ(mailbox.TryPublish(MakeTestFrame(3)).code,
                  RenderFrameMailboxPublishCode::Accepted);
        ASSERT_EQ(mailbox.TryPublish(MakeTestFrame(4)).code,
                  RenderFrameMailboxPublishCode::ReplacedOldest);

        EXPECT_EQ(acquired.packet->GetHeader().sequence, 1U);
    }

    TEST(RenderConcurrencyValidation,
         FrameReplacementDestroysOldestOwnershipOutsideMailboxLock)
    {
        TestFrameMailbox mailbox(2);
        DestructionProbe probe;
        ASSERT_EQ(mailbox.TryPublish(MakeTestFrame(1, &probe)).code,
                  RenderFrameMailboxPublishCode::Accepted);
        ASSERT_EQ(mailbox.TryPublish(MakeTestFrame(2)).code,
                  RenderFrameMailboxPublishCode::Accepted);
        probe.armed.store(true, std::memory_order_release);

        std::thread publisher([&]() {
            const auto result = mailbox.TryPublish(MakeTestFrame(3));
            EXPECT_EQ(result.code,
                      RenderFrameMailboxPublishCode::ReplacedOldest);
        });
        probe.entered.wait();

        std::atomic<uint32> pendingCount = 0;
        std::latch queried{1};
        std::thread observer([&]() {
            pendingCount.store(mailbox.GetPendingCount(),
                               std::memory_order_relaxed);
            queried.count_down();
        });
        queried.wait();
        probe.release.count_down();

        observer.join();
        publisher.join();
        EXPECT_EQ(pendingCount.load(std::memory_order_relaxed), 2U);
    }

    TEST(RenderConcurrencyValidation,
         FrameCoalescingDestroysDiscardedOwnershipOutsideMailboxLock)
    {
        TestFrameMailbox mailbox(2);
        DestructionProbe probe;
        ASSERT_EQ(mailbox.TryPublish(MakeTestFrame(1, &probe)).code,
                  RenderFrameMailboxPublishCode::Accepted);
        ASSERT_EQ(mailbox.TryPublish(MakeTestFrame(2)).code,
                  RenderFrameMailboxPublishCode::Accepted);
        probe.armed.store(true, std::memory_order_release);

        std::thread consumer([&]() {
            auto acquired = mailbox.AcquireLatest();
            ASSERT_NE(acquired.packet, nullptr);
            EXPECT_EQ(acquired.packet->GetHeader().sequence, 2U);
        });
        probe.entered.wait();

        std::atomic<uint32> pendingCount = 1;
        std::latch queried{1};
        std::thread observer([&]() {
            pendingCount.store(mailbox.GetPendingCount(),
                               std::memory_order_relaxed);
            queried.count_down();
        });
        queried.wait();
        probe.release.count_down();

        observer.join();
        consumer.join();
        EXPECT_EQ(pendingCount.load(std::memory_order_relaxed), 0U);
    }

    TEST(RenderConcurrencyValidation,
         UploadCountPressureLeavesRejectedReservationUnchanged)
    {
        RenderTransportConfig config;
        config.uploadRequestCapacity = 1;
        config.uploadByteCapacity = 1024;
        std::atomic<uint32> wakeCount = 0;
        RenderResourceGateway gateway(config, &CountWake, &wakeCount);
        const auto first =
            gateway.ReserveResource(AssetId{1001}, RenderResourceKind::Mesh);
        const auto second =
            gateway.ReserveResource(AssetId{1002}, RenderResourceKind::Mesh);
        ASSERT_EQ(first.code, RenderResourceReserveCode::Reserved);
        ASSERT_EQ(second.code, RenderResourceReserveCode::Reserved);

        const auto firstRequest =
            MakeMeshUploadRequest(first.handle, AssetId{1001}, 1);
        const auto secondRequest =
            MakeMeshUploadRequest(second.handle, AssetId{1002}, 2);
        EXPECT_EQ(gateway.TryEnqueueUpload(firstRequest).code,
                  RenderUploadEnqueueCode::Accepted);
        EXPECT_EQ(gateway.TryEnqueueUpload(secondRequest).code,
                  RenderUploadEnqueueCode::QueueFullByCount);
        EXPECT_EQ(gateway.QueryResourceStatus(second.handle).state,
                  RenderResourcePublicState::Reserved);
        EXPECT_EQ(gateway.GetRetainedUploadCount(), 1U);
        EXPECT_EQ(gateway.GetRetainedUploadBytes(),
                  firstRequest->GetDerivedPayloadBytes());
        EXPECT_EQ(wakeCount.load(std::memory_order_relaxed), 1U);
    }

    TEST(RenderConcurrencyValidation,
         UploadDerivedBytePressureIsExactAndDequeueBalancesCounters)
    {
        RenderTransportConfig config;
        config.uploadRequestCapacity = 2;
        config.uploadByteCapacity = 36;
        std::atomic<uint32> wakeCount = 0;
        RenderResourceGateway gateway(config, &CountWake, &wakeCount);
        const auto first =
            gateway.ReserveResource(AssetId{1101}, RenderResourceKind::Mesh);
        const auto second =
            gateway.ReserveResource(AssetId{1102}, RenderResourceKind::Mesh);
        const auto firstRequest =
            MakeMeshUploadRequest(first.handle, AssetId{1101}, 1, 36);
        const auto secondRequest =
            MakeMeshUploadRequest(second.handle, AssetId{1102}, 2, 36);
        ASSERT_EQ(firstRequest->GetDerivedPayloadBytes(), 36U);

        EXPECT_EQ(gateway.TryEnqueueUpload(firstRequest).code,
                  RenderUploadEnqueueCode::Accepted);
        EXPECT_EQ(gateway.TryEnqueueUpload(secondRequest).code,
                  RenderUploadEnqueueCode::QueueFullByBytes);
        EXPECT_EQ(gateway.QueryResourceStatus(second.handle).state,
                  RenderResourcePublicState::Reserved);

        const ResourceUploadRequestRef dequeued = gateway.TryDequeueUpload();
        EXPECT_EQ(dequeued, firstRequest);
        EXPECT_EQ(gateway.GetRetainedUploadCount(), 0U);
        EXPECT_EQ(gateway.GetRetainedUploadBytes(), 0U);
        EXPECT_EQ(wakeCount.load(std::memory_order_relaxed), 1U);
    }

    TEST(RenderConcurrencyValidation,
         UploadGatewayMapsInvalidStaleAndCancelledRequestsExactly)
    {
        RenderTransportConfig config;
        RenderResourceGateway gateway(config);
        EXPECT_EQ(gateway.TryEnqueueUpload({}).code,
                  RenderUploadEnqueueCode::InvalidRequest);

        const auto cancelled =
            gateway.ReserveResource(AssetId{1201}, RenderResourceKind::Mesh);
        const auto cancelledRequest =
            MakeMeshUploadRequest(cancelled.handle, AssetId{1201}, 1);
        ASSERT_EQ(gateway.RequestRelease(cancelled.handle).code,
                  RenderReleaseCode::Accepted);
        EXPECT_EQ(gateway.TryEnqueueUpload(cancelledRequest).code,
                  RenderUploadEnqueueCode::Cancelled);

        const RenderResourceHandle released = gateway.TryDequeueRelease();
        ASSERT_EQ(released, cancelled.handle);
        ASSERT_TRUE(gateway.GetStatusTable().CompareExchange(
            released,
            MakePacked(released.generation,
                       RenderResourcePublicState::Evicting),
            MakePacked(released.generation,
                       RenderResourcePublicState::Released),
            RenderStatusWriter::Render));
        const auto replacement =
            gateway.ReserveResource(AssetId{1202}, RenderResourceKind::Mesh);
        ASSERT_EQ(replacement.handle.slot, cancelled.handle.slot);
        ASSERT_GT(replacement.handle.generation,
                  cancelled.handle.generation);
        EXPECT_EQ(gateway.TryEnqueueUpload(cancelledRequest).code,
                  RenderUploadEnqueueCode::StaleGeneration);
    }

    TEST(RenderConcurrencyValidation,
         ReleaseRingUsesEveryNonzeroStatusSlot)
    {
        RenderResourceStatusTable table(RVX_TEST_CAPACITY);
        RenderResourceReservationDirectory directory(table);
        std::atomic<uint32> wakeCount = 0;
        std::atomic<uint32> fatalCount = 0;
        RenderReleaseQueue queue(directory,
                                 table,
                                 RVX_TEST_CAPACITY,
                                 &CountWake,
                                 &wakeCount,
                                 &CountFatal,
                                 &fatalCount);

        EXPECT_EQ(queue.GetUsableCapacity(), RVX_TEST_CAPACITY - 1U);
        for (uint32 index = 1; index < RVX_TEST_CAPACITY; ++index)
        {
            const auto reserved = directory.ReserveResource(
                AssetId{2000U + index}, RenderResourceKind::Texture);
            ASSERT_EQ(reserved.code, RenderResourceReserveCode::Reserved);
            EXPECT_EQ(queue.RequestRelease(reserved.handle).code,
                      RenderReleaseCode::Accepted);
        }

        EXPECT_EQ(queue.GetPendingCount(), RVX_TEST_CAPACITY - 1U);
        EXPECT_EQ(wakeCount.load(std::memory_order_relaxed),
                  RVX_TEST_CAPACITY - 1U);
        EXPECT_EQ(fatalCount.load(std::memory_order_relaxed), 0U);
    }

    TEST(RenderConcurrencyValidation,
         ReleaseQueueReportsDuplicateAndStaleWithoutExtraWake)
    {
        RenderResourceStatusTable table(RVX_TEST_CAPACITY);
        RenderResourceReservationDirectory directory(table);
        std::atomic<uint32> wakeCount = 0;
        RenderReleaseQueue queue(directory,
                                 table,
                                 RVX_TEST_CAPACITY,
                                 &CountWake,
                                 &wakeCount);
        const auto first =
            directory.ReserveResource(AssetId{3101}, RenderResourceKind::Mesh);
        ASSERT_EQ(queue.RequestRelease(first.handle).code,
                  RenderReleaseCode::Accepted);
        EXPECT_EQ(queue.RequestRelease(first.handle).code,
                  RenderReleaseCode::AlreadyPending);

        const RenderResourceHandle released = queue.TryDequeue();
        ASSERT_EQ(released, first.handle);
        ASSERT_TRUE(table.CompareExchange(
            released,
            MakePacked(released.generation,
                       RenderResourcePublicState::Evicting),
            MakePacked(released.generation,
                       RenderResourcePublicState::Released),
            RenderStatusWriter::Render));
        const auto replacement =
            directory.ReserveResource(AssetId{3102}, RenderResourceKind::Mesh);
        ASSERT_EQ(replacement.handle.slot, first.handle.slot);
        EXPECT_EQ(queue.RequestRelease(first.handle).code,
                  RenderReleaseCode::StaleGeneration);
        EXPECT_EQ(wakeCount.load(std::memory_order_relaxed), 1U);
    }

    TEST(RenderConcurrencyValidation,
         ReleasePublicationInvariantFailureUsesRuntimeFatalSink)
    {
        RenderResourceStatusTable table(RVX_TEST_CAPACITY);
        RenderResourceReservationDirectory directory(table);
        std::atomic<uint32> wakeCount = 0;
        std::atomic<uint32> fatalCount = 0;
        RenderReleaseQueue queue(directory,
                                 table,
                                 RVX_TEST_CAPACITY,
                                 &CountWake,
                                 &wakeCount,
                                 &CountFatal,
                                 &fatalCount);
        const auto reserved =
            directory.ReserveResource(AssetId{3201}, RenderResourceKind::Mesh);
        RenderReleaseQueueTestAccess::ForcePublicationInvariantFailure(queue);

        EXPECT_EQ(queue.RequestRelease(reserved.handle).code,
                  RenderReleaseCode::Accepted);
        EXPECT_EQ(table.Query(reserved.handle).state,
                  RenderResourcePublicState::Evicting);
        EXPECT_EQ(fatalCount.load(std::memory_order_relaxed), 1U);
        EXPECT_EQ(wakeCount.load(std::memory_order_relaxed), 0U);
    }

    TEST(RenderConcurrencyValidation,
         ReleaseQueueRejectsCapacityThatDoesNotMatchStatusTable)
    {
        RenderResourceStatusTable table(RVX_TEST_CAPACITY);
        RenderResourceReservationDirectory directory(table);

        EXPECT_THROW(RenderReleaseQueue(directory, table, 0),
                     std::invalid_argument);
        EXPECT_THROW(RenderReleaseQueue(directory,
                                        table,
                                        RVX_TEST_CAPACITY - 1U),
                     std::invalid_argument);
    }

    TEST(RenderConcurrencyValidation,
         ControlMailboxCoalescesOwnedGenerationValues)
    {
        std::atomic<uint32> wakeCount = 0;
        BasicRenderControlMailbox<std::unique_ptr<uint32>,
                                  std::unique_ptr<uint32>>
            mailbox(&CountWake, &wakeCount);

        EXPECT_TRUE(mailbox.TryPublishSurface(
            1, std::make_unique<uint32>(11)));
        EXPECT_TRUE(mailbox.TryPublishSurface(
            3, std::make_unique<uint32>(33)));
        EXPECT_FALSE(mailbox.TryPublishSurface(
            2, std::make_unique<uint32>(22)));
        EXPECT_TRUE(mailbox.TryPublishResize(
            4, std::make_unique<uint32>(44)));
        EXPECT_TRUE(mailbox.TryPublishResize(
            5, std::make_unique<uint32>(55)));

        auto controls = mailbox.AcquireLatest();
        EXPECT_FALSE(controls.stopRequested);
        ASSERT_TRUE(controls.surface.has_value());
        EXPECT_EQ(controls.surface->generation, 3U);
        ASSERT_NE(controls.surface->value, nullptr);
        EXPECT_EQ(*controls.surface->value, 33U);
        ASSERT_TRUE(controls.resize.has_value());
        EXPECT_EQ(controls.resize->generation, 5U);
        ASSERT_NE(controls.resize->value, nullptr);
        EXPECT_EQ(*controls.resize->value, 55U);
        EXPECT_EQ(wakeCount.load(std::memory_order_relaxed), 4U);
    }

    TEST(RenderConcurrencyValidation,
         ControlStopFlagPreemptsQueuedSurfaceAndResizeValues)
    {
        std::atomic<uint32> wakeCount = 0;
        BasicRenderControlMailbox<uint64, uint64> mailbox(&CountWake,
                                                          &wakeCount);
        ASSERT_TRUE(mailbox.TryPublishSurface(1, 101));
        ASSERT_TRUE(mailbox.TryPublishResize(1, 202));
        mailbox.RequestStop();

        const auto controls = mailbox.AcquireLatest();
        EXPECT_TRUE(controls.stopRequested);
        EXPECT_FALSE(controls.surface.has_value());
        EXPECT_FALSE(controls.resize.has_value());
        EXPECT_TRUE(mailbox.IsStopRequested());
        EXPECT_EQ(wakeCount.load(std::memory_order_relaxed), 3U);
    }

    TEST(RenderConcurrencyValidation,
         GatewayPreservesFrozenOutcomesAndShutdownIsNarrow)
    {
        RenderTransportConfig config;
        RenderResourceGateway gateway(config);
        const auto invalid =
            gateway.ReserveResource(AssetId{}, RenderResourceKind::Mesh);
        EXPECT_EQ(invalid.code, RenderResourceReserveCode::InvalidAsset);
        EXPECT_FALSE(invalid.handle.IsValid());
        const auto invalidKind =
            gateway.ReserveResource(AssetId{4001},
                                    RenderResourceKind::Invalid);
        EXPECT_EQ(invalidKind.code, RenderResourceReserveCode::KindMismatch);
        EXPECT_FALSE(invalidKind.handle.IsValid());

        const auto reserved =
            gateway.ReserveResource(AssetId{4002}, RenderResourceKind::Material);
        const auto existing =
            gateway.ReserveResource(AssetId{4002}, RenderResourceKind::Material);
        ASSERT_EQ(existing.code, RenderResourceReserveCode::Existing);
        EXPECT_EQ(existing.handle, reserved.handle);
        EXPECT_EQ(existing.status.code, RenderResourceStatusCode::Current);
        EXPECT_EQ(existing.status.state, RenderResourcePublicState::Reserved);

        gateway.BeginShutdown();
        const auto shuttingDown =
            gateway.ReserveResource(AssetId{4003}, RenderResourceKind::Mesh);
        EXPECT_EQ(shuttingDown.code, RenderResourceReserveCode::ShuttingDown);
        EXPECT_FALSE(shuttingDown.handle.IsValid());
        EXPECT_EQ(gateway.TryEnqueueUpload({}).code,
                  RenderUploadEnqueueCode::ShuttingDown);
        EXPECT_EQ(gateway.RequestRelease(reserved.handle).code,
                  RenderReleaseCode::ShuttingDown);
        EXPECT_EQ(gateway.QueryResourceStatus(reserved.handle).state,
                  RenderResourcePublicState::Reserved);
    }

    TEST(RenderConcurrencyValidation,
         GatewayRejectsRequestWhoseAssetDoesNotMatchReservation)
    {
        RenderTransportConfig config;
        RenderResourceGateway gateway(config);
        const auto reserved =
            gateway.ReserveResource(AssetId{4101}, RenderResourceKind::Mesh);
        ASSERT_EQ(reserved.code, RenderResourceReserveCode::Reserved);
        const auto mismatched =
            MakeMeshUploadRequest(reserved.handle, AssetId{4102}, 1);

        EXPECT_EQ(gateway.TryEnqueueUpload(mismatched).code,
                  RenderUploadEnqueueCode::InvalidRequest);
        EXPECT_EQ(gateway.QueryResourceStatus(reserved.handle).state,
                  RenderResourcePublicState::Reserved);
        EXPECT_EQ(gateway.GetRetainedUploadCount(), 0U);
    }
} // namespace
} // namespace RVX
