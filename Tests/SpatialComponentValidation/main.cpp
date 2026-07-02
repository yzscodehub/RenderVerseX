#include "Core/Core.h"
#include "Scene/Actor.h"
#include "Scene/Components/StaticMeshComponent.h"
#include "Scene/PrimitiveComponent.h"
#include "Scene/SceneManager.h"
#include "World/PickingService.h"
#include "World/SpatialSubsystem.h"
#include "World/World.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

using namespace RVX;

namespace
{
    struct PrimitiveFixture
    {
        SceneEntity* entity = nullptr;
        StaticMeshComponent* primitive = nullptr;
    };

    PrimitiveFixture CreatePrimitive(SceneManager& sceneManager,
                                     const char* name,
                                     const Vec3& location,
                                     uint32 layerMask = ~0u)
    {
        const auto handle = sceneManager.CreateEntity(name);
        auto* entity = sceneManager.GetEntity(handle);
        auto* primitive = static_cast<Actor*>(entity)->AddComponent<StaticMeshComponent>();
        primitive->AttachToComponent(entity->GetRootComponent());
        primitive->SetLocalBounds(AABB(Vec3(-1.0f), Vec3(1.0f)));
        primitive->SetLayerMask(layerMask);
        entity->SetPosition(location);
        return {entity, primitive};
    }

    TEST(SpatialComponentValidation, PrimitiveRaycastReturnsActorAndComponent)
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        auto fixture = CreatePrimitive(sceneManager, "PrimitiveHit", Vec3(0.0f));
        sceneManager.Update(0.0f);
        sceneManager.RebuildSpatialIndex();

        RaycastHit hit;
        const bool didHit = sceneManager.Raycast(Ray(Vec3(0.0f, 0.0f, 10.0f),
                                                     Vec3(0.0f, 0.0f, -1.0f)),
                                                 hit);

        EXPECT_TRUE(didHit);
        EXPECT_EQ(fixture.entity, hit.entity);
        EXPECT_EQ(static_cast<Actor*>(fixture.entity), hit.actor);
        EXPECT_EQ(static_cast<PrimitiveComponent*>(fixture.primitive), hit.component);
        EXPECT_TRUE(hit.distance > 0.0f);

