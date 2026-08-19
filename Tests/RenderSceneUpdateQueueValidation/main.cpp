#include "Render/Runtime/RenderSceneUpdateQueue.h"
#include "RenderContracts/RenderFramePacketV5.h"

#include <gtest/gtest.h>

#include <memory>

namespace
{
    std::unique_ptr<const RVX::RenderSceneUpdateBatch> MakeBatch(
        RVX::uint64 base,
        RVX::uint64 target,
        bool fullReset = false)
    {
        auto batch = std::make_unique<RVX::RenderSceneUpdateBatch>();
        batch->baseSceneRevision = base;
        batch->targetSceneRevision = target;
        batch->fullReset = fullReset;
        return batch;
    }
}

TEST(RenderSceneUpdateQueueValidation, RequiresInitialFullResetCheckpoint)
{
    RVX::RenderSceneUpdateQueue queue;
    auto result = queue.TryPublish(MakeBatch(0, 1, false));

    EXPECT_EQ(result.code, RVX::RenderSceneUpdatePublishCode::RevisionMismatch);
    ASSERT_NE(result.rejectedBatch, nullptr);
    EXPECT_EQ(queue.GetSnapshot().pendingCount, 0U);
}

TEST(RenderSceneUpdateQueueValidation, FullQueueReturnsOwnershipAndNeverDrops)
{
    RVX::RenderSceneUpdateQueue queue(2);
    EXPECT_TRUE(queue.TryPublish(MakeBatch(0, 1, true)).IsAccepted());
    EXPECT_TRUE(queue.TryPublish(MakeBatch(1, 2)).IsAccepted());

    auto full = queue.TryPublish(MakeBatch(2, 3));
    EXPECT_EQ(full.code, RVX::RenderSceneUpdatePublishCode::Full);
    ASSERT_NE(full.rejectedBatch, nullptr);
    EXPECT_EQ(full.snapshot.lastPublishedRevision, 2U);

    auto first = queue.AcquireNext();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->targetSceneRevision, 1U);
    EXPECT_TRUE(queue.TryPublish(std::move(full.rejectedBatch)).IsAccepted());

    auto second = queue.AcquireNext();
    auto third = queue.AcquireNext();
    ASSERT_NE(second, nullptr);
    ASSERT_NE(third, nullptr);
    EXPECT_EQ(second->targetSceneRevision, 2U);
    EXPECT_EQ(third->targetSceneRevision, 3U);
    EXPECT_EQ(queue.AcquireNext(), nullptr);
}

TEST(RenderSceneUpdateQueueValidation, RejectsRevisionGapsWithoutAdvancingProducer)
{
    RVX::RenderSceneUpdateQueue queue;
    EXPECT_TRUE(queue.TryPublish(MakeBatch(0, 4, true)).IsAccepted());

    auto gap = queue.TryPublish(MakeBatch(3, 5));
    EXPECT_EQ(gap.code, RVX::RenderSceneUpdatePublishCode::RevisionMismatch);
    EXPECT_EQ(gap.snapshot.lastPublishedRevision, 4U);
    EXPECT_TRUE(queue.TryPublish(MakeBatch(4, 5)).IsAccepted());
}

TEST(RenderSceneUpdateQueueValidation, AccumulatorCoalescesCompleteStateRules)
{
    RVX::RenderSceneMutationAccumulator accumulator;
    accumulator.Begin(7);

    RVX::RenderPrimitiveSnapshot created;
    created.objectId = 10;
    ASSERT_TRUE(accumulator.UpsertPrimitive(created, true));
    created.sortKey = 42;
    ASSERT_TRUE(accumulator.UpsertPrimitive(created, false));
    ASSERT_TRUE(accumulator.RemovePrimitive(10));

    RVX::RenderPrimitiveSnapshot updated;
    updated.objectId = 20;
    ASSERT_TRUE(accumulator.UpsertPrimitive(updated));
    ASSERT_TRUE(accumulator.RemovePrimitive(20));

    ASSERT_TRUE(accumulator.RemovePrimitive(30));
    RVX::RenderPrimitiveSnapshot illegalReuse;
    illegalReuse.objectId = 30;
    EXPECT_FALSE(accumulator.UpsertPrimitive(illegalReuse, true));

    const RVX::RenderSceneUpdateBatch batch = accumulator.Build(8);
    ASSERT_TRUE(batch.IsStructurallyValid());
    ASSERT_EQ(batch.primitives.size(), 2U);
    EXPECT_EQ(batch.primitives[0].objectId, 20U);
    EXPECT_EQ(batch.primitives[0].operation,
              RVX::RenderSceneMutationOperation::Remove);
    EXPECT_EQ(batch.primitives[1].objectId, 30U);
    EXPECT_EQ(batch.primitives[1].operation,
              RVX::RenderSceneMutationOperation::Remove);
}

TEST(RenderSceneUpdateQueueValidation, V5PacketRequiresSceneRevision)
{
    RVX::RenderFrameHeaderV5 header;
    header.sequence = 1;
    RVX::RenderViewSnapshot view;
    view.viewportWidth = 1280;
    view.viewportHeight = 720;
    RVX::RenderExtractionDiagnostics diagnostics;
    diagnostics.complete = true;

    EXPECT_EQ(RVX::RenderFramePacketV5::Create(
                  header, view, {}, {}, diagnostics),
              nullptr);
    header.requiredSceneRevision = 9;
    const auto packet = RVX::RenderFramePacketV5::Create(
        header, view, {}, {}, diagnostics);
    ASSERT_NE(packet, nullptr);
    EXPECT_EQ(packet->GetHeader().requiredSceneRevision, 9U);
}
