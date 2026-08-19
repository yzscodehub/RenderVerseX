#include "ECS/ECS.h"

#include <gtest/gtest.h>

namespace
{
    struct Health
    {
        int value = 0;
    };
}

TEST(EcsEntityValidation, DestroyInvalidatesGenerationAndEntityRef)
{
    RVX::ECS::Registry registry;
    const RVX::ECS::EntityHandle first = registry.CreateEntity();
    const RVX::ECS::EntityRef reference = registry.GetRef(first);

    ASSERT_TRUE(first.IsValid());
    ASSERT_TRUE(reference.IsValid());
    ASSERT_TRUE(registry.Add<Health>(first, {.value = 10}));
    ASSERT_TRUE(registry.DestroyEntity(first));
    EXPECT_FALSE(registry.IsAlive(first));
    EXPECT_FALSE(reference.IsValid());
    EXPECT_EQ(registry.TryGet<Health>(first), nullptr);

    const RVX::ECS::EntityHandle replacement = registry.CreateEntity();
    ASSERT_TRUE(replacement.IsValid());
    EXPECT_EQ(replacement.GetIndex(), first.GetIndex());
    EXPECT_NE(replacement.GetGeneration(), first.GetGeneration());
    EXPECT_FALSE(registry.IsAlive(first));
    EXPECT_TRUE(registry.IsAlive(replacement));
}

TEST(EcsEntityValidation, EnableStateAndJournalCursorRemainExplicit)
{
    RVX::ECS::Registry registry;
    RVX::ECS::StructuralJournalCursor cursor;

    const RVX::ECS::EntityHandle entity = registry.CreateEntity();
    ASSERT_TRUE(registry.Disable(entity));
    ASSERT_TRUE(registry.Enable(entity));

    const RVX::ECS::StructuralJournalRead read = registry.ReadStructuralChanges(cursor);
    EXPECT_EQ(read.continuity, RVX::ECS::StructuralJournalContinuity::Continuous);
    ASSERT_EQ(read.changes.size(), 3U);
    EXPECT_EQ(read.changes[0].kind, RVX::ECS::StructuralChangeKind::EntityCreated);
    EXPECT_EQ(read.changes[1].kind, RVX::ECS::StructuralChangeKind::EntityDisabled);
    EXPECT_EQ(read.changes[2].kind, RVX::ECS::StructuralChangeKind::EntityEnabled);

    registry.TrimStructuralJournalBefore(read.nextSequence);
    cursor.nextSequence = 1;
    const RVX::ECS::StructuralJournalRead staleRead = registry.ReadStructuralChanges(cursor);
    EXPECT_EQ(staleRead.continuity, RVX::ECS::StructuralJournalContinuity::Lost);
    EXPECT_TRUE(staleRead.changes.empty());
    EXPECT_EQ(staleRead.nextSequence, registry.GetStructuralJournal().GetNextSequence());
    EXPECT_EQ(cursor.nextSequence, registry.GetStructuralJournal().GetNextSequence());
}

TEST(EcsEntityValidation, EntityRefSafelyExpiresWhenRegistryIsDestroyed)
{
    RVX::ECS::EntityRef reference;
    {
        RVX::ECS::Registry registry;
        const RVX::ECS::EntityHandle entity = registry.CreateEntity();
        ASSERT_TRUE(registry.Add<Health>(entity, {.value = 4}));
        reference = registry.GetRef(entity);
        ASSERT_TRUE(reference.IsValid());
    }

    EXPECT_FALSE(reference.IsValid());
    EXPECT_FALSE(reference.Has<Health>());
    EXPECT_EQ(reference.TryGet<Health>(), nullptr);
    EXPECT_FALSE(reference.Add<Health>({.value = 8}));
}