        sceneManager.Shutdown();
    }

    TEST(SpatialComponentValidation, PrimitiveBoxQueryReturnsPrimitiveAndDeduplicatedEntity)
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        auto fixture = CreatePrimitive(sceneManager, "PrimitiveQuery", Vec3(0.0f));
        fixture.entity->SetLocalBounds(AABB(Vec3(-5.0f), Vec3(5.0f)));
        sceneManager.Update(0.0f);
        sceneManager.RebuildSpatialIndex();

        std::vector<PrimitiveComponent*> primitives;
        sceneManager.QueryBoxPrimitives(AABB(Vec3(-2.0f), Vec3(2.0f)), primitives);
        EXPECT_EQ(static_cast<size_t>(1), primitives.size());
        EXPECT_EQ(static_cast<PrimitiveComponent*>(fixture.primitive), primitives[0]);

        std::vector<SceneEntity*> entities;
        sceneManager.QueryBox(AABB(Vec3(-2.0f), Vec3(2.0f)), entities);
        EXPECT_EQ(static_cast<size_t>(1), entities.size());
        EXPECT_EQ(fixture.entity, entities[0]);

        sceneManager.Shutdown();
    }

    TEST(SpatialComponentValidation, LegacyEntitySpatialQueriesStillWork)
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        const auto handle = sceneManager.CreateEntity("LegacySpatial");
        auto* entity = sceneManager.GetEntity(handle);
        entity->SetLocalBounds(AABB(Vec3(-1.0f), Vec3(1.0f)));
        sceneManager.Update(0.0f);
        sceneManager.RebuildSpatialIndex();

        RaycastHit hit;
        const bool didHit = sceneManager.Raycast(Ray(Vec3(0.0f, 0.0f, 10.0f),
                                                     Vec3(0.0f, 0.0f, -1.0f)),
                                                 hit);

        EXPECT_TRUE(didHit);
        EXPECT_EQ(entity, hit.entity);
        EXPECT_EQ(static_cast<Actor*>(entity), hit.actor);
        EXPECT_EQ(nullptr, hit.component);

        sceneManager.Shutdown();
    }

    TEST(SpatialComponentValidation, PrimitiveLayerFilterAffectsSpatialQueries)
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        auto fixture = CreatePrimitive(sceneManager, "LayeredPrimitive", Vec3(0.0f), 1u << 3);
        sceneManager.Update(0.0f);
        sceneManager.RebuildSpatialIndex();

        RaycastHit hit;
        EXPECT_FALSE(sceneManager.Raycast(Ray(Vec3(0.0f, 0.0f, 10.0f),
                                                Vec3(0.0f, 0.0f, -1.0f)),
                                            Spatial::QueryFilter::Layer(2),
                                            hit));

        EXPECT_TRUE(sceneManager.Raycast(Ray(Vec3(0.0f, 0.0f, 10.0f),
                                               Vec3(0.0f, 0.0f, -1.0f)),
                                           Spatial::QueryFilter::Layer(3),
                                           hit));
        EXPECT_EQ(static_cast<PrimitiveComponent*>(fixture.primitive), hit.component);

        sceneManager.Shutdown();
    }

    TEST(SpatialComponentValidation, PrimitiveCustomAndTypeFiltersUseSpatialEntityContract)
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        auto fixture = CreatePrimitive(sceneManager, "FilteredPrimitive", Vec3(0.0f), 1u << 4);
        sceneManager.Update(0.0f);
        sceneManager.RebuildSpatialIndex();

        Spatial::QueryFilter filter = Spatial::QueryFilter::Type(
            static_cast<uint32_t>(EntityType::Node));
        filter.customFilter = [](const Spatial::ISpatialEntity* entity) {
            return entity && entity->GetLayerMask() == (1u << 4);
        };

        RaycastHit hit;
        EXPECT_TRUE(sceneManager.Raycast(Ray(Vec3(0.0f, 0.0f, 10.0f),
                                               Vec3(0.0f, 0.0f, -1.0f)),
                                           filter,
                                           hit));
        EXPECT_EQ(fixture.entity, hit.entity);
        EXPECT_EQ(static_cast<PrimitiveComponent*>(fixture.primitive), hit.component);

        sceneManager.Shutdown();
    }

    TEST(SpatialComponentValidation, NonIndexablePrimitiveFallsBackToLegacyEntityBounds)
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        auto fixture = CreatePrimitive(sceneManager, "DisabledPrimitiveFallback", Vec3(0.0f));
        fixture.primitive->SetEnabled(false);
        fixture.entity->SetLocalBounds(AABB(Vec3(-2.0f), Vec3(2.0f)));
        sceneManager.Update(0.0f);
        sceneManager.RebuildSpatialIndex();

        std::vector<PrimitiveComponent*> primitives;
        sceneManager.QueryBoxPrimitives(AABB(Vec3(-3.0f), Vec3(3.0f)), primitives);
        EXPECT_TRUE(primitives.empty());

        RaycastHit hit;
        EXPECT_TRUE(sceneManager.Raycast(Ray(Vec3(0.0f, 0.0f, 10.0f),
                                               Vec3(0.0f, 0.0f, -1.0f)),
                                           hit));
        EXPECT_EQ(fixture.entity, hit.entity);
        EXPECT_EQ(static_cast<Actor*>(fixture.entity), hit.actor);
        EXPECT_EQ(nullptr, hit.component);

        sceneManager.Shutdown();
    }

    TEST(SpatialComponentValidation, PrimitiveDisableThroughBasePointerUpdatesSpatialIndex)
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        auto fixture = CreatePrimitive(sceneManager, "BaseDisablePrimitive", Vec3(0.0f));
        fixture.entity->SetLocalBounds(AABB(Vec3(-2.0f), Vec3(2.0f)));
        sceneManager.Update(0.0f);
        sceneManager.RebuildSpatialIndex();

        ActorComponent* baseComponent = fixture.primitive;
        baseComponent->SetEnabled(false);
        sceneManager.Update(0.0f);

        std::vector<PrimitiveComponent*> primitives;
        sceneManager.QueryBoxPrimitives(AABB(Vec3(-3.0f), Vec3(3.0f)), primitives);
        EXPECT_TRUE(primitives.empty());

        std::vector<SceneEntity*> entities;
        sceneManager.QueryBox(AABB(Vec3(-3.0f), Vec3(3.0f)), entities);
        EXPECT_EQ(static_cast<size_t>(1), entities.size());
        EXPECT_EQ(fixture.entity, entities[0]);

        sceneManager.Shutdown();
    }

    TEST(SpatialComponentValidation, PrimitiveUnregisterImmediatelyFallsBackToEntityBounds)
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        auto fixture = CreatePrimitive(sceneManager, "ImmediateUnregisterFallback", Vec3(0.0f));
        fixture.entity->SetLocalBounds(AABB(Vec3(-2.0f), Vec3(2.0f)));
        sceneManager.Update(0.0f);
        sceneManager.RebuildSpatialIndex();

        EXPECT_TRUE(static_cast<Actor*>(fixture.entity)->RemoveComponent<StaticMeshComponent>());

        std::vector<PrimitiveComponent*> primitives;
        sceneManager.QueryBoxPrimitives(AABB(Vec3(-3.0f), Vec3(3.0f)), primitives);
        EXPECT_TRUE(primitives.empty());

        std::vector<SceneEntity*> entities;
        sceneManager.QueryBox(AABB(Vec3(-3.0f), Vec3(3.0f)), entities);
        EXPECT_EQ(static_cast<size_t>(1), entities.size());
        EXPECT_EQ(fixture.entity, entities[0]);

        RaycastHit hit;
        EXPECT_TRUE(sceneManager.Raycast(Ray(Vec3(0.0f, 0.0f, 10.0f),
                                               Vec3(0.0f, 0.0f, -1.0f)),
                                           hit));
        EXPECT_EQ(fixture.entity, hit.entity);
        EXPECT_EQ(nullptr, hit.component);

        sceneManager.Shutdown();
    }

    TEST(SpatialComponentValidation, MultiplePrimitivesDeduplicateEntityQueries)
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        auto fixture = CreatePrimitive(sceneManager, "MultiPrimitiveActor", Vec3(0.0f));
        auto* secondPrimitive = static_cast<Actor*>(fixture.entity)->AddComponent<StaticMeshComponent>();
        secondPrimitive->AttachToComponent(fixture.entity->GetRootComponent());
        secondPrimitive->SetRelativeLocation(Vec3(3.0f, 0.0f, 0.0f));
        secondPrimitive->SetLocalBounds(AABB(Vec3(-1.0f), Vec3(1.0f)));

        sceneManager.Update(0.0f);
        sceneManager.RebuildSpatialIndex();

        std::vector<PrimitiveComponent*> primitives;
        sceneManager.QueryBoxPrimitives(AABB(Vec3(-2.0f), Vec3(5.0f)), primitives);
        EXPECT_EQ(static_cast<size_t>(2), primitives.size());

        std::vector<SceneEntity*> entities;
        sceneManager.QueryBox(AABB(Vec3(-2.0f), Vec3(5.0f)), entities);
        EXPECT_EQ(static_cast<size_t>(1), entities.size());
        EXPECT_EQ(fixture.entity, entities[0]);

        sceneManager.Shutdown();
    }

    TEST(SpatialComponentValidation, PrimitiveTransformUsesIncrementalBVHRefit)
    {
        SceneConfig config;
        config.rebuildThreshold = 1.0f;

        SceneManager sceneManager;
        sceneManager.Initialize(config);

        auto fixture = CreatePrimitive(sceneManager, "PrimitiveRefit", Vec3(0.0f));
        sceneManager.Update(0.0f);

        const auto initialStats = sceneManager.GetSpatialIndex()->GetStats();

        fixture.entity->SetPosition(Vec3(20.0f, 0.0f, 0.0f));
        sceneManager.Update(0.0f);

        const auto refitStats = sceneManager.GetSpatialIndex()->GetStats();
        EXPECT_EQ(initialStats.buildCount, refitStats.buildCount);
        EXPECT_GT(refitStats.refitCount, initialStats.refitCount);

        std::vector<PrimitiveComponent*> oldPrimitives;
        sceneManager.QueryBoxPrimitives(AABB(Vec3(-2.0f), Vec3(2.0f)), oldPrimitives);
        EXPECT_TRUE(oldPrimitives.empty());

        std::vector<PrimitiveComponent*> movedPrimitives;
        sceneManager.QueryBoxPrimitives(AABB(Vec3(18.0f, -2.0f, -2.0f),
                                             Vec3(22.0f, 2.0f, 2.0f)),
                                        movedPrimitives);
        ASSERT_EQ(static_cast<size_t>(1), movedPrimitives.size());
        EXPECT_EQ(static_cast<PrimitiveComponent*>(fixture.primitive), movedPrimitives[0]);

        sceneManager.Shutdown();
    }

    TEST(SpatialComponentValidation, OwnerLayerDirtyQueueDeduplicatesPrimitiveProxyUpdate)
    {
        SceneConfig config;
        config.rebuildThreshold = 1.0f;

        SceneManager sceneManager;
        sceneManager.Initialize(config);

        auto fixture = CreatePrimitive(sceneManager, "OwnerLayerDirtyQueue", Vec3(0.0f));
        sceneManager.Update(0.0f);

        const auto initialStats = sceneManager.GetStats();
        EXPECT_EQ(static_cast<size_t>(0), initialStats.pendingDirtyEntityCount);
        EXPECT_EQ(static_cast<size_t>(0), initialStats.pendingDirtyPrimitiveCount);

        fixture.entity->SetLayer(3);
        fixture.entity->SetLayer(4);

        const auto pendingStats = sceneManager.GetStats();
        EXPECT_EQ(static_cast<size_t>(1), pendingStats.pendingDirtyEntityCount);
        EXPECT_EQ(static_cast<size_t>(1), pendingStats.pendingDirtyPrimitiveCount);

        sceneManager.Update(0.0f);

        const auto updatedStats = sceneManager.GetStats();
        EXPECT_EQ(initialStats.spatialStats.buildCount, updatedStats.spatialStats.buildCount);
        EXPECT_GT(updatedStats.spatialStats.refitCount, initialStats.spatialStats.refitCount);
        EXPECT_EQ(static_cast<size_t>(0), updatedStats.pendingDirtyEntityCount);
        EXPECT_EQ(static_cast<size_t>(0), updatedStats.pendingDirtyPrimitiveCount);

        RaycastHit hit;
        EXPECT_FALSE(sceneManager.Raycast(Ray(Vec3(0.0f, 0.0f, 10.0f),
                                                Vec3(0.0f, 0.0f, -1.0f)),
                                           Spatial::QueryFilter::Layer(3),
                                           hit));
        EXPECT_TRUE(sceneManager.Raycast(Ray(Vec3(0.0f, 0.0f, 10.0f),
                                               Vec3(0.0f, 0.0f, -1.0f)),
                                          Spatial::QueryFilter::Layer(4),
                                          hit));
        EXPECT_EQ(static_cast<PrimitiveComponent*>(fixture.primitive), hit.component);

        sceneManager.Shutdown();
    }

    TEST(SpatialComponentValidation, OwnerDirtyQueueTouchesOnlyOwnedPrimitiveProxies)
    {
        SceneConfig config;
        config.rebuildThreshold = 1.0f;

        SceneManager sceneManager;
        sceneManager.Initialize(config);

        auto ownerFixture = CreatePrimitive(sceneManager, "OwnerWithTwoPrimitives", Vec3(0.0f));
        auto* secondPrimitive = static_cast<Actor*>(ownerFixture.entity)->AddComponent<StaticMeshComponent>();
        secondPrimitive->AttachToComponent(ownerFixture.entity->GetRootComponent());
        secondPrimitive->SetRelativeLocation(Vec3(3.0f, 0.0f, 0.0f));
        secondPrimitive->SetLocalBounds(AABB(Vec3(-1.0f), Vec3(1.0f)));

        constexpr int unrelatedCount = 6;
        for (int i = 0; i < unrelatedCount; ++i)
        {
            CreatePrimitive(sceneManager,
                            "UnrelatedSpatialOwner",
                            Vec3(20.0f + static_cast<float>(i) * 4.0f, 0.0f, 0.0f));
        }

        sceneManager.Update(0.0f);

        const auto initialStats = sceneManager.GetStats();
        EXPECT_EQ(static_cast<size_t>(unrelatedCount + 2), initialStats.ownerPrimitiveLinkCount);
        EXPECT_EQ(static_cast<size_t>(0), initialStats.pendingDirtyEntityCount);
        EXPECT_EQ(static_cast<size_t>(0), initialStats.pendingDirtyPrimitiveCount);

        ownerFixture.entity->SetLayer(2);

        const auto pendingStats = sceneManager.GetStats();
        EXPECT_EQ(static_cast<size_t>(1), pendingStats.pendingDirtyEntityCount);
        EXPECT_EQ(static_cast<size_t>(2), pendingStats.pendingDirtyPrimitiveCount);

        sceneManager.Update(0.0f);

        const auto updatedStats = sceneManager.GetStats();
        EXPECT_EQ(initialStats.spatialStats.buildCount, updatedStats.spatialStats.buildCount);
        EXPECT_GT(updatedStats.spatialStats.refitCount, initialStats.spatialStats.refitCount);
        EXPECT_EQ(static_cast<size_t>(unrelatedCount + 2), updatedStats.ownerPrimitiveLinkCount);
        EXPECT_EQ(static_cast<size_t>(0), updatedStats.pendingDirtyEntityCount);
        EXPECT_EQ(static_cast<size_t>(0), updatedStats.pendingDirtyPrimitiveCount);

        RaycastHit hit;
        EXPECT_TRUE(sceneManager.Raycast(Ray(Vec3(0.0f, 0.0f, 10.0f),
                                               Vec3(0.0f, 0.0f, -1.0f)),
                                          Spatial::QueryFilter::Layer(2),
                                          hit));
        EXPECT_EQ(ownerFixture.entity, hit.entity);
        EXPECT_EQ(static_cast<PrimitiveComponent*>(ownerFixture.primitive), hit.component);

        sceneManager.Shutdown();
    }

    TEST(SpatialComponentValidation, MixedLegacyAndPrimitiveQueriesSynchronizeBeforeUpdate)
    {
        SceneConfig config;
        config.rebuildThreshold = 1.0f;

        SceneManager sceneManager;
        sceneManager.Initialize(config);

        const auto legacyHandle = sceneManager.CreateEntity("MixedLegacySpatial");
        auto* legacyEntity = sceneManager.GetEntity(legacyHandle);
        legacyEntity->SetLocalBounds(AABB(Vec3(-1.0f), Vec3(1.0f)));

        auto primitiveFixture = CreatePrimitive(sceneManager,
                                                "MixedPrimitiveSpatial",
                                                Vec3(8.0f, 0.0f, 0.0f));

        sceneManager.Update(0.0f);

        const auto initialStats = sceneManager.GetStats();
        EXPECT_EQ(static_cast<size_t>(0), initialStats.pendingDirtyEntityCount);
        EXPECT_EQ(static_cast<size_t>(0), initialStats.pendingDirtyPrimitiveCount);

        legacyEntity->SetPosition(Vec3(0.0f, 6.0f, 0.0f));
        primitiveFixture.entity->SetPosition(Vec3(8.0f, 6.0f, 0.0f));

        const auto pendingStats = sceneManager.GetStats();
        EXPECT_EQ(static_cast<size_t>(2), pendingStats.pendingDirtyEntityCount);
        EXPECT_EQ(static_cast<size_t>(1), pendingStats.pendingDirtyPrimitiveCount);

        std::vector<SceneEntity*> oldEntities;
        sceneManager.QueryBox(AABB(Vec3(-2.0f, -2.0f, -2.0f),
                                   Vec3(10.0f, 2.0f, 2.0f)),
                              oldEntities);
        EXPECT_TRUE(oldEntities.empty());

        const auto synchronizedStats = sceneManager.GetStats();
        EXPECT_EQ(initialStats.spatialStats.buildCount, synchronizedStats.spatialStats.buildCount);
        EXPECT_GT(synchronizedStats.spatialStats.refitCount, initialStats.spatialStats.refitCount);
        EXPECT_EQ(static_cast<size_t>(0), synchronizedStats.pendingDirtyEntityCount);
        EXPECT_EQ(static_cast<size_t>(0), synchronizedStats.pendingDirtyPrimitiveCount);

        std::vector<SceneEntity*> movedEntities;
        sceneManager.QuerySphere(Vec3(4.0f, 6.0f, 0.0f), 6.0f, movedEntities);
        ASSERT_EQ(static_cast<size_t>(2), movedEntities.size());
        EXPECT_NE(movedEntities.end(), std::find(movedEntities.begin(), movedEntities.end(), legacyEntity));
        EXPECT_NE(movedEntities.end(),
                  std::find(movedEntities.begin(), movedEntities.end(), primitiveFixture.entity));

        std::vector<PrimitiveComponent*> movedPrimitives;
        sceneManager.QuerySpherePrimitives(Vec3(8.0f, 6.0f, 0.0f), 3.0f, movedPrimitives);
        ASSERT_EQ(static_cast<size_t>(1), movedPrimitives.size());
        EXPECT_EQ(static_cast<PrimitiveComponent*>(primitiveFixture.primitive), movedPrimitives[0]);

        sceneManager.Shutdown();
    }

    TEST(SpatialComponentValidation, HighCountDirtyAndRemovalChurnKeepsQueriesConsistent)
    {
        SceneConfig config;
        config.rebuildThreshold = 1.0f;

        SceneManager sceneManager;
        sceneManager.Initialize(config);

        constexpr int ownerCount = 24;
        std::vector<PrimitiveFixture> fixtures;
        std::vector<StaticMeshComponent*> extraPrimitives;
        fixtures.reserve(ownerCount);

        for (int i = 0; i < ownerCount; ++i)
        {
            auto fixture = CreatePrimitive(sceneManager,
                                           "SpatialChurnOwner",
                                           Vec3(static_cast<float>(i) * 8.0f, 0.0f, 0.0f));
            fixtures.push_back(fixture);

            if (i % 3 == 0)
            {
                auto* extra = static_cast<Actor*>(fixture.entity)->AddComponent<StaticMeshComponent>();
                extra->AttachToComponent(fixture.entity->GetRootComponent());
                extra->SetRelativeLocation(Vec3(2.0f, 0.0f, 0.0f));
                extra->SetLocalBounds(AABB(Vec3(-0.5f), Vec3(0.5f)));
                extraPrimitives.push_back(extra);
            }
        }

        sceneManager.Update(0.0f);

        const auto initialStats = sceneManager.GetStats();
        EXPECT_EQ(static_cast<size_t>(ownerCount + extraPrimitives.size()),
                  initialStats.ownerPrimitiveLinkCount);
        EXPECT_EQ(static_cast<size_t>(0), initialStats.pendingDirtyEntityCount);
        EXPECT_EQ(static_cast<size_t>(0), initialStats.pendingDirtyPrimitiveCount);

        size_t expectedDirtyPrimitives = 0;
        for (int i = 0; i < ownerCount; i += 4)
        {
            fixtures[i].entity->SetPosition(Vec3(static_cast<float>(i) * 8.0f, 6.0f, 0.0f));
            expectedDirtyPrimitives += (i % 3 == 0) ? 2u : 1u;
        }

        const auto pendingMoveStats = sceneManager.GetStats();
        EXPECT_EQ(static_cast<size_t>((ownerCount + 3) / 4), pendingMoveStats.pendingDirtyEntityCount);
        EXPECT_EQ(expectedDirtyPrimitives, pendingMoveStats.pendingDirtyPrimitiveCount);

        sceneManager.Update(0.0f);

        const auto movedStats = sceneManager.GetStats();
        EXPECT_EQ(initialStats.spatialStats.buildCount, movedStats.spatialStats.buildCount);
        EXPECT_GT(movedStats.spatialStats.refitCount, initialStats.spatialStats.refitCount);
        EXPECT_EQ(static_cast<size_t>(0), movedStats.pendingDirtyEntityCount);
        EXPECT_EQ(static_cast<size_t>(0), movedStats.pendingDirtyPrimitiveCount);

        std::vector<PrimitiveComponent*> oldPrimitives;
        sceneManager.QueryBoxPrimitives(AABB(Vec3(-1.0f, -1.0f, -1.0f),
                                             Vec3(3.0f, 1.0f, 1.0f)),
                                        oldPrimitives);
        EXPECT_TRUE(oldPrimitives.empty());

        std::vector<PrimitiveComponent*> movedPrimitives;
        sceneManager.QueryBoxPrimitives(AABB(Vec3(-1.0f, 5.0f, -1.0f),
                                             Vec3(3.0f, 7.0f, 1.0f)),
                                        movedPrimitives);
        EXPECT_EQ(static_cast<size_t>(2), movedPrimitives.size());

        size_t removedPrimitiveCount = 0;
        for (int i = 0; i < ownerCount; i += 6)
        {
            auto* actor = static_cast<Actor*>(fixtures[i].entity);
            ASSERT_TRUE(actor->RemoveComponent<StaticMeshComponent>());
            ++removedPrimitiveCount;
        }

        const auto pendingRemovedStats = sceneManager.GetStats();
        EXPECT_EQ(static_cast<size_t>(ownerCount + extraPrimitives.size() - removedPrimitiveCount),
                  pendingRemovedStats.ownerPrimitiveLinkCount);
        EXPECT_EQ(static_cast<size_t>(0), pendingRemovedStats.pendingDirtyEntityCount);
        EXPECT_EQ(static_cast<size_t>(0), pendingRemovedStats.pendingDirtyPrimitiveCount);
        EXPECT_EQ(movedStats.spatialStats.buildCount, pendingRemovedStats.spatialStats.buildCount);

        std::vector<PrimitiveComponent*> removedOwnerPrimitives;
        sceneManager.QueryBoxPrimitives(AABB(Vec3(-1.0f, 5.0f, -1.0f),
                                             Vec3(1.0f, 7.0f, 1.0f)),
                                        removedOwnerPrimitives);
        EXPECT_TRUE(removedOwnerPrimitives.empty());

        const auto removedStats = sceneManager.GetStats();
        EXPECT_EQ(movedStats.spatialStats.buildCount + 1u, removedStats.spatialStats.buildCount);
        EXPECT_EQ(static_cast<size_t>(ownerCount + extraPrimitives.size() - removedPrimitiveCount),
                  removedStats.ownerPrimitiveLinkCount);

        std::vector<PrimitiveComponent*> survivingExtraPrimitives;
        sceneManager.QueryBoxPrimitives(AABB(Vec3(1.25f, 5.0f, -1.0f),
                                             Vec3(2.75f, 7.0f, 1.0f)),
                                        survivingExtraPrimitives);
        ASSERT_EQ(static_cast<size_t>(1), survivingExtraPrimitives.size());
        EXPECT_EQ(static_cast<PrimitiveComponent*>(extraPrimitives.front()), survivingExtraPrimitives[0]);

        sceneManager.Shutdown();
    }

    TEST(SpatialComponentValidation, InactiveOwnerRemovesPrimitiveProxyThroughDirtyQueue)
    {
        SceneConfig config;
        config.rebuildThreshold = 1.0f;

        SceneManager sceneManager;
        sceneManager.Initialize(config);

        auto fixture = CreatePrimitive(sceneManager, "InactiveOwnerProxy", Vec3(0.0f));
        sceneManager.Update(0.0f);

        const auto initialStats = sceneManager.GetStats();

        fixture.entity->SetActive(false);
        const auto pendingStats = sceneManager.GetStats();
        EXPECT_EQ(static_cast<size_t>(1), pendingStats.pendingDirtyEntityCount);
        EXPECT_EQ(static_cast<size_t>(1), pendingStats.pendingDirtyPrimitiveCount);

        sceneManager.Update(0.0f);

        const auto rebuiltStats = sceneManager.GetStats();
        EXPECT_GT(rebuiltStats.spatialStats.buildCount, initialStats.spatialStats.buildCount);
        EXPECT_EQ(static_cast<size_t>(0), rebuiltStats.pendingDirtyEntityCount);
        EXPECT_EQ(static_cast<size_t>(0), rebuiltStats.pendingDirtyPrimitiveCount);

        std::vector<PrimitiveComponent*> primitives;
        sceneManager.QueryBoxPrimitives(AABB(Vec3(-2.0f), Vec3(2.0f)), primitives);
        EXPECT_TRUE(primitives.empty());

        std::vector<SceneEntity*> entities;
        sceneManager.QueryBox(AABB(Vec3(-2.0f), Vec3(2.0f)), entities);
        EXPECT_TRUE(entities.empty());

        RaycastHit hit;
        EXPECT_FALSE(sceneManager.Raycast(Ray(Vec3(0.0f, 0.0f, 10.0f),
                                                Vec3(0.0f, 0.0f, -1.0f)),
                                           hit));

        sceneManager.Shutdown();
    }

    TEST(SpatialComponentValidation, WorldSpatialSubsystemAndPickingReturnComponentHit)
    {
        World world;
        world.Initialize();

        auto* sceneManager = world.GetSceneManager();
        auto fixture = CreatePrimitive(*sceneManager, "WorldPrimitive", Vec3(0.0f));

        auto* spatial = world.GetSpatial();
        ASSERT_NE(nullptr, spatial);
        EXPECT_EQ(sceneManager->GetSpatialIndex(), spatial->GetIndex());
        spatial->RebuildIndex();

        Ray ray(Vec3(0.0f, 0.0f, 10.0f), Vec3(0.0f, 0.0f, -1.0f));
        RaycastHit spatialHit;
        EXPECT_TRUE(spatial->Raycast(ray, spatialHit));
        EXPECT_EQ(fixture.entity, spatialHit.entity);
        EXPECT_EQ(static_cast<Actor*>(fixture.entity), spatialHit.actor);
        EXPECT_EQ(static_cast<PrimitiveComponent*>(fixture.primitive), spatialHit.component);

        PickingService picking(&world);
        RaycastHit pickHit;
        EXPECT_TRUE(picking.Pick(ray, pickHit));
        EXPECT_EQ(fixture.entity, pickHit.entity);
        EXPECT_EQ(static_cast<PrimitiveComponent*>(fixture.primitive), pickHit.component);

        world.Shutdown();
    }

    TEST(SpatialComponentValidation, WorldSpatialSubsystemQueriesSceneIndexAfterTick)
    {
        World world;
        world.Initialize();

        auto* sceneManager = world.GetSceneManager();
        auto fixture = CreatePrimitive(*sceneManager, "WorldTickSpatial", Vec3(0.0f));

        auto* spatial = world.GetSpatial();
        ASSERT_NE(nullptr, spatial);
        EXPECT_EQ(sceneManager->GetSpatialIndex(), spatial->GetIndex());

        world.Tick(0.0f);

        fixture.entity->SetPosition(Vec3(12.0f, 0.0f, 0.0f));
        world.Tick(0.0f);

        std::vector<SceneEntity*> oldEntities;
        spatial->QueryBox(AABB(Vec3(-2.0f), Vec3(2.0f)), oldEntities);
        EXPECT_TRUE(oldEntities.empty());

        std::vector<SceneEntity*> movedEntities;
        spatial->QueryBox(AABB(Vec3(10.0f, -2.0f, -2.0f),
                               Vec3(14.0f, 2.0f, 2.0f)),
                          movedEntities);
        ASSERT_EQ(static_cast<size_t>(1), movedEntities.size());
        EXPECT_EQ(fixture.entity, movedEntities[0]);

        world.Shutdown();
    }

    TEST(SpatialComponentValidation, WorldSpatialSubsystemLegacyEntityDoesNotRequireUserData)
    {
        World world;
        world.Initialize();

        auto* sceneManager = world.GetSceneManager();
        const auto handle = sceneManager->CreateEntity("WorldLegacySpatial");
        auto* entity = sceneManager->GetEntity(handle);
        entity->SetLocalBounds(AABB(Vec3(-1.0f), Vec3(1.0f)));
        entity->SetUserData(nullptr);

        auto* spatial = world.GetSpatial();
        ASSERT_NE(nullptr, spatial);
        spatial->RebuildIndex();

        RaycastHit hit;
        EXPECT_TRUE(spatial->Raycast(Ray(Vec3(0.0f, 0.0f, 10.0f),
                                             Vec3(0.0f, 0.0f, -1.0f)),
                                         hit));
        EXPECT_EQ(entity, hit.entity);
        EXPECT_EQ(static_cast<Actor*>(entity), hit.actor);
        EXPECT_EQ(nullptr, hit.component);

        world.Shutdown();
    }

    TEST(SpatialComponentValidation, WorldEntityQueriesDeduplicateMultiPrimitiveActors)
    {
        World world;
        world.Initialize();

        auto* sceneManager = world.GetSceneManager();
        auto fixture = CreatePrimitive(*sceneManager, "WorldMultiPrimitiveActor", Vec3(0.0f));
        auto* secondPrimitive = static_cast<Actor*>(fixture.entity)->AddComponent<StaticMeshComponent>();
        secondPrimitive->AttachToComponent(fixture.entity->GetRootComponent());
        secondPrimitive->SetRelativeLocation(Vec3(3.0f, 0.0f, 0.0f));
        secondPrimitive->SetLocalBounds(AABB(Vec3(-1.0f), Vec3(1.0f)));

        auto* spatial = world.GetSpatial();
        ASSERT_NE(nullptr, spatial);
        spatial->RebuildIndex();

        std::vector<SceneEntity*> entities;
        spatial->QueryBox(AABB(Vec3(-2.0f), Vec3(5.0f)), entities);
        EXPECT_EQ(static_cast<size_t>(1), entities.size());
        EXPECT_EQ(fixture.entity, entities[0]);

        world.Shutdown();
    }

    TEST(SpatialComponentValidation, WorldRetiredPrimitiveProxyFallsBackThroughSceneIndex)
    {
        World world;
        world.Initialize();

        auto* sceneManager = world.GetSceneManager();
        auto fixture = CreatePrimitive(*sceneManager, "WorldRetiredPrimitive", Vec3(0.0f));
        fixture.entity->SetLocalBounds(AABB(Vec3(-2.0f), Vec3(2.0f)));

        auto* spatial = world.GetSpatial();
        ASSERT_NE(nullptr, spatial);
        spatial->RebuildIndex();

        EXPECT_TRUE(static_cast<Actor*>(fixture.entity)->RemoveComponent<StaticMeshComponent>());

        std::vector<SceneEntity*> immediateEntities;
        spatial->QueryBox(AABB(Vec3(-3.0f), Vec3(3.0f)), immediateEntities);
        ASSERT_EQ(static_cast<size_t>(1), immediateEntities.size());
        EXPECT_EQ(fixture.entity, immediateEntities[0]);

        spatial->RebuildIndex();
        std::vector<SceneEntity*> rebuiltEntities;
        spatial->QueryBox(AABB(Vec3(-3.0f), Vec3(3.0f)), rebuiltEntities);
        EXPECT_EQ(static_cast<size_t>(1), rebuiltEntities.size());
        EXPECT_EQ(fixture.entity, rebuiltEntities[0]);

        world.Shutdown();
    }

    class LogEnvironment final : public ::testing::Environment
    {
    public:
        void SetUp() override
        {
            Log::Initialize();
        }

        void TearDown() override
        {
            Log::Shutdown();
        }
    };

    [[maybe_unused]] ::testing::Environment* const g_logEnvironment =
        ::testing::AddGlobalTestEnvironment(new LogEnvironment());
} // namespace
