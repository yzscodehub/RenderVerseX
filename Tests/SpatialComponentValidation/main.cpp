#include "Core/Core.h"
#include "Scene/Actor.h"
#include "Scene/Components/StaticMeshComponent.h"
#include "Scene/PrimitiveComponent.h"
#include "Scene/SceneManager.h"
#include "TestFramework/TestRunner.h"
#include "World/PickingService.h"
#include "World/SpatialSubsystem.h"
#include "World/World.h"

using namespace RVX;
using namespace RVX::Test;

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

    bool Test_PrimitiveRaycastReturnsActorAndComponent()
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

        TEST_ASSERT_TRUE(didHit);
        TEST_ASSERT_EQ(fixture.entity, hit.entity);
        TEST_ASSERT_EQ(static_cast<Actor*>(fixture.entity), hit.actor);
        TEST_ASSERT_EQ(static_cast<PrimitiveComponent*>(fixture.primitive), hit.component);
        TEST_ASSERT_TRUE(hit.distance > 0.0f);

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrimitiveBoxQueryReturnsPrimitiveAndDeduplicatedEntity()
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        auto fixture = CreatePrimitive(sceneManager, "PrimitiveQuery", Vec3(0.0f));
        fixture.entity->SetLocalBounds(AABB(Vec3(-5.0f), Vec3(5.0f)));
        sceneManager.Update(0.0f);
        sceneManager.RebuildSpatialIndex();

        std::vector<PrimitiveComponent*> primitives;
        sceneManager.QueryBoxPrimitives(AABB(Vec3(-2.0f), Vec3(2.0f)), primitives);
        TEST_ASSERT_EQ(static_cast<size_t>(1), primitives.size());
        TEST_ASSERT_EQ(static_cast<PrimitiveComponent*>(fixture.primitive), primitives[0]);

        std::vector<SceneEntity*> entities;
        sceneManager.QueryBox(AABB(Vec3(-2.0f), Vec3(2.0f)), entities);
        TEST_ASSERT_EQ(static_cast<size_t>(1), entities.size());
        TEST_ASSERT_EQ(fixture.entity, entities[0]);

        sceneManager.Shutdown();
        return true;
    }

    bool Test_LegacyEntitySpatialQueriesStillWork()
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

        TEST_ASSERT_TRUE(didHit);
        TEST_ASSERT_EQ(entity, hit.entity);
        TEST_ASSERT_EQ(static_cast<Actor*>(entity), hit.actor);
        TEST_ASSERT_EQ(nullptr, hit.component);

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrimitiveLayerFilterAffectsSpatialQueries()
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        auto fixture = CreatePrimitive(sceneManager, "LayeredPrimitive", Vec3(0.0f), 1u << 3);
        sceneManager.Update(0.0f);
        sceneManager.RebuildSpatialIndex();

        RaycastHit hit;
        TEST_ASSERT_FALSE(sceneManager.Raycast(Ray(Vec3(0.0f, 0.0f, 10.0f),
                                                Vec3(0.0f, 0.0f, -1.0f)),
                                            Spatial::QueryFilter::Layer(2),
                                            hit));

        TEST_ASSERT_TRUE(sceneManager.Raycast(Ray(Vec3(0.0f, 0.0f, 10.0f),
                                               Vec3(0.0f, 0.0f, -1.0f)),
                                           Spatial::QueryFilter::Layer(3),
                                           hit));
        TEST_ASSERT_EQ(static_cast<PrimitiveComponent*>(fixture.primitive), hit.component);

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrimitiveCustomAndTypeFiltersUseSpatialEntityContract()
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
        TEST_ASSERT_TRUE(sceneManager.Raycast(Ray(Vec3(0.0f, 0.0f, 10.0f),
                                               Vec3(0.0f, 0.0f, -1.0f)),
                                           filter,
                                           hit));
        TEST_ASSERT_EQ(fixture.entity, hit.entity);
        TEST_ASSERT_EQ(static_cast<PrimitiveComponent*>(fixture.primitive), hit.component);

        sceneManager.Shutdown();
        return true;
    }

    bool Test_NonIndexablePrimitiveFallsBackToLegacyEntityBounds()
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
        TEST_ASSERT_TRUE(primitives.empty());

        RaycastHit hit;
        TEST_ASSERT_TRUE(sceneManager.Raycast(Ray(Vec3(0.0f, 0.0f, 10.0f),
                                               Vec3(0.0f, 0.0f, -1.0f)),
                                           hit));
        TEST_ASSERT_EQ(fixture.entity, hit.entity);
        TEST_ASSERT_EQ(static_cast<Actor*>(fixture.entity), hit.actor);
        TEST_ASSERT_EQ(nullptr, hit.component);

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrimitiveDisableThroughBasePointerUpdatesSpatialIndex()
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
        TEST_ASSERT_TRUE(primitives.empty());

        std::vector<SceneEntity*> entities;
        sceneManager.QueryBox(AABB(Vec3(-3.0f), Vec3(3.0f)), entities);
        TEST_ASSERT_EQ(static_cast<size_t>(1), entities.size());
        TEST_ASSERT_EQ(fixture.entity, entities[0]);

        sceneManager.Shutdown();
        return true;
    }

    bool Test_PrimitiveUnregisterImmediatelyFallsBackToEntityBounds()
    {
        SceneManager sceneManager;
        sceneManager.Initialize();

        auto fixture = CreatePrimitive(sceneManager, "ImmediateUnregisterFallback", Vec3(0.0f));
        fixture.entity->SetLocalBounds(AABB(Vec3(-2.0f), Vec3(2.0f)));
        sceneManager.Update(0.0f);
        sceneManager.RebuildSpatialIndex();

        TEST_ASSERT_TRUE(static_cast<Actor*>(fixture.entity)->RemoveComponent<StaticMeshComponent>());

        std::vector<PrimitiveComponent*> primitives;
        sceneManager.QueryBoxPrimitives(AABB(Vec3(-3.0f), Vec3(3.0f)), primitives);
        TEST_ASSERT_TRUE(primitives.empty());

        std::vector<SceneEntity*> entities;
        sceneManager.QueryBox(AABB(Vec3(-3.0f), Vec3(3.0f)), entities);
        TEST_ASSERT_EQ(static_cast<size_t>(1), entities.size());
        TEST_ASSERT_EQ(fixture.entity, entities[0]);

        RaycastHit hit;
        TEST_ASSERT_TRUE(sceneManager.Raycast(Ray(Vec3(0.0f, 0.0f, 10.0f),
                                               Vec3(0.0f, 0.0f, -1.0f)),
                                           hit));
        TEST_ASSERT_EQ(fixture.entity, hit.entity);
        TEST_ASSERT_EQ(nullptr, hit.component);

        sceneManager.Shutdown();
        return true;
    }

    bool Test_MultiplePrimitivesDeduplicateEntityQueries()
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
        TEST_ASSERT_EQ(static_cast<size_t>(2), primitives.size());

        std::vector<SceneEntity*> entities;
        sceneManager.QueryBox(AABB(Vec3(-2.0f), Vec3(5.0f)), entities);
        TEST_ASSERT_EQ(static_cast<size_t>(1), entities.size());
        TEST_ASSERT_EQ(fixture.entity, entities[0]);

        sceneManager.Shutdown();
        return true;
    }

    bool Test_WorldSpatialSubsystemAndPickingReturnComponentHit()
    {
        World world;
        world.Initialize();

        auto* sceneManager = world.GetSceneManager();
        auto fixture = CreatePrimitive(*sceneManager, "WorldPrimitive", Vec3(0.0f));

        auto* spatial = world.GetSpatial();
        TEST_ASSERT_NOT_NULL(spatial);
        spatial->RebuildIndex();

        Ray ray(Vec3(0.0f, 0.0f, 10.0f), Vec3(0.0f, 0.0f, -1.0f));
        RaycastHit spatialHit;
        TEST_ASSERT_TRUE(spatial->Raycast(ray, spatialHit));
        TEST_ASSERT_EQ(fixture.entity, spatialHit.entity);
        TEST_ASSERT_EQ(static_cast<Actor*>(fixture.entity), spatialHit.actor);
        TEST_ASSERT_EQ(static_cast<PrimitiveComponent*>(fixture.primitive), spatialHit.component);

        PickingService picking(&world);
        RaycastHit pickHit;
        TEST_ASSERT_TRUE(picking.Pick(ray, pickHit));
        TEST_ASSERT_EQ(fixture.entity, pickHit.entity);
        TEST_ASSERT_EQ(static_cast<PrimitiveComponent*>(fixture.primitive), pickHit.component);

        world.Shutdown();
        return true;
    }

    bool Test_WorldSpatialSubsystemLegacyEntityDoesNotRequireUserData()
    {
        World world;
        world.Initialize();

        auto* sceneManager = world.GetSceneManager();
        const auto handle = sceneManager->CreateEntity("WorldLegacySpatial");
        auto* entity = sceneManager->GetEntity(handle);
        entity->SetLocalBounds(AABB(Vec3(-1.0f), Vec3(1.0f)));
        entity->SetUserData(nullptr);

        auto* spatial = world.GetSpatial();
        TEST_ASSERT_NOT_NULL(spatial);
        spatial->RebuildIndex();

        RaycastHit hit;
        TEST_ASSERT_TRUE(spatial->Raycast(Ray(Vec3(0.0f, 0.0f, 10.0f),
                                             Vec3(0.0f, 0.0f, -1.0f)),
                                         hit));
        TEST_ASSERT_EQ(entity, hit.entity);
        TEST_ASSERT_EQ(static_cast<Actor*>(entity), hit.actor);
        TEST_ASSERT_EQ(nullptr, hit.component);

        world.Shutdown();
        return true;
    }

    bool Test_WorldEntityQueriesDeduplicateMultiPrimitiveActors()
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
        TEST_ASSERT_NOT_NULL(spatial);
        spatial->RebuildIndex();

        std::vector<SceneEntity*> entities;
        spatial->QueryBox(AABB(Vec3(-2.0f), Vec3(5.0f)), entities);
        TEST_ASSERT_EQ(static_cast<size_t>(1), entities.size());
        TEST_ASSERT_EQ(fixture.entity, entities[0]);

        world.Shutdown();
        return true;
    }

    bool Test_WorldRetiredPrimitiveProxyIsSafeUntilRebuild()
    {
        World world;
        world.Initialize();

        auto* sceneManager = world.GetSceneManager();
        auto fixture = CreatePrimitive(*sceneManager, "WorldRetiredPrimitive", Vec3(0.0f));
        fixture.entity->SetLocalBounds(AABB(Vec3(-2.0f), Vec3(2.0f)));

        auto* spatial = world.GetSpatial();
        TEST_ASSERT_NOT_NULL(spatial);
        spatial->RebuildIndex();

        TEST_ASSERT_TRUE(static_cast<Actor*>(fixture.entity)->RemoveComponent<StaticMeshComponent>());

        std::vector<SceneEntity*> staleEntities;
        spatial->QueryBox(AABB(Vec3(-3.0f), Vec3(3.0f)), staleEntities);
        TEST_ASSERT_TRUE(staleEntities.empty());

        spatial->RebuildIndex();
        std::vector<SceneEntity*> rebuiltEntities;
        spatial->QueryBox(AABB(Vec3(-3.0f), Vec3(3.0f)), rebuiltEntities);
        TEST_ASSERT_EQ(static_cast<size_t>(1), rebuiltEntities.size());
        TEST_ASSERT_EQ(fixture.entity, rebuiltEntities[0]);

        world.Shutdown();
        return true;
    }
} // namespace

