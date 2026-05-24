#include "Core/Core.h"
#include "Scene/Actor.h"
#include "Scene/Components/StaticMeshComponent.h"
#include "Scene/PrimitiveComponent.h"
#include "Scene/SceneManager.h"
#include "World/PickingService.h"
#include "World/SpatialSubsystem.h"
#include "World/World.h"

#include <gtest/gtest.h>

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

    TEST(SpatialComponentValidation, WorldSpatialSubsystemAndPickingReturnComponentHit)
    {
        World world;
        world.Initialize();

        auto* sceneManager = world.GetSceneManager();
        auto fixture = CreatePrimitive(*sceneManager, "WorldPrimitive", Vec3(0.0f));

        auto* spatial = world.GetSpatial();
        ASSERT_NE(nullptr, spatial);
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

    TEST(SpatialComponentValidation, WorldRetiredPrimitiveProxyIsSafeUntilRebuild)
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

        std::vector<SceneEntity*> staleEntities;
        spatial->QueryBox(AABB(Vec3(-3.0f), Vec3(3.0f)), staleEntities);
        EXPECT_TRUE(staleEntities.empty());

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
