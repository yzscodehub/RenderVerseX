#include "ECS/ECS.h"

#include <gtest/gtest.h>
#include <optional>
#include <vector>

namespace
{
    struct Position
    {
        int value = 0;
    };

    struct Velocity
    {
        int value = 0;
    };

    struct RenderableTag
    {
    };

    struct HiddenTag
    {
    };

    struct FirstRegisteredFragment
    {
        int value = 0;
    };

    struct SecondRegisteredFragment
    {
        int value = 0;
    };
}

TEST(EcsFragmentQueryValidation, SparseQueryFiltersTagsAndDisabledEntities)
{
    RVX::ECS::Registry registry;
    const RVX::ECS::EntityHandle first = registry.CreateEntity();
    const RVX::ECS::EntityHandle second = registry.CreateEntity();
    const RVX::ECS::EntityHandle positionOnly = registry.CreateEntity();

    ASSERT_TRUE(registry.Add<Position>(first, {.value = 2}));
    ASSERT_TRUE(registry.Add<Velocity>(first, {.value = 3}));
    ASSERT_TRUE(registry.AddTag<RenderableTag>(first));
    ASSERT_TRUE(registry.Add<Position>(second, {.value = 5}));
    ASSERT_TRUE(registry.Add<Velocity>(second, {.value = 7}));
    ASSERT_TRUE(registry.AddTag<RenderableTag>(second));
    ASSERT_TRUE(registry.Add<Position>(positionOnly, {.value = 11}));
    ASSERT_TRUE(registry.Disable(second));

    int accumulated = 0;
    const RVX::uint32 visited = registry.Query<RVX::ECS::Read<Position>, RVX::ECS::Write<Velocity>>().Each(
        [&](RVX::ECS::EntityHandle entity, const Position& position, Velocity& velocity)
        {
            EXPECT_TRUE(registry.HasTag<RenderableTag>(entity));
            accumulated += position.value;
            velocity.value += position.value;
            EXPECT_FALSE(registry.AddTag<int>(entity));
        });

    EXPECT_EQ(visited, 1U);
    EXPECT_EQ(accumulated, 2);
    ASSERT_NE(registry.TryGet<Velocity>(first), nullptr);
    EXPECT_EQ(registry.TryGet<Velocity>(first)->value, 5);
    EXPECT_EQ(registry.GetFragmentWriteVersion<Velocity>(), 1U);
    EXPECT_EQ(registry.GetFragmentWriteVersion<Velocity>(first), 1U);
    EXPECT_EQ(registry.GetFragmentWriteVersion<Velocity>(second), 0U);
}

TEST(EcsFragmentQueryValidation, DirectWriteIsTheOnlyMutableLookupPath)
{
    RVX::ECS::Registry registry;
    const RVX::ECS::EntityHandle entity = registry.CreateEntity();
    ASSERT_TRUE(registry.Add<Position>(entity, {.value = 4}));

    ASSERT_TRUE(registry.Write<Position>(entity, [](Position& position)
    {
        position.value = 9;
    }));

    const Position* position = registry.TryGet<Position>(entity);
    ASSERT_NE(position, nullptr);
    EXPECT_EQ(position->value, 9);
    EXPECT_EQ(registry.GetFragmentWriteVersion<Position>(entity), 1U);
}

TEST(EcsFragmentQueryValidation, MultiWriteQueryAdvancesVersionsLeftToRight)
{
    RVX::ECS::Registry registry;
    const RVX::ECS::EntityHandle entity = registry.CreateEntity();
    ASSERT_TRUE(registry.Add<Position>(entity, {.value = 4}));
    ASSERT_TRUE(registry.Add<Velocity>(entity, {.value = 6}));

    const RVX::uint32 visited = registry.Query<RVX::ECS::Write<Position>, RVX::ECS::Write<Velocity>>().Each(
        [&](RVX::ECS::EntityHandle current, Position& position, Velocity& velocity)
        {
            EXPECT_EQ(current, entity);
            EXPECT_EQ(registry.GetFragmentWriteVersion<Position>(current), 1U);
            EXPECT_EQ(registry.GetFragmentWriteVersion<Velocity>(current), 2U);
            ++position.value;
            ++velocity.value;
        });

    EXPECT_EQ(visited, 1U);
    EXPECT_EQ(registry.GetFragmentWriteVersion<Position>(), 1U);
    EXPECT_EQ(registry.GetFragmentWriteVersion<Velocity>(), 2U);
}

