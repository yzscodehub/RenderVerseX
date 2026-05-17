#include "Core/Core.h"
#include "Render/Renderer/RenderScene.h"
#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/MeshResource.h"
#include "Runtime/Camera/Camera.h"
#include "Scene/Actor.h"
#include "Scene/Components/MeshRendererComponent.h"
#include "Scene/Components/StaticMeshComponent.h"
#include "Scene/Mesh.h"
#include "Scene/SceneManager.h"
#include "TestFramework/TestRunner.h"
#include "World/World.h"

#include <memory>
#include <vector>

using namespace RVX;
using namespace RVX::Test;

namespace
{
    class TestMeshResource : public Resource::MeshResource
    {
    public:
        void MarkLoaded()
        {
            SetState(Resource::ResourceState::Loaded);
        }
    };

    class TestMaterialResource : public Resource::MaterialResource
    {
    public:
        void MarkLoaded()
        {
            SetState(Resource::ResourceState::Loaded);
        }
    };

    RenderObject MakeObject(const Vec3& center, float extent = 0.5f)
    {
        RenderObject object;
        object.bounds = AABB(center - Vec3(extent), center + Vec3(extent));
        object.visible = true;
        return object;
    }

    Camera MakeTestCamera()
    {
        Camera camera;
        camera.SetPerspective(1.04719755f, 1.0f, 0.1f, 100.0f);
        camera.SetPosition(Vec3(0.0f, 0.0f, 5.0f));
        camera.LookAt(Vec3(0.0f, 0.0f, 0.0f));
        return camera;
    }

    Resource::ResourceHandle<Resource::MeshResource> MakeMeshResource(Resource::ResourceId id)
    {
        auto* resource = new TestMeshResource();
        resource->SetId(id);
        resource->SetMesh(MeshFactory::CreateTriangle());
        resource->SetBounds(AABB(Vec3(-1.0f), Vec3(1.0f)));
        resource->MarkLoaded();
        return Resource::ResourceHandle<Resource::MeshResource>(resource);
    }

    Resource::ResourceHandle<Resource::MaterialResource> MakeMaterialResource(Resource::ResourceId id)
    {
        auto* resource = new TestMaterialResource();
        resource->SetId(id);
        resource->MarkLoaded();
        return Resource::ResourceHandle<Resource::MaterialResource>(resource);
    }

    SceneEntity* CreateEntity(World& world, const std::string& name)
    {
        auto* sceneManager = world.GetSceneManager();
        const auto handle = sceneManager->CreateEntity(name);
        return sceneManager->GetEntity(handle);
    }
} // namespace

bool Test_CullAgainstCameraRejectsOutsideObjects()
{
    RenderScene scene;
    scene.AddObject(MakeObject(Vec3(0.0f, 0.0f, 0.0f)));
    scene.AddObject(MakeObject(Vec3(100.0f, 0.0f, 0.0f)));

    std::vector<uint32_t> visibleIndices;
    scene.CullAgainstCamera(MakeTestCamera(), visibleIndices);

    TEST_ASSERT_EQ(visibleIndices.size(), 1);
    TEST_ASSERT_EQ(visibleIndices[0], 0u);
    return true;
}

bool Test_CullAgainstCameraHonorsVisibilityFlag()
{
    RenderScene scene;
    RenderObject hidden = MakeObject(Vec3(0.0f, 0.0f, 0.0f));
    hidden.visible = false;
    scene.AddObject(hidden);

    std::vector<uint32_t> visibleIndices;
    scene.CullAgainstCamera(MakeTestCamera(), visibleIndices);

    TEST_ASSERT_TRUE(visibleIndices.empty());
    return true;
}

bool Test_StaticMeshComponentCollectsRenderObjectFromWorld()
{
    World world;
    world.Initialize();

    auto* entity = CreateEntity(world, "PrimitiveEntity");
    TEST_ASSERT_NOT_NULL(entity);
    entity->SetPosition(Vec3(4.0f, 0.0f, 0.0f));

    auto mesh = MakeMeshResource(1001);
    auto material = MakeMaterialResource(2001);

    auto* primitive = static_cast<Actor*>(entity)->AddComponent<StaticMeshComponent>();
    TEST_ASSERT_NOT_NULL(primitive);
    TEST_ASSERT_TRUE(primitive->AttachToComponent(entity->GetRootComponent()));
    primitive->SetMesh(mesh);
    primitive->SetMaterial(0, material);
    primitive->SetCastsShadow(false);
    primitive->SetReceivesShadow(false);

    RenderScene scene;
    scene.CollectFromWorld(&world);

    TEST_ASSERT_EQ(static_cast<size_t>(1), scene.GetObjectCount());
    const auto& object = scene.GetObject(0);
    TEST_ASSERT_EQ(entity->GetHandle(), object.entityId);
    TEST_ASSERT_EQ(mesh.GetId(), object.meshId);
    TEST_ASSERT_EQ(mesh.Get(), object.meshResource);
    TEST_ASSERT_EQ(static_cast<size_t>(1), object.materialIds.size());
    TEST_ASSERT_EQ(material.GetId(), object.materialIds[0]);
    TEST_ASSERT_EQ(material.Get(), object.materialResources[0]);
    TEST_ASSERT_FALSE(object.castsShadow);
    TEST_ASSERT_FALSE(object.receivesShadow);
    TEST_ASSERT_EQ(Vec3(4.0f, 0.0f, 0.0f), Vec3(object.worldMatrix[3]));

    world.Shutdown();
    return true;
}

