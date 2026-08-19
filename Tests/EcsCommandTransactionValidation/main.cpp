#include "ECS/ECS.h"

#include <gtest/gtest.h>
#include <optional>

namespace
{
    struct Health
    {
        int value = 0;
    };

    struct Damage
    {
        int value = 0;
    };

    struct ParentLink
    {
        RVX::ECS::EntityHandle parent = RVX::ECS::EntityHandle::Invalid();
    };
}

TEST(EcsCommandTransactionValidation, DirectHandleCommandsApplyAndReportReceipts)
{
    RVX::ECS::Registry registry;
    const RVX::ECS::EntityHandle entity = registry.CreateEntity();
    auto commands = registry.CreateCommandBuffer();

    const RVX::ECS::CommandReceipt add = commands.Add<Health>(entity, {.value = 20});
    const RVX::ECS::CommandReceipt disable = commands.SetEnabled(entity, false);
    ASSERT_TRUE(commands.Commit(registry));

    EXPECT_TRUE(add.IsApplied());
    EXPECT_EQ(add.GetError(), RVX::ECS::CommandError::None);
    EXPECT_TRUE(disable.IsApplied());
    EXPECT_FALSE(registry.IsEnabled(entity));
    ASSERT_NE(registry.TryGet<Health>(entity), nullptr);
    EXPECT_EQ(registry.TryGet<Health>(entity)->value, 20);
}

TEST(EcsCommandTransactionValidation, TransactionFailureRollsBackAndTerminatesAllReceipts)
{
    RVX::ECS::Registry registry;
    const RVX::ECS::EntityHandle existing = registry.CreateEntity();
    ASSERT_TRUE(registry.Add<Health>(existing, {.value = 5}));

    auto transaction = registry.BeginTransaction();
    const RVX::ECS::EntityReceipt created = transaction.Create();
    const RVX::ECS::CommandReceipt addedDamage = transaction.Add<Damage>(existing, {.value = 3});
    const RVX::ECS::CommandReceipt duplicateHealth = transaction.Add<Health>(existing, {.value = 8});
    const RVX::ECS::TransactionReceipt result = transaction.Commit();

    EXPECT_TRUE(result.IsRejected());
    EXPECT_TRUE(created.IsRejected());
    EXPECT_FALSE(created.IsResolved());
    EXPECT_EQ(created.GetError(), RVX::ECS::CommandError::TransactionAborted);
    EXPECT_TRUE(addedDamage.IsRejected());
    EXPECT_EQ(addedDamage.GetError(), RVX::ECS::CommandError::TransactionAborted);
    EXPECT_TRUE(duplicateHealth.IsRejected());
    EXPECT_EQ(duplicateHealth.GetError(), RVX::ECS::CommandError::OperationRejected);
    EXPECT_FALSE(registry.Has<Damage>(existing));
    ASSERT_NE(registry.TryGet<Health>(existing), nullptr);
    EXPECT_EQ(registry.TryGet<Health>(existing)->value, 5);
}

TEST(EcsCommandTransactionValidation, TransactionResolvesCreateReceiptForFollowingCommands)
{
    RVX::ECS::Registry registry;
    auto transaction = registry.BeginTransaction();
    const RVX::ECS::EntityReceipt created = transaction.Create();
    const RVX::ECS::CommandReceipt addHealth = transaction.Add<Health>(created, {.value = 13});

    const RVX::ECS::TransactionReceipt result = transaction.Commit();
    ASSERT_TRUE(result.IsApplied());
    ASSERT_TRUE(created.IsResolved());
    EXPECT_TRUE(addHealth.IsApplied());
    ASSERT_NE(registry.TryGet<Health>(created.GetEntity()), nullptr);
    EXPECT_EQ(registry.TryGet<Health>(created.GetEntity())->value, 13);
}

TEST(EcsCommandTransactionValidation, CommandBufferRejectsSameHandleInAnotherRegistry)
{
    RVX::ECS::Registry source;
    RVX::ECS::Registry destination;
    const RVX::ECS::EntityHandle sourceEntity = source.CreateEntity();
    const RVX::ECS::EntityHandle destinationEntity = destination.CreateEntity();
    ASSERT_EQ(sourceEntity.GetIndex(), destinationEntity.GetIndex());
    ASSERT_EQ(sourceEntity.GetGeneration(), destinationEntity.GetGeneration());

    auto commands = source.CreateCommandBuffer();
    const RVX::ECS::CommandReceipt add = commands.Add<Health>(sourceEntity, {.value = 17});

    EXPECT_FALSE(commands.Commit(destination));
    EXPECT_TRUE(add.IsRejected());
    EXPECT_EQ(add.GetError(), RVX::ECS::CommandError::InvalidTarget);
    EXPECT_FALSE(source.Has<Health>(sourceEntity));
    EXPECT_FALSE(destination.Has<Health>(destinationEntity));
}