TEST(EcsFragmentQueryValidation, DestructionJournalUsesRegisteredFragmentOrder)
{
    RVX::ECS::Registry registry;
    const RVX::ECS::EntityHandle entity = registry.CreateEntity();
    ASSERT_TRUE(registry.Add<FirstRegisteredFragment>(entity, {.value = 1}));
    ASSERT_TRUE(registry.Add<SecondRegisteredFragment>(entity, {.value = 2}));

    RVX::ECS::StructuralJournalCursor cursor = registry.GetStructuralJournal().CreateCursor();
    ASSERT_TRUE(registry.DestroyEntity(entity));

    const RVX::ECS::StructuralJournalRead changes = registry.ReadStructuralChanges(cursor);
    ASSERT_EQ(changes.changes.size(), 3U);
    EXPECT_EQ(changes.changes[0].kind, RVX::ECS::StructuralChangeKind::FragmentRemoved);
    EXPECT_EQ(changes.changes[0].fragmentType, std::type_index(typeid(FirstRegisteredFragment)));
    EXPECT_EQ(changes.changes[0].fragmentTypeId, 1U);
    EXPECT_EQ(changes.changes[1].kind, RVX::ECS::StructuralChangeKind::FragmentRemoved);
    EXPECT_EQ(changes.changes[1].fragmentType, std::type_index(typeid(SecondRegisteredFragment)));
    EXPECT_EQ(changes.changes[1].fragmentTypeId, 2U);
    EXPECT_EQ(changes.changes[2].kind, RVX::ECS::StructuralChangeKind::EntityDestroyed);
}

TEST(EcsFragmentQueryValidation, QuerySafelyBecomesEmptyAfterRegistryDestruction)
{
    std::optional<RVX::ECS::Query<RVX::ECS::Read<Position>>> query;
    {
        RVX::ECS::Registry registry;
        const RVX::ECS::EntityHandle entity = registry.CreateEntity();
        ASSERT_TRUE(registry.Add<Position>(entity, {.value = 3}));
        query.emplace(registry.Query<RVX::ECS::Read<Position>>());
    }

    EXPECT_EQ(query->Each([](RVX::ECS::EntityHandle, const Position&) {}), 0U);
}

TEST(EcsFragmentQueryValidation, WithWithoutAndEnableableMembershipAreExplicit)
{
    RVX::ECS::Registry registry;
    const auto visible = registry.CreateEntity();
    const auto hidden = registry.CreateEntity();
    const auto disabledEntity = registry.CreateEntity();
    for (const auto entity : {visible, hidden, disabledEntity})
    {
        ASSERT_TRUE(registry.Add<Position>(entity, {.value = 1}));
        ASSERT_TRUE(registry.AddTag<RenderableTag>(entity));
    }
    ASSERT_TRUE(registry.AddTag<HiddenTag>(hidden));
    ASSERT_TRUE(registry.Disable(disabledEntity));

    RVX::uint32 callbacks = 0;
    auto filtered = registry.Query<RVX::ECS::Read<Position>,
                                   RVX::ECS::With<RVX::ECS::Tag<RenderableTag>>,
                                   RVX::ECS::Without<RVX::ECS::Tag<HiddenTag>>>();
    EXPECT_EQ(filtered.Each([&](RVX::ECS::EntityHandle entity, const Position&)
    {
        ++callbacks;
        EXPECT_EQ(entity, visible);
    }), 1u);
    EXPECT_EQ(callbacks, 1u);

    EXPECT_EQ(filtered.EachIncludingDisabled(
        [](RVX::ECS::EntityHandle, const Position&) {}), 2u);

    ASSERT_TRUE(registry.SetFragmentEnabled<Position>(visible, false));
    EXPECT_TRUE(registry.Has<Position>(visible));
    EXPECT_FALSE(registry.IsFragmentEnabled<Position>(visible));
    EXPECT_EQ(filtered.EachIncludingDisabled(
        [](RVX::ECS::EntityHandle, const Position&) {}), 1u);
    ASSERT_TRUE(registry.SetFragmentEnabled<Position>(visible, true));
    EXPECT_TRUE(registry.IsFragmentEnabled<Position>(visible));
}

TEST(EcsFragmentQueryValidation, StableIterationExplicitlySortsGenerationSafeHandles)
{
    RVX::ECS::Registry registry;
    const RVX::ECS::EntityHandle first = registry.CreateEntity();
    const RVX::ECS::EntityHandle second = registry.CreateEntity();
    const RVX::ECS::EntityHandle third = registry.CreateEntity();
    ASSERT_TRUE(registry.Add<Position>(first, {.value = 1}));
    ASSERT_TRUE(registry.Add<Position>(second, {.value = 2}));
    ASSERT_TRUE(registry.Add<Position>(third, {.value = 3}));

    // Sparse-set removal swaps the final dense value into the removed slot.
    // Recreate the middle handle so the physical pool order is deliberately
    // different from the generation-safe handle ordering.
    ASSERT_TRUE(registry.DestroyEntity(second));
    const RVX::ECS::EntityHandle recycled = registry.CreateEntity();
    ASSERT_EQ(recycled.GetIndex(), second.GetIndex());
    ASSERT_NE(recycled.GetGeneration(), second.GetGeneration());
    ASSERT_TRUE(registry.Add<Position>(recycled, {.value = 4}));

    std::vector<RVX::ECS::EntityHandle> visited;
    EXPECT_EQ(registry.Query<RVX::ECS::Read<Position>>().EachStable(
                  [&visited](RVX::ECS::EntityHandle entity, const Position&)
                  {
                      visited.push_back(entity);
                  }),
              3u);

    ASSERT_EQ(visited.size(), 3u);
    EXPECT_TRUE(visited[0] < visited[1]);
    EXPECT_TRUE(visited[1] < visited[2]);
    EXPECT_EQ(visited[0], first);
    EXPECT_EQ(visited[1], recycled);
    EXPECT_EQ(visited[2], third);
}