bool Test_RenderSceneCollectorFallsBackToLegacyWhenStaticMeshPrimitiveHasNoRenderData()
{
    World world;
    world.Initialize();

    auto* entity = CreateEntity(world, "LegacyFallbackEntity");
    TEST_ASSERT_NOT_NULL(entity);

    auto mesh = MakeMeshResource(1002);
    auto* legacyRenderer = entity->AddComponent<MeshRendererComponent>();
    legacyRenderer->SetMesh(mesh);

    auto* emptyPrimitive = static_cast<Actor*>(entity)->AddComponent<StaticMeshComponent>();
    TEST_ASSERT_NOT_NULL(emptyPrimitive);
    TEST_ASSERT_TRUE(emptyPrimitive->IsRegistered());
    TEST_ASSERT_FALSE(emptyPrimitive->HasRenderData());

    RenderScene scene;
    scene.CollectFromWorld(&world);

    TEST_ASSERT_EQ(static_cast<size_t>(1), scene.GetObjectCount());
    TEST_ASSERT_EQ(entity->GetHandle(), scene.GetObject(0).entityId);
    TEST_ASSERT_EQ(mesh.GetId(), scene.GetObject(0).meshId);

    world.Shutdown();
    return true;
}

bool Test_RenderSceneCollectorDoesNotDuplicateWhenStaticMeshPrimitiveRenders()
{
    World world;
    world.Initialize();

    auto* entity = CreateEntity(world, "DedupEntity");
    TEST_ASSERT_NOT_NULL(entity);

    auto mesh = MakeMeshResource(1003);
    auto* legacyRenderer = entity->AddComponent<MeshRendererComponent>();
    legacyRenderer->SetMesh(mesh);

    auto* primitive = static_cast<Actor*>(entity)->AddComponent<StaticMeshComponent>();
    TEST_ASSERT_NOT_NULL(primitive);
    TEST_ASSERT_TRUE(primitive->AttachToComponent(entity->GetRootComponent()));
    primitive->SetMesh(mesh);

    RenderScene scene;
    scene.CollectFromWorld(&world);

    TEST_ASSERT_EQ(static_cast<size_t>(1), scene.GetObjectCount());
    TEST_ASSERT_EQ(entity->GetHandle(), scene.GetObject(0).entityId);
    TEST_ASSERT_EQ(mesh.GetId(), scene.GetObject(0).meshId);

    world.Shutdown();
    return true;
}

bool Test_RenderSceneCollectorSkipsInactivePrimitiveOwners()
{
    World world;
    world.Initialize();

    auto* entity = CreateEntity(world, "InactivePrimitiveEntity");
    TEST_ASSERT_NOT_NULL(entity);

    auto mesh = MakeMeshResource(1004);
    auto* primitive = static_cast<Actor*>(entity)->AddComponent<StaticMeshComponent>();
    TEST_ASSERT_NOT_NULL(primitive);
    TEST_ASSERT_TRUE(primitive->AttachToComponent(entity->GetRootComponent()));
    primitive->SetMesh(mesh);
    entity->SetActive(false);

    RenderScene scene;
    scene.CollectFromWorld(&world);

    TEST_ASSERT_EQ(static_cast<size_t>(0), scene.GetObjectCount());

    world.Shutdown();
    return true;
}

bool Test_RenderSceneCollectorDoesNotFallbackWhenPrimitiveIsHidden()
{
    World world;
    world.Initialize();

    auto* entity = CreateEntity(world, "HiddenPrimitiveEntity");
    TEST_ASSERT_NOT_NULL(entity);

    auto mesh = MakeMeshResource(1005);
    auto* legacyRenderer = entity->AddComponent<MeshRendererComponent>();
    legacyRenderer->SetMesh(mesh);

    auto* primitive = static_cast<Actor*>(entity)->AddComponent<StaticMeshComponent>();
    TEST_ASSERT_NOT_NULL(primitive);
    TEST_ASSERT_TRUE(primitive->AttachToComponent(entity->GetRootComponent()));
    primitive->SetMesh(mesh);
    primitive->SetVisible(false);

    RenderScene scene;
    scene.CollectFromWorld(&world);

    TEST_ASSERT_EQ(static_cast<size_t>(0), scene.GetObjectCount());

    world.Shutdown();
    return true;
}

int main()
{
    Log::Initialize();
    RVX_CORE_INFO("RenderScene Validation Tests");

    TestSuite suite;
    suite.AddTest("CullAgainstCameraRejectsOutsideObjects", Test_CullAgainstCameraRejectsOutsideObjects);
    suite.AddTest("CullAgainstCameraHonorsVisibilityFlag", Test_CullAgainstCameraHonorsVisibilityFlag);
    suite.AddTest("StaticMeshComponentCollectsRenderObjectFromWorld",
                  Test_StaticMeshComponentCollectsRenderObjectFromWorld);
    suite.AddTest("RenderSceneCollectorFallsBackToLegacyWhenStaticMeshPrimitiveHasNoRenderData",
                  Test_RenderSceneCollectorFallsBackToLegacyWhenStaticMeshPrimitiveHasNoRenderData);
    suite.AddTest("RenderSceneCollectorDoesNotDuplicateWhenStaticMeshPrimitiveRenders",
                  Test_RenderSceneCollectorDoesNotDuplicateWhenStaticMeshPrimitiveRenders);
    suite.AddTest("RenderSceneCollectorSkipsInactivePrimitiveOwners",
                  Test_RenderSceneCollectorSkipsInactivePrimitiveOwners);
    suite.AddTest("RenderSceneCollectorDoesNotFallbackWhenPrimitiveIsHidden",
                  Test_RenderSceneCollectorDoesNotFallbackWhenPrimitiveIsHidden);

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