TEST(EcsCommandTransactionValidation, EntityRefAndEntityReceiptCannotCrossCommandBufferRegistries)
{
    RVX::ECS::Registry source;
    RVX::ECS::Registry destination;
    const RVX::ECS::EntityHandle sourceEntity = source.CreateEntity();
    const RVX::ECS::EntityRef sourceReference = source.GetRef(sourceEntity);

    auto sourceCommands = source.CreateCommandBuffer();
    const RVX::ECS::EntityReceipt sourceCreated = sourceCommands.Create();
    auto destinationCommands = destination.CreateCommandBuffer();

    const RVX::ECS::CommandReceipt byReference = destinationCommands.Add<Health>(sourceReference, {.value = 2});
    const RVX::ECS::CommandReceipt byReceipt = destinationCommands.Add<Health>(sourceCreated, {.value = 3});

    EXPECT_TRUE(byReference.IsRejected());
    EXPECT_EQ(byReference.GetError(), RVX::ECS::CommandError::InvalidTarget);
    EXPECT_TRUE(byReceipt.IsRejected());
    EXPECT_EQ(byReceipt.GetError(), RVX::ECS::CommandError::InvalidTarget);
}

TEST(EcsCommandTransactionValidation, ResetAndStaleTransactionTerminalizeOutstandingReceipts)
{
    RVX::ECS::Registry registry;
    auto transaction = registry.BeginTransaction();
    const RVX::ECS::EntityReceipt resetCreated = transaction.Create();
    transaction.Commands().Reset();
    const RVX::ECS::TransactionReceipt resetResult = transaction.Commit();

    EXPECT_TRUE(resetResult.IsApplied());
    EXPECT_TRUE(resetCreated.IsDiscarded());
    EXPECT_EQ(resetCreated.GetError(), RVX::ECS::CommandError::Discarded);

    std::optional<RVX::ECS::EntityTransaction> staleTransaction;
    RVX::ECS::EntityReceipt staleCreated;
    {
        RVX::ECS::Registry temporaryRegistry;
        staleTransaction.emplace(temporaryRegistry);
        staleCreated = staleTransaction->Create();
        ASSERT_TRUE(staleCreated.IsQueued());
    }

    const RVX::ECS::TransactionReceipt staleResult = staleTransaction->Commit();
    EXPECT_TRUE(staleResult.IsRejected());
    EXPECT_EQ(staleResult.GetError(), RVX::ECS::CommandError::InvalidTarget);
    EXPECT_TRUE(staleCreated.IsRejected());
    EXPECT_EQ(staleCreated.GetError(), RVX::ECS::CommandError::InvalidTarget);
}

TEST(EcsCommandTransactionValidation, CommandBufferDestructorDiscardsOutstandingReceipts)
{
    RVX::ECS::Registry registry;
    const RVX::ECS::EntityHandle entity = registry.CreateEntity();
    RVX::ECS::CommandReceipt receipt;
    {
        auto commands = registry.CreateCommandBuffer();
        receipt = commands.SetEnabled(entity, false);
        ASSERT_TRUE(receipt.IsQueued());
    }

    EXPECT_TRUE(receipt.IsDiscarded());
    EXPECT_EQ(receipt.GetError(), RVX::ECS::CommandError::Discarded);
    EXPECT_TRUE(registry.IsEnabled(entity));
}

TEST(EcsCommandTransactionValidation, FragmentEnableChangesPlaybackAtTheCommandBoundary)
{
    RVX::ECS::Registry registry;
    const auto entity = registry.CreateEntity();
    ASSERT_TRUE(registry.Add<Health>(entity, {.value = 9}));
    auto commands = registry.CreateCommandBuffer();
    const auto disable = commands.SetFragmentEnabled<Health>(entity, false);

    EXPECT_TRUE(registry.IsFragmentEnabled<Health>(entity));
    ASSERT_TRUE(commands.Commit(registry));
    EXPECT_TRUE(disable.IsApplied());
    EXPECT_TRUE(registry.Has<Health>(entity));
    EXPECT_FALSE(registry.IsFragmentEnabled<Health>(entity));

    RVX::ECS::StructuralJournalCursor cursor{
        .nextSequence = registry.GetStructuralJournal().GetNextSequence() - 1u,
    };
    const auto changes = registry.ReadStructuralChanges(cursor);
    ASSERT_EQ(changes.changes.size(), 1u);
    EXPECT_EQ(changes.changes.front().kind,
              RVX::ECS::StructuralChangeKind::FragmentDisabled);
}

TEST(EcsCommandTransactionValidation, LinkedReceiptsPublishRelationshipFragmentsAtomically)
{
    RVX::ECS::Registry registry;
    auto transaction = registry.BeginTransaction();
    const auto parent = transaction.Create();
    const auto child = transaction.Create();
    const auto relation = transaction.AddLinked<ParentLink, &ParentLink::parent>(child, parent);

    ASSERT_TRUE(transaction.Commit().IsApplied());
    ASSERT_TRUE(parent.IsResolved());
    ASSERT_TRUE(child.IsResolved());
    EXPECT_TRUE(relation.IsApplied());
    const ParentLink* link = registry.TryGet<ParentLink>(child.GetEntity());
    ASSERT_NE(link, nullptr);
    EXPECT_EQ(link->parent, parent.GetEntity());
}