int main()
{
    Log::Initialize();
    RVX_CORE_INFO("Spatial Component Validation Tests");

    TestSuite suite;
    suite.AddTest("PrimitiveRaycastReturnsActorAndComponent",
                  Test_PrimitiveRaycastReturnsActorAndComponent);
    suite.AddTest("PrimitiveBoxQueryReturnsPrimitiveAndDeduplicatedEntity",
                  Test_PrimitiveBoxQueryReturnsPrimitiveAndDeduplicatedEntity);
    suite.AddTest("LegacyEntitySpatialQueriesStillWork",
                  Test_LegacyEntitySpatialQueriesStillWork);
    suite.AddTest("PrimitiveLayerFilterAffectsSpatialQueries",
                  Test_PrimitiveLayerFilterAffectsSpatialQueries);
    suite.AddTest("PrimitiveCustomAndTypeFiltersUseSpatialEntityContract",
                  Test_PrimitiveCustomAndTypeFiltersUseSpatialEntityContract);
    suite.AddTest("NonIndexablePrimitiveFallsBackToLegacyEntityBounds",
                  Test_NonIndexablePrimitiveFallsBackToLegacyEntityBounds);
    suite.AddTest("PrimitiveDisableThroughBasePointerUpdatesSpatialIndex",
                  Test_PrimitiveDisableThroughBasePointerUpdatesSpatialIndex);
    suite.AddTest("PrimitiveUnregisterImmediatelyFallsBackToEntityBounds",
                  Test_PrimitiveUnregisterImmediatelyFallsBackToEntityBounds);
    suite.AddTest("MultiplePrimitivesDeduplicateEntityQueries",
                  Test_MultiplePrimitivesDeduplicateEntityQueries);
    suite.AddTest("WorldSpatialSubsystemAndPickingReturnComponentHit",
                  Test_WorldSpatialSubsystemAndPickingReturnComponentHit);
    suite.AddTest("WorldSpatialSubsystemLegacyEntityDoesNotRequireUserData",
                  Test_WorldSpatialSubsystemLegacyEntityDoesNotRequireUserData);
    suite.AddTest("WorldEntityQueriesDeduplicateMultiPrimitiveActors",
                  Test_WorldEntityQueriesDeduplicateMultiPrimitiveActors);
    suite.AddTest("WorldRetiredPrimitiveProxyIsSafeUntilRebuild",
                  Test_WorldRetiredPrimitiveProxyIsSafeUntilRebuild);

    auto results = suite.Run();
    suite.PrintResults(results);

    Log::Shutdown();

    for (const auto& result : results)
    {
        if (!result.passed)
            return 1;
    }
    return 0;
}
