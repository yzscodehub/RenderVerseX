#include "Core/Core.h"
#include "Render/Renderer/RenderDrawItem.h"
#include "Render/Renderer/RenderScene.h"
#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/MeshResource.h"
#include "Runtime/Camera/Camera.h"
#include "Scene/Actor.h"
#include "Scene/Components/MeshRendererComponent.h"
#include "Scene/Components/StaticMeshComponent.h"
#include "Scene/Mesh.h"
#include "Scene/SceneManager.h"
#include "World/World.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <memory>
#include <vector>

using namespace RVX;

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
        resource->SetMaterialData(std::make_shared<Material>());
        resource->MarkLoaded();
        return Resource::ResourceHandle<Resource::MaterialResource>(resource);
    }

    Resource::ResourceHandle<Resource::MaterialResource> MakeMaterialResource(Resource::ResourceId id,
                                                                             Material::AlphaMode alphaMode)
    {
        auto resource = MakeMaterialResource(id);
        resource->GetMaterial()->SetAlphaMode(alphaMode);
        return resource;
    }

    RenderObject MakeMaterialObject(uint64 meshId,
                                    std::initializer_list<Resource::MaterialResource*> materials,
                                    const Vec3& position = Vec3(0.0f))
    {
        RenderObject object = MakeObject(position);
        object.meshId = meshId;
        object.worldMatrix = Mat4Identity();
        object.worldMatrix[3] = Vec4(position, 1.0f);

        for (Resource::MaterialResource* material : materials)
        {
            object.materialResources.push_back(material);
            object.materialIds.push_back(material ? material->GetId() : 0);
        }

        return object;
    }

    SceneEntity* CreateEntity(World& world, const std::string& name)
    {
        auto* sceneManager = world.GetSceneManager();
        const auto handle = sceneManager->CreateEntity(name);
        return sceneManager->GetEntity(handle);
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

TEST(RenderSceneValidation, CullAgainstCameraRejectsOutsideObjects)
{
    RenderScene scene;
    scene.AddObject(MakeObject(Vec3(0.0f, 0.0f, 0.0f)));
    scene.AddObject(MakeObject(Vec3(100.0f, 0.0f, 0.0f)));

    std::vector<uint32_t> visibleIndices;
    scene.CullAgainstCamera(MakeTestCamera(), visibleIndices);

    EXPECT_EQ(visibleIndices.size(), 1);
    EXPECT_EQ(visibleIndices[0], 0u);
}

TEST(RenderSceneValidation, CullAgainstCameraHonorsVisibilityFlag)
{
    RenderScene scene;
    RenderObject hidden = MakeObject(Vec3(0.0f, 0.0f, 0.0f));
    hidden.visible = false;
    scene.AddObject(hidden);

    std::vector<uint32_t> visibleIndices;
    scene.CullAgainstCamera(MakeTestCamera(), visibleIndices);

    EXPECT_TRUE(visibleIndices.empty());
}

TEST(RenderSceneValidation, BuildMaterialDrawListsRoutesMaterialModesAndPreservesIdentity)
{
    auto opaqueMaterial = MakeMaterialResource(2101, Material::AlphaMode::Opaque);
    auto maskedMaterial = MakeMaterialResource(2102, Material::AlphaMode::Mask);
    auto transparentMaterial = MakeMaterialResource(2103, Material::AlphaMode::Blend);

    RenderScene scene;
    scene.AddObject(MakeMaterialObject(3101,
                                       {opaqueMaterial.Get(), maskedMaterial.Get(), transparentMaterial.Get()}));

    std::vector<uint32_t> visibleIndices = {0};
    std::vector<RenderDrawItem> opaqueItems;
    std::vector<RenderDrawItem> maskedItems;
    std::vector<RenderDrawItem> transparentItems;

    BuildMaterialDrawLists(scene, visibleIndices, Vec3(0.0f, 0.0f, 5.0f),
                           opaqueItems, maskedItems, transparentItems);

    ASSERT_EQ(static_cast<size_t>(1), opaqueItems.size());
    ASSERT_EQ(static_cast<size_t>(1), maskedItems.size());
    ASSERT_EQ(static_cast<size_t>(1), transparentItems.size());

    EXPECT_EQ(0u, opaqueItems[0].objectIndex);
    EXPECT_EQ(0u, opaqueItems[0].submeshIndex);
    EXPECT_EQ(3101u, opaqueItems[0].meshId);
    EXPECT_EQ(opaqueMaterial.GetId(), opaqueItems[0].materialId);
    EXPECT_EQ(opaqueMaterial.Get(), opaqueItems[0].materialResource);
    EXPECT_EQ(MaterialRenderMode::Opaque, opaqueItems[0].renderMode);

    EXPECT_EQ(0u, maskedItems[0].objectIndex);
    EXPECT_EQ(1u, maskedItems[0].submeshIndex);
    EXPECT_EQ(3101u, maskedItems[0].meshId);
    EXPECT_EQ(maskedMaterial.GetId(), maskedItems[0].materialId);
    EXPECT_EQ(maskedMaterial.Get(), maskedItems[0].materialResource);
    EXPECT_EQ(MaterialRenderMode::Masked, maskedItems[0].renderMode);

    EXPECT_EQ(0u, transparentItems[0].objectIndex);
    EXPECT_EQ(2u, transparentItems[0].submeshIndex);
    EXPECT_EQ(3101u, transparentItems[0].meshId);
    EXPECT_EQ(transparentMaterial.GetId(), transparentItems[0].materialId);
    EXPECT_EQ(transparentMaterial.Get(), transparentItems[0].materialResource);
    EXPECT_EQ(MaterialRenderMode::Transparent, transparentItems[0].renderMode);
}

TEST(RenderSceneValidation, BuildMaterialDrawListsSortsTransparentBackToFront)
{
    auto transparentMaterial = MakeMaterialResource(2201, Material::AlphaMode::Blend);

    RenderScene scene;
    scene.AddObject(MakeMaterialObject(3201, {transparentMaterial.Get()}, Vec3(0.0f, 0.0f, 2.0f)));
    scene.AddObject(MakeMaterialObject(3202, {transparentMaterial.Get()}, Vec3(0.0f, 0.0f, 9.0f)));

    std::vector<uint32_t> visibleIndices = {0, 1};
    std::vector<RenderDrawItem> opaqueItems;
    std::vector<RenderDrawItem> maskedItems;
    std::vector<RenderDrawItem> transparentItems;

    BuildMaterialDrawLists(scene, visibleIndices, Vec3(0.0f),
                           opaqueItems, maskedItems, transparentItems);

    EXPECT_TRUE(opaqueItems.empty());
    EXPECT_TRUE(maskedItems.empty());
    ASSERT_EQ(static_cast<size_t>(2), transparentItems.size());
    EXPECT_EQ(1u, transparentItems[0].objectIndex);
    EXPECT_EQ(0u, transparentItems[1].objectIndex);
    EXPECT_GT(transparentItems[0].depthFromCamera, transparentItems[1].depthFromCamera);
}

TEST(RenderSceneValidation, StaticMeshComponentCollectsRenderObjectFromWorld)
{
    World world;
    world.Initialize();

    auto* entity = CreateEntity(world, "PrimitiveEntity");
    ASSERT_NE(nullptr, entity);
    entity->SetPosition(Vec3(4.0f, 0.0f, 0.0f));

    auto mesh = MakeMeshResource(1001);
    auto material = MakeMaterialResource(2001);

    auto* primitive = static_cast<Actor*>(entity)->AddComponent<StaticMeshComponent>();
    ASSERT_NE(nullptr, primitive);
    EXPECT_TRUE(primitive->AttachToComponent(entity->GetRootComponent()));
    primitive->SetMesh(mesh);
    primitive->SetMaterial(0, material);
    primitive->SetCastsShadow(false);
    primitive->SetReceivesShadow(false);

    RenderScene scene;
    scene.CollectFromWorld(&world);

    EXPECT_EQ(static_cast<size_t>(1), scene.GetObjectCount());
    const auto& object = scene.GetObject(0);
    EXPECT_EQ(entity->GetHandle(), object.entityId);
    EXPECT_EQ(mesh.GetId(), object.meshId);
    EXPECT_EQ(mesh.Get(), object.meshResource);
    EXPECT_EQ(static_cast<size_t>(1), object.materialIds.size());
    EXPECT_EQ(material.GetId(), object.materialIds[0]);
    EXPECT_EQ(material.Get(), object.materialResources[0]);
    EXPECT_FALSE(object.castsShadow);
    EXPECT_FALSE(object.receivesShadow);
    EXPECT_EQ(Vec3(4.0f, 0.0f, 0.0f), Vec3(object.worldMatrix[3]));

    world.Shutdown();
}

TEST(RenderSceneValidation, RenderSceneCollectorFallsBackToLegacyWhenStaticMeshPrimitiveHasNoRenderData)
{
    World world;
    world.Initialize();

    auto* entity = CreateEntity(world, "LegacyFallbackEntity");
    ASSERT_NE(nullptr, entity);

    auto mesh = MakeMeshResource(1002);
    auto* legacyRenderer = entity->AddComponent<MeshRendererComponent>();
    legacyRenderer->SetMesh(mesh);

    auto* emptyPrimitive = static_cast<Actor*>(entity)->AddComponent<StaticMeshComponent>();
    ASSERT_NE(nullptr, emptyPrimitive);
    EXPECT_TRUE(emptyPrimitive->IsRegistered());
    EXPECT_FALSE(emptyPrimitive->HasRenderData());

    RenderScene scene;
    scene.CollectFromWorld(&world);

    EXPECT_EQ(static_cast<size_t>(1), scene.GetObjectCount());
    EXPECT_EQ(entity->GetHandle(), scene.GetObject(0).entityId);
    EXPECT_EQ(mesh.GetId(), scene.GetObject(0).meshId);

    world.Shutdown();
}

TEST(RenderSceneValidation, RenderSceneCollectorDoesNotDuplicateWhenStaticMeshPrimitiveRenders)
{
    World world;
    world.Initialize();

    auto* entity = CreateEntity(world, "DedupEntity");
    ASSERT_NE(nullptr, entity);

    auto mesh = MakeMeshResource(1003);
    auto* legacyRenderer = entity->AddComponent<MeshRendererComponent>();
    legacyRenderer->SetMesh(mesh);

    auto* primitive = static_cast<Actor*>(entity)->AddComponent<StaticMeshComponent>();
    ASSERT_NE(nullptr, primitive);
    EXPECT_TRUE(primitive->AttachToComponent(entity->GetRootComponent()));
    primitive->SetMesh(mesh);

    RenderScene scene;
    scene.CollectFromWorld(&world);

    EXPECT_EQ(static_cast<size_t>(1), scene.GetObjectCount());
    EXPECT_EQ(entity->GetHandle(), scene.GetObject(0).entityId);
    EXPECT_EQ(mesh.GetId(), scene.GetObject(0).meshId);

    world.Shutdown();
}

TEST(RenderSceneValidation, RenderSceneCollectorSkipsInactivePrimitiveOwners)
{
    World world;
    world.Initialize();

    auto* entity = CreateEntity(world, "InactivePrimitiveEntity");
    ASSERT_NE(nullptr, entity);

    auto mesh = MakeMeshResource(1004);
    auto* primitive = static_cast<Actor*>(entity)->AddComponent<StaticMeshComponent>();
    ASSERT_NE(nullptr, primitive);
    EXPECT_TRUE(primitive->AttachToComponent(entity->GetRootComponent()));
    primitive->SetMesh(mesh);
    entity->SetActive(false);

    RenderScene scene;
    scene.CollectFromWorld(&world);

    EXPECT_EQ(static_cast<size_t>(0), scene.GetObjectCount());

    world.Shutdown();
}

TEST(RenderSceneValidation, RenderSceneCollectorDoesNotFallbackWhenPrimitiveIsHidden)
{
    World world;
    world.Initialize();

    auto* entity = CreateEntity(world, "HiddenPrimitiveEntity");
    ASSERT_NE(nullptr, entity);

    auto mesh = MakeMeshResource(1005);
    auto* legacyRenderer = entity->AddComponent<MeshRendererComponent>();
    legacyRenderer->SetMesh(mesh);

    auto* primitive = static_cast<Actor*>(entity)->AddComponent<StaticMeshComponent>();
    ASSERT_NE(nullptr, primitive);
    EXPECT_TRUE(primitive->AttachToComponent(entity->GetRootComponent()));
    primitive->SetMesh(mesh);
    primitive->SetVisible(false);

    RenderScene scene;
    scene.CollectFromWorld(&world);

    EXPECT_EQ(static_cast<size_t>(0), scene.GetObjectCount());

    world.Shutdown();
}