TEST(EcsEntityValidation, LostJournalReadNeverReturnsPartialRetainedChanges)
{
    RVX::ECS::Registry registry;
    RVX::ECS::StructuralJournalCursor staleCursor;
    const RVX::ECS::EntityHandle entity = registry.CreateEntity();
    ASSERT_TRUE(registry.Disable(entity));

    // Keep the disable change in the journal while making the creation change
    // unavailable. A stale consumer must rebuild rather than apply the tail.
    registry.TrimStructuralJournalBefore(2U);
    const RVX::ECS::StructuralJournalRead read = registry.ReadStructuralChanges(staleCursor);

    EXPECT_EQ(read.continuity, RVX::ECS::StructuralJournalContinuity::Lost);
    EXPECT_TRUE(read.changes.empty());
    EXPECT_EQ(read.nextSequence, registry.GetStructuralJournal().GetNextSequence());
}

TEST(EcsEntityValidation, BoundedJournalAutomaticallyForcesAuthoritativeResync)
{
    RVX::ECS::Registry registry(2);
    RVX::ECS::StructuralJournalCursor staleCursor;

    const RVX::ECS::EntityHandle entity = registry.CreateEntity();
    ASSERT_TRUE(registry.Disable(entity));
    ASSERT_TRUE(registry.Enable(entity));

    EXPECT_EQ(registry.GetStructuralJournal().GetCapacity(), 2u);
    EXPECT_EQ(registry.GetStructuralJournal().GetFirstAvailableSequence(), 2u);
    const RVX::ECS::StructuralJournalRead stale =
        registry.ReadStructuralChanges(staleCursor);
    EXPECT_EQ(stale.continuity, RVX::ECS::StructuralJournalContinuity::Lost);
    EXPECT_TRUE(stale.changes.empty());

    RVX::ECS::StructuralJournalCursor current =
        registry.GetStructuralJournal().CreateCursor();
    const RVX::ECS::StructuralJournalRead currentRead =
        registry.ReadStructuralChanges(current);
    EXPECT_EQ(currentRead.continuity, RVX::ECS::StructuralJournalContinuity::Continuous);
    EXPECT_TRUE(currentRead.changes.empty());
}

TEST(EcsEntityValidation, StructuralMutationGuardRejectsStructuralChangesButKeepsReadsAndWritesAvailable)
{
    RVX::ECS::Registry registry;
    const RVX::ECS::EntityHandle entity = registry.CreateEntity();
    ASSERT_TRUE(entity.IsValid());
    ASSERT_TRUE(registry.Add<Health>(entity, {.value = 4}));

    {
        RVX::ECS::Registry::StructuralMutationGuard outer =
            registry.AcquireStructuralMutationGuard();
        RVX::ECS::Registry::StructuralMutationGuard nested =
            registry.AcquireStructuralMutationGuard();

        EXPECT_FALSE(registry.CreateEntity().IsValid());
        EXPECT_FALSE(registry.DestroyEntity(entity));
        EXPECT_FALSE(registry.Add<int>(entity, 7));
        EXPECT_FALSE(registry.Remove<Health>(entity));
        EXPECT_FALSE(registry.SetEnabled(entity, false));
        EXPECT_FALSE(registry.SetFragmentEnabled<Health>(entity, false));

        RVX::ECS::EntityTransaction transaction = registry.BeginTransaction();
        EXPECT_TRUE(transaction.Create().IsQueued());
        EXPECT_FALSE(transaction.Commit().IsApplied());

        RVX::uint32 queryCount = 0;
        registry.Query<RVX::ECS::Read<Health>>().Each(
            [&queryCount](RVX::ECS::EntityHandle, const Health&) { ++queryCount; });
        EXPECT_EQ(queryCount, 1u);
        EXPECT_TRUE(registry.Write<Health>(entity, [](Health& health) { health.value = 9; }));
    }

    EXPECT_TRUE(registry.IsAlive(entity));
    ASSERT_NE(registry.TryGet<Health>(entity), nullptr);
    EXPECT_EQ(registry.TryGet<Health>(entity)->value, 9);
    EXPECT_TRUE(registry.SetEnabled(entity, false));
    EXPECT_TRUE(registry.Enable(entity));
}
