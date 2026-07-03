#include "Animation/Data/Skeleton.h"
#include "Core/Core.h"
#include "Render/Renderer/RenderDrawItem.h"
#include "RenderContracts/RenderProxy.h"
#include "Render/Renderer/RenderScene.h"
#include "RHI/RHITexture.h"
#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/MeshResource.h"
#include "Runtime/Camera/Camera.h"
#include "Scene/Actor.h"
#include "Scene/Components/LightComponent.h"
#include "Scene/Components/MeshRendererComponent.h"
#include "Scene/Components/SkeletonComponent.h"
#include "Scene/Components/SkyboxComponent.h"
#include "Scene/Components/StaticMeshComponent.h"
#include "Scene/Mesh.h"
#include "Scene/SceneManager.h"
#include "World/World.h"

#include "RenderExtraction/RenderProxySceneBridge.h"
#include "RenderExtraction/SceneSkyboxPassBridge.h"

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

    class TestTextureResource : public Resource::TextureResource
    {
    public:
        void MarkLoaded()
        {
            SetState(Resource::ResourceState::Loaded);
        }
    };

    class FakeSkyboxTexture final : public RHITexture
    {
    public:
        uint32 GetWidth() const override { return 4; }
        uint32 GetHeight() const override { return 4; }
        uint32 GetDepth() const override { return 1; }
        uint32 GetMipLevels() const override { return 1; }
        uint32 GetArraySize() const override { return 1; }
        RHIFormat GetFormat() const override { return RHIFormat::RGBA8_UNORM; }
        RHITextureUsage GetUsage() const override { return RHITextureUsage::ShaderResource; }
        RHITextureDimension GetDimension() const override { return RHITextureDimension::TextureCube; }
        RHISampleCount GetSampleCount() const override { return RHISampleCount::Count1; }
    };

    struct FakeSkyboxPassTarget
    {
        SceneSkyboxPassActions MakeActions()
        {
            SceneSkyboxPassActions actions;
            actions.setProcedural =
                [this](const Vec3&,
                       const Vec3&,
                       const Vec3&,
                       const Vec3&,
                       const Vec3&,
                       float,
                       float)
                {
                    proceduralSet = true;
                    clearedReason.clear();
                };
            actions.setSolidColor =
                [this](const Vec3&, float)
                {
                    solidColorSet = true;
                    clearedReason.clear();
                };
            actions.setCubemap =
                [this](RHITexture* texture, float exposure, float rotation, float blurLevel)
                {
                    selectedCubemap = texture;
                    cubemapExposure = exposure;
                    cubemapRotation = rotation;
                    cubemapBlurLevel = blurLevel;
                    clearedReason.clear();
                };
            actions.clear =
                [this](const char* reason)
                {
                    selectedCubemap = nullptr;
                    clearedReason = reason ? reason : "";
                };
            return actions;
        }

        bool proceduralSet = false;
        bool solidColorSet = false;
        RHITexture* selectedCubemap = nullptr;
        float cubemapExposure = 0.0f;
        float cubemapRotation = 0.0f;
        float cubemapBlurLevel = 0.0f;
        std::string clearedReason;
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

    Resource::ResourceHandle<Resource::TextureResource> MakeCubemapTextureResource(Resource::ResourceId id)
    {
        auto* resource = new TestTextureResource();
        resource->SetId(id);

        Resource::TextureMetadata metadata;
        metadata.width = 1;
        metadata.height = 1;
        metadata.depth = 1;
        metadata.mipLevels = 1;
        metadata.arrayLayers = 6;
        metadata.format = Resource::TextureFormat::RGBA8;
        metadata.isCubemap = true;
        metadata.isArray = false;
        metadata.isSRGB = false;
        metadata.usage = Resource::TextureUsage::Color;
        resource->SetData(std::vector<uint8>(6u * 4u, 255u), metadata);
        resource->MarkLoaded();

        return Resource::ResourceHandle<Resource::TextureResource>(resource);
    }

    Animation::Skeleton::Ptr MakeTwoBoneSkeleton()
    {
        auto skeleton = Animation::Skeleton::Create();
        skeleton->AddBone("Root");
        skeleton->AddBone("Child", 0);
        skeleton->ComputeInverseBindPoses();
        return skeleton;
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
            if (!material)
            {
                object.materialModes.push_back(RenderMaterialMode::Opaque);
                continue;
            }

            switch (material->GetAlphaMode())
            {
                case Resource::MaterialAlphaMode::Mask:
                    object.materialModes.push_back(RenderMaterialMode::Masked);
                    break;
                case Resource::MaterialAlphaMode::Blend:
                    object.materialModes.push_back(RenderMaterialMode::Transparent);
                    break;
                case Resource::MaterialAlphaMode::Opaque:
                default:
                    object.materialModes.push_back(RenderMaterialMode::Opaque);
                    break;
            }
        }

        return object;
    }

    SceneEntity* CreateEntity(World& world, const std::string& name)
    {
        auto* sceneManager = world.GetSceneManager();
        const auto handle = sceneManager->CreateEntity(name);
        return sceneManager->GetEntity(handle);
    }

    class EmptyPrimitiveComponent : public PrimitiveComponent
    {
    public:
        const char* GetClassName() const override { return "EmptyPrimitiveComponent"; }
        bool HasRenderProxy() const override { return false; }
        bool HasRenderData() const override { return false; }
    };

    class LegacyOnlyPrimitiveComponent : public PrimitiveComponent
    {
    public:
        const char* GetClassName() const override { return "LegacyOnlyPrimitiveComponent"; }
        bool HasRenderProxy() const override { return false; }
        bool HasRenderData() const override { return true; }
    };

    class FailingProxyPrimitiveComponent : public PrimitiveComponent
    {
    public:
        const char* GetClassName() const override { return "FailingProxyPrimitiveComponent"; }
        bool HasRenderProxy() const override { return true; }
        bool HasRenderData() const override { return true; }
        bool CreateRenderProxy(RenderPrimitiveProxy& outProxy) const override
        {
            (void)outProxy;
            return false;
        }
    };

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

TEST(RenderSceneValidation, BuildMaterialDrawListsInfersMissingMaterialModesFromMaterialSource)
{
    auto maskedMaterial = MakeMaterialResource(2151, Material::AlphaMode::Mask);
    auto transparentMaterial = MakeMaterialResource(2152, Material::AlphaMode::Blend);

    RenderObject object = MakeMaterialObject(3151, {maskedMaterial.Get(), transparentMaterial.Get()});
    object.materialModes.clear();

    RenderScene scene;
    scene.AddObject(object);

    std::vector<uint32_t> visibleIndices = {0};
    std::vector<RenderDrawItem> opaqueItems;
    std::vector<RenderDrawItem> maskedItems;
    std::vector<RenderDrawItem> transparentItems;

    BuildMaterialDrawLists(scene,
                           visibleIndices,
                           Vec3(0.0f, 0.0f, 5.0f),
                           opaqueItems,
                           maskedItems,
                           transparentItems);

    EXPECT_TRUE(opaqueItems.empty());
    ASSERT_EQ(static_cast<size_t>(1), maskedItems.size());
    ASSERT_EQ(static_cast<size_t>(1), transparentItems.size());
    EXPECT_EQ(maskedMaterial.GetId(), maskedItems[0].materialId);
    EXPECT_EQ(maskedMaterial.Get(), maskedItems[0].materialResource);
    EXPECT_EQ(MaterialRenderMode::Masked, maskedItems[0].renderMode);
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
    primitive->SetLayerMask(0x34u);
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
    ASSERT_EQ(static_cast<size_t>(1), object.materialModes.size());
    EXPECT_EQ(RenderMaterialMode::Opaque, object.materialModes[0]);
    EXPECT_EQ(material.Get(), object.materialResources[0]);
    EXPECT_EQ(0x34u, object.layerMask);
    EXPECT_FALSE(object.castsShadow);
    EXPECT_FALSE(object.receivesShadow);
    EXPECT_EQ(Vec3(4.0f, 0.0f, 0.0f), Vec3(object.worldMatrix[3]));

    world.Shutdown();
}

TEST(RenderSceneValidation, StaticMeshComponentCollectsRenderObjectFromSceneManager)
{
    World world;
    world.Initialize();

    auto* entity = CreateEntity(world, "SceneManagerPrimitiveEntity");
    ASSERT_NE(nullptr, entity);
    entity->SetPosition(Vec3(2.0f, 0.0f, 0.0f));

    auto mesh = MakeMeshResource(1005);
    auto material = MakeMaterialResource(2005);

    auto* primitive = static_cast<Actor*>(entity)->AddComponent<StaticMeshComponent>();
    ASSERT_NE(nullptr, primitive);
    EXPECT_TRUE(primitive->AttachToComponent(entity->GetRootComponent()));
    primitive->SetMesh(mesh);
    primitive->SetMaterial(0, material);

    RenderScene worldScene;
    worldScene.CollectFromWorld(&world);

    RenderScene sceneManagerScene;
    sceneManagerScene.CollectFromSceneManager(world.GetSceneManager());

    ASSERT_EQ(worldScene.GetObjectCount(), sceneManagerScene.GetObjectCount());
    ASSERT_EQ(static_cast<size_t>(1), sceneManagerScene.GetObjectCount());
    const auto& object = sceneManagerScene.GetObject(0);
    EXPECT_EQ(entity->GetHandle(), object.entityId);
    EXPECT_EQ(mesh.GetId(), object.meshId);
    EXPECT_EQ(mesh.Get(), object.meshResource);
    ASSERT_EQ(static_cast<size_t>(1), object.materialIds.size());
    EXPECT_EQ(material.GetId(), object.materialIds[0]);
    EXPECT_EQ(Vec3(2.0f, 0.0f, 0.0f), Vec3(object.worldMatrix[3]));

    world.Shutdown();
}

TEST(RenderSceneValidation, StaticMeshComponentCreatesRenderProxy)
{
    World world;
    world.Initialize();

    auto* entity = CreateEntity(world, "ProxyPrimitiveEntity");
    ASSERT_NE(nullptr, entity);
    entity->SetPosition(Vec3(2.0f, 3.0f, 4.0f));

    auto mesh = MakeMeshResource(1101);
    auto material = MakeMaterialResource(2101);

    auto* primitive = static_cast<Actor*>(entity)->AddComponent<StaticMeshComponent>();
    ASSERT_NE(nullptr, primitive);
    EXPECT_TRUE(primitive->AttachToComponent(entity->GetRootComponent()));
    primitive->SetMesh(mesh);
    primitive->SetMaterial(0, material);
    primitive->SetLayerMask(0x10u);
    primitive->SetCastsShadow(false);
    primitive->SetReceivesShadow(false);

    RenderPrimitiveProxy proxy;
    ASSERT_TRUE(primitive->CreateRenderProxy(proxy));

    EXPECT_EQ(0u, proxy.ownerId);
    EXPECT_EQ(mesh.GetId(), proxy.meshId);
    EXPECT_EQ(mesh.Get(), proxy.meshResource);
    ASSERT_EQ(static_cast<size_t>(1), proxy.materialIds.size());
    ASSERT_EQ(static_cast<size_t>(1), proxy.materialModes.size());
    ASSERT_EQ(static_cast<size_t>(1), proxy.materialResources.size());
    EXPECT_EQ(material.GetId(), proxy.materialIds[0]);
    EXPECT_EQ(RenderMaterialMode::Opaque, proxy.materialModes[0]);
    EXPECT_EQ(material.Get(), proxy.materialResources[0]);
    EXPECT_EQ(0x10u, proxy.layerMask);
    EXPECT_FALSE(proxy.castsShadow);
    EXPECT_FALSE(proxy.receivesShadow);
    EXPECT_TRUE(proxy.visible);
    EXPECT_EQ(Vec3(2.0f, 3.0f, 4.0f), Vec3(proxy.worldMatrix[3]));

    world.Shutdown();
}

TEST(RenderSceneValidation, RenderSceneApplyProxySnapshotPopulatesObjectsAndLights)
{
    RenderProxySnapshot snapshot;
    snapshot.BeginBuild(77);

    RenderPrimitiveProxy primitive;
    primitive.ownerId = 42;
    primitive.worldMatrix = Mat4Identity();
    primitive.worldMatrix[3] = Vec4(1.0f, 2.0f, 3.0f, 1.0f);
    primitive.normalMatrix = Mat4Identity();
    primitive.bounds = AABB(Vec3(-1.0f), Vec3(1.0f));
    primitive.meshId = 3101;
    primitive.materialIds = {4101};
    primitive.skinningMatrices = {Mat4Identity(), Mat4Identity()};
    primitive.skinningMatrices[1][3] = Vec4(0.0f, 2.0f, 0.0f, 1.0f);
    primitive.materialModes = {RenderMaterialMode::Masked};
    primitive.sortKey = 4101;
    primitive.layerMask = 0x5Au;
    primitive.visible = true;
    primitive.castsShadow = false;
    primitive.receivesShadow = true;
    snapshot.primitives.push_back(primitive);

    RenderLightProxy light;
    light.ownerId = 43;
    light.type = RenderLightProxy::Type::Point;
    light.position = Vec3(0.0f, 4.0f, 0.0f);
    light.color = Vec3(1.0f, 0.5f, 0.25f);
    light.intensity = 3.0f;
    light.range = 12.0f;
    light.castsShadow = true;
    snapshot.lights.push_back(light);
    snapshot.MarkComplete();

    RenderScene scene;
    scene.AddObject(MakeObject(Vec3(99.0f)));
    scene.ApplyProxySnapshot(snapshot);

    ASSERT_EQ(static_cast<size_t>(1), scene.GetObjectCount());
    ASSERT_EQ(static_cast<size_t>(1), scene.GetLightCount());
    const RenderProxySnapshotMetadata& metadata = scene.GetSourceSnapshotMetadata();
    EXPECT_EQ(RVX_RENDER_PROXY_SNAPSHOT_SCHEMA_VERSION, metadata.schemaVersion);
    EXPECT_EQ(77u, metadata.sequence);
    EXPECT_EQ(RenderProxySnapshotStatus::Complete, metadata.status);
    EXPECT_TRUE(metadata.complete);
    EXPECT_EQ(static_cast<size_t>(1), metadata.primitiveCount);
    EXPECT_EQ(static_cast<size_t>(1), metadata.lightCount);

    const RenderObject& object = scene.GetObject(0);
    EXPECT_EQ(42u, object.entityId);
    EXPECT_EQ(3101u, object.meshId);
    EXPECT_EQ(Vec3(1.0f, 2.0f, 3.0f), Vec3(object.worldMatrix[3]));
    ASSERT_EQ(static_cast<size_t>(1), object.materialIds.size());
    EXPECT_EQ(4101u, object.materialIds[0]);
    ASSERT_TRUE(object.HasSkinningData());
    ASSERT_EQ(static_cast<size_t>(2), object.skinningMatrices.size());
    EXPECT_EQ(Vec3(0.0f, 2.0f, 0.0f), Vec3(object.skinningMatrices[1][3]));
    EXPECT_EQ(0x5Au, object.layerMask);
    ASSERT_EQ(static_cast<size_t>(1), object.materialModes.size());
    EXPECT_EQ(RenderMaterialMode::Masked, object.materialModes[0]);
    EXPECT_FALSE(object.castsShadow);
    EXPECT_TRUE(object.receivesShadow);

    const RenderLight& renderLight = scene.GetLight(0);
    EXPECT_EQ(RenderLight::Type::Point, renderLight.type);
    EXPECT_EQ(Vec3(0.0f, 4.0f, 0.0f), renderLight.position);
    EXPECT_EQ(Vec3(1.0f, 0.5f, 0.25f), renderLight.color);
    EXPECT_EQ(3.0f, renderLight.intensity);
    EXPECT_TRUE(renderLight.castsShadow);
}

TEST(RenderSceneValidation, RenderProxyBridgeBuildsPrimitiveAndLightSnapshot)
{
    World world;
    world.Initialize();

    auto* meshEntity = CreateEntity(world, "ProxyMeshEntity");
    ASSERT_NE(nullptr, meshEntity);
    meshEntity->SetPosition(Vec3(3.0f, 0.0f, 0.0f));

    auto mesh = MakeMeshResource(1201);
    auto material = MakeMaterialResource(2201);
    auto* primitive = static_cast<Actor*>(meshEntity)->AddComponent<StaticMeshComponent>();
    ASSERT_NE(nullptr, primitive);
    EXPECT_TRUE(primitive->AttachToComponent(meshEntity->GetRootComponent()));
    primitive->SetMesh(mesh);
    primitive->SetMaterial(0, material);

    auto* lightEntity = CreateEntity(world, "ProxyLightEntity");
    ASSERT_NE(nullptr, lightEntity);
    lightEntity->SetPosition(Vec3(0.0f, 5.0f, 0.0f));
    auto* light = lightEntity->AddComponent<LightComponent>();
    ASSERT_NE(nullptr, light);
    light->SetLightType(LightType::Point);
    light->SetColor(Vec3(0.25f, 0.5f, 1.0f));
    light->SetIntensity(2.0f);
    light->SetCastsShadow(true);

    RenderProxySceneBridge bridge;
    RenderProxySnapshot snapshot;
    RenderProxySceneBridgeResult result;
    ASSERT_TRUE(bridge.BuildSnapshot(&world, snapshot, &result));

    EXPECT_TRUE(result.usedProxyPath);
    EXPECT_FALSE(result.requiresLegacyFallback);
    EXPECT_EQ(RenderProxySceneBridgeFallbackReason::None, result.fallbackReason);
    EXPECT_EQ(static_cast<size_t>(1), result.primitiveCount);
    EXPECT_EQ(static_cast<size_t>(1), result.lightCount);

    ASSERT_EQ(static_cast<size_t>(1), snapshot.primitives.size());
    EXPECT_EQ(meshEntity->GetHandle(), snapshot.primitives[0].ownerId);
    EXPECT_EQ(meshEntity->GetHandle(), snapshot.primitives[0].id.value);
    EXPECT_EQ(mesh.GetId(), snapshot.primitives[0].meshId);
    EXPECT_EQ(material.GetId(), snapshot.primitives[0].materialIds[0]);
    ASSERT_EQ(static_cast<size_t>(1), snapshot.primitives[0].materialModes.size());
    EXPECT_EQ(RenderMaterialMode::Opaque, snapshot.primitives[0].materialModes[0]);

    ASSERT_EQ(static_cast<size_t>(1), snapshot.lights.size());
    EXPECT_EQ(lightEntity->GetHandle(), snapshot.lights[0].ownerId);
    EXPECT_EQ(RenderLightProxy::Type::Point, snapshot.lights[0].type);
    EXPECT_EQ(Vec3(0.0f, 5.0f, 0.0f), snapshot.lights[0].position);
    EXPECT_TRUE(snapshot.lights[0].castsShadow);

    world.Shutdown();
}

TEST(RenderSceneValidation, RenderProxyBridgePublishesCompleteSnapshotContract)
{
    World world;
    world.Initialize();

    auto* meshEntity = CreateEntity(world, "SnapshotContractMesh");
    ASSERT_NE(nullptr, meshEntity);
    auto mesh = MakeMeshResource(1202);
    auto material = MakeMaterialResource(2202);
    auto* primitive = static_cast<Actor*>(meshEntity)->AddComponent<StaticMeshComponent>();
    ASSERT_NE(nullptr, primitive);
    EXPECT_TRUE(primitive->AttachToComponent(meshEntity->GetRootComponent()));
    primitive->SetMesh(mesh);
    primitive->SetMaterial(0, material);

    auto* lightEntity = CreateEntity(world, "SnapshotContractLight");
    ASSERT_NE(nullptr, lightEntity);
    auto* light = lightEntity->AddComponent<LightComponent>();
    ASSERT_NE(nullptr, light);
    light->SetLightType(LightType::Directional);

    RenderProxySceneBridge bridge;
    RenderProxySnapshot firstSnapshot;
    RenderProxySceneBridgeResult firstResult;
    ASSERT_TRUE(bridge.BuildSnapshot(&world, firstSnapshot, &firstResult));

    EXPECT_EQ(RVX_RENDER_PROXY_SNAPSHOT_SCHEMA_VERSION, firstResult.snapshotSchemaVersion);
    EXPECT_GT(firstResult.snapshotSequence, 0u);
    EXPECT_TRUE(firstResult.snapshotComplete);
    EXPECT_EQ(static_cast<size_t>(1), firstResult.primitiveCount);
    EXPECT_EQ(static_cast<size_t>(1), firstResult.lightCount);

    const RenderProxySnapshotMetadata firstMetadata = firstSnapshot.GetMetadata();
    EXPECT_EQ(firstResult.snapshotSchemaVersion, firstMetadata.schemaVersion);
    EXPECT_EQ(firstResult.snapshotSequence, firstMetadata.sequence);
    EXPECT_TRUE(firstMetadata.complete);
    EXPECT_EQ(RenderProxySnapshotStatus::Complete, firstMetadata.status);
    EXPECT_EQ(firstResult.primitiveCount, firstMetadata.primitiveCount);
    EXPECT_EQ(firstResult.lightCount, firstMetadata.lightCount);

    RenderScene scene;
    scene.ApplyProxySnapshot(firstSnapshot);
    EXPECT_EQ(firstMetadata.sequence, scene.GetSourceSnapshotMetadata().sequence);
    EXPECT_TRUE(scene.GetSourceSnapshotMetadata().complete);
    EXPECT_EQ(firstMetadata.primitiveCount, scene.GetSourceSnapshotMetadata().primitiveCount);

    RenderProxySnapshot secondSnapshot;
    RenderProxySceneBridgeResult secondResult;
    ASSERT_TRUE(bridge.BuildSnapshot(&world, secondSnapshot, &secondResult));
    EXPECT_EQ(firstResult.snapshotSequence + 1u, secondResult.snapshotSequence);
    EXPECT_TRUE(secondSnapshot.GetMetadata().complete);

    world.Shutdown();
}

TEST(RenderSceneValidation, RenderProxyBridgePropagatesSkeletonSkinningMatrices)
{
    World world;
    world.Initialize();

    auto* meshEntity = CreateEntity(world, "SkinnedProxyMeshEntity");
    ASSERT_NE(nullptr, meshEntity);

    auto mesh = MakeMeshResource(1202);
    auto material = MakeMaterialResource(2202);
    auto* primitive = static_cast<Actor*>(meshEntity)->AddComponent<StaticMeshComponent>();
    ASSERT_NE(nullptr, primitive);
    EXPECT_TRUE(primitive->AttachToComponent(meshEntity->GetRootComponent()));
    primitive->SetMesh(mesh);
    primitive->SetMaterial(0, material);

    auto* skeleton = meshEntity->AddComponent<SkeletonComponent>();
    ASSERT_NE(nullptr, skeleton);
    skeleton->SetSkeleton(MakeTwoBoneSkeleton());
    skeleton->SetBoneLocalPosition(1, Vec3(0.0f, 2.0f, 0.0f));

    RenderProxySceneBridge bridge;
    RenderProxySnapshot snapshot;
    RenderProxySceneBridgeResult result;
    ASSERT_TRUE(bridge.BuildSnapshot(&world, snapshot, &result));

    ASSERT_EQ(static_cast<size_t>(1), snapshot.primitives.size());
    const RenderPrimitiveProxy& proxy = snapshot.primitives[0];
    ASSERT_TRUE(proxy.HasSkinningData());
    ASSERT_EQ(static_cast<size_t>(2), proxy.skinningMatrices.size());
    EXPECT_EQ(Vec3(0.0f, 2.0f, 0.0f), Vec3(proxy.skinningMatrices[1][3]));

    RenderScene scene;
    scene.ApplyProxySnapshot(snapshot);

    ASSERT_EQ(static_cast<size_t>(1), scene.GetObjectCount());
    const RenderObject& object = scene.GetObject(0);
    ASSERT_TRUE(object.HasSkinningData());
    ASSERT_EQ(static_cast<size_t>(2), object.skinningMatrices.size());
    EXPECT_EQ(Vec3(0.0f, 2.0f, 0.0f), Vec3(object.skinningMatrices[1][3]));

    world.Shutdown();
}

TEST(RenderSceneValidation, SceneSkyboxBridgeReportsMissingCubemapResource)
{
    World world;
    world.Initialize();

    auto* entity = CreateEntity(world, "MissingCubemapSkybox");
    ASSERT_NE(nullptr, entity);
    auto* skybox = entity->AddComponent<SkyboxComponent>();
    ASSERT_NE(nullptr, skybox);
    skybox->SetSkyboxType(SkyboxType::Cubemap);

    SceneSkyboxPassBridge bridge;
    FakeSkyboxPassTarget target;
    SceneSkyboxPassBridgeResult result;
    EXPECT_FALSE(bridge.Update(&world, target.MakeActions(), {}, &result));

    EXPECT_TRUE(result.skyboxFound);
    EXPECT_FALSE(result.uploadRequested);
    EXPECT_EQ(SceneSkyboxPassBridgeFallbackReason::SkyboxCubemapMissing, result.fallbackReason);
    EXPECT_EQ(std::string(ToString(result.fallbackReason)), target.clearedReason);
    EXPECT_EQ(nullptr, target.selectedCubemap);

    world.Shutdown();
}

TEST(RenderSceneValidation, SceneSkyboxBridgeRequestsUploadForNotReadyCubemap)
{
    World world;
    world.Initialize();

    auto* entity = CreateEntity(world, "NotReadyCubemapSkybox");
    ASSERT_NE(nullptr, entity);
    auto* skybox = entity->AddComponent<SkyboxComponent>();
    ASSERT_NE(nullptr, skybox);
    auto cubemap = MakeCubemapTextureResource(9101);
    skybox->SetCubemap(cubemap);

    bool uploadRequested = false;
    Resource::ResourceId uploadId = Resource::InvalidResourceId;
    SceneSkyboxTextureAccess textureAccess;
    textureAccess.requestUpload =
        [&uploadRequested, &uploadId](IRenderTextureUploadSource* texture)
        {
            uploadRequested = true;
            uploadId = texture ? texture->GetRenderResourceId() : Resource::InvalidResourceId;
        };
    textureAccess.isGPUReady =
        [](Resource::ResourceId) -> bool
        {
            return false;
        };
    textureAccess.getTexture =
        [](Resource::ResourceId) -> RHITexture*
        {
            return nullptr;
        };

    SceneSkyboxPassBridge bridge;
    FakeSkyboxPassTarget target;
    SceneSkyboxPassBridgeResult result;
    EXPECT_FALSE(bridge.Update(&world, target.MakeActions(), textureAccess, &result));

    EXPECT_TRUE(result.skyboxFound);
    EXPECT_TRUE(result.uploadRequested);
    EXPECT_TRUE(uploadRequested);
    EXPECT_EQ(cubemap.GetId(), uploadId);
    EXPECT_EQ(SceneSkyboxPassBridgeFallbackReason::SkyboxCubemapNotReady, result.fallbackReason);
    EXPECT_EQ(std::string(ToString(result.fallbackReason)), target.clearedReason);
    EXPECT_EQ(nullptr, target.selectedCubemap);

    world.Shutdown();
}

TEST(RenderSceneValidation, SceneSkyboxBridgePassesReadyCubemapToTarget)
{
    World world;
    world.Initialize();

    auto* entity = CreateEntity(world, "ReadyCubemapSkybox");
    ASSERT_NE(nullptr, entity);
    auto* skybox = entity->AddComponent<SkyboxComponent>();
    ASSERT_NE(nullptr, skybox);
    auto cubemap = MakeCubemapTextureResource(9102);
    skybox->SetCubemap(cubemap);
    skybox->SetExposure(1.5f);
    skybox->SetRotation(0.25f);
    skybox->SetBlurLevel(2.0f);

    FakeSkyboxTexture gpuCubemap;
    SceneSkyboxTextureAccess textureAccess;
    textureAccess.isGPUReady =
        [cubemap](Resource::ResourceId id) -> bool
        {
            return id == cubemap.GetId();
        };
    textureAccess.getTexture =
        [cubemap, &gpuCubemap](Resource::ResourceId id) -> RHITexture*
        {
            return id == cubemap.GetId() ? &gpuCubemap : nullptr;
        };

    SceneSkyboxPassBridge bridge;
    FakeSkyboxPassTarget target;
    SceneSkyboxPassBridgeResult result;
    EXPECT_TRUE(bridge.Update(&world, target.MakeActions(), textureAccess, &result));

    EXPECT_TRUE(result.skyboxFound);
    EXPECT_FALSE(result.uploadRequested);
    EXPECT_EQ(SceneSkyboxPassBridgeFallbackReason::None, result.fallbackReason);
    EXPECT_EQ(&gpuCubemap, result.selectedCubemap);
    EXPECT_EQ(&gpuCubemap, target.selectedCubemap);
    EXPECT_EQ(1.5f, target.cubemapExposure);
    EXPECT_EQ(0.25f, target.cubemapRotation);
    EXPECT_EQ(2.0f, target.cubemapBlurLevel);
    EXPECT_TRUE(target.clearedReason.empty());

    world.Shutdown();
}

TEST(RenderSceneValidation, SceneSkyboxBridgeKeepsEquirectangularUnsupported)
{
    World world;
    world.Initialize();

    auto* entity = CreateEntity(world, "EquirectangularSkybox");
    ASSERT_NE(nullptr, entity);
    auto* skybox = entity->AddComponent<SkyboxComponent>();
    ASSERT_NE(nullptr, skybox);
    skybox->SetSkyboxType(SkyboxType::Equirectangular);

    SceneSkyboxPassBridge bridge;
    FakeSkyboxPassTarget target;
    SceneSkyboxPassBridgeResult result;
    EXPECT_FALSE(bridge.Update(&world, target.MakeActions(), {}, &result));

    EXPECT_TRUE(result.skyboxFound);
    EXPECT_FALSE(result.uploadRequested);
    EXPECT_EQ(SceneSkyboxPassBridgeFallbackReason::SkyboxEquirectangularDrawingNotImplemented,
              result.fallbackReason);
    EXPECT_EQ(std::string(ToString(result.fallbackReason)), target.clearedReason);
    EXPECT_EQ(nullptr, target.selectedCubemap);

    world.Shutdown();
}

TEST(RenderSceneValidation, RenderProxyBridgeBuildsLegacyMeshRendererProxy)
{
    World world;
    world.Initialize();

    auto* entity = CreateEntity(world, "LegacyRendererEntity");
    ASSERT_NE(nullptr, entity);

    auto mesh = MakeMeshResource(1202);
    auto* legacyRenderer = entity->AddComponent<MeshRendererComponent>();
    ASSERT_NE(nullptr, legacyRenderer);
    legacyRenderer->SetMesh(mesh);

    RenderProxySceneBridge bridge;
    RenderProxySnapshot snapshot;
    RenderProxySceneBridgeResult result;
    EXPECT_TRUE(bridge.BuildSnapshot(&world, snapshot, &result));

    EXPECT_TRUE(result.usedProxyPath);
    EXPECT_FALSE(result.requiresLegacyFallback);
    EXPECT_EQ(RenderProxySceneBridgeFallbackReason::None, result.fallbackReason);
    ASSERT_EQ(static_cast<size_t>(1), snapshot.primitives.size());
    EXPECT_EQ(entity->GetHandle(), snapshot.primitives[0].ownerId);
    EXPECT_EQ(mesh.GetId(), snapshot.primitives[0].meshId);
    EXPECT_TRUE(snapshot.lights.empty());

    world.Shutdown();
}

TEST(RenderSceneValidation, RenderProxyBridgeDoesNotFallbackForHiddenPrimitiveControlledLegacyRenderer)
{
    World world;
    world.Initialize();

    auto* entity = CreateEntity(world, "HiddenPrimitiveControlledEntity");
    ASSERT_NE(nullptr, entity);

    auto mesh = MakeMeshResource(1203);
    auto* legacyRenderer = entity->AddComponent<MeshRendererComponent>();
    ASSERT_NE(nullptr, legacyRenderer);
    legacyRenderer->SetMesh(mesh);

    auto* primitive = static_cast<Actor*>(entity)->AddComponent<StaticMeshComponent>();
    ASSERT_NE(nullptr, primitive);
    EXPECT_TRUE(primitive->AttachToComponent(entity->GetRootComponent()));
    primitive->SetMesh(mesh);
    primitive->SetVisible(false);

    RenderProxySceneBridge bridge;
    RenderProxySnapshot snapshot;
    RenderProxySceneBridgeResult result;
    EXPECT_TRUE(bridge.BuildSnapshot(&world, snapshot, &result));

    EXPECT_TRUE(result.usedProxyPath);
    EXPECT_FALSE(result.requiresLegacyFallback);
    EXPECT_TRUE(snapshot.primitives.empty());

    world.Shutdown();
}

TEST(RenderSceneValidation, RenderProxyBridgeFallbackOnlyWhenLegacyPrimitiveIsRenderable)
{
    World world;
    world.Initialize();

    auto* emptyEntity = CreateEntity(world, "EmptyPrimitiveEntity");
    ASSERT_NE(nullptr, emptyEntity);
    auto* emptyPrimitive = static_cast<Actor*>(emptyEntity)->AddComponent<EmptyPrimitiveComponent>();
    ASSERT_NE(nullptr, emptyPrimitive);

    RenderProxySceneBridge bridge;
    RenderProxySnapshot snapshot;
    RenderProxySceneBridgeResult result;
    EXPECT_TRUE(bridge.BuildSnapshot(&world, snapshot, &result));
    EXPECT_FALSE(result.requiresLegacyFallback);

    auto* legacyEntity = CreateEntity(world, "LegacyPrimitiveEntity");
    ASSERT_NE(nullptr, legacyEntity);
    auto* legacyPrimitive = static_cast<Actor*>(legacyEntity)->AddComponent<LegacyOnlyPrimitiveComponent>();
    ASSERT_NE(nullptr, legacyPrimitive);

    EXPECT_FALSE(bridge.BuildSnapshot(&world, snapshot, &result));
    EXPECT_TRUE(result.requiresLegacyFallback);
    EXPECT_EQ(RenderProxySceneBridgeFallbackReason::PrimitiveProxyUnavailable, result.fallbackReason);
    EXPECT_EQ(legacyEntity->GetHandle(), result.fallbackOwnerId);

    world.Shutdown();
}

TEST(RenderSceneValidation, RenderProxyBridgeReportsProxyCreationFailure)
{
    World world;
    world.Initialize();

    auto* entity = CreateEntity(world, "FailingProxyPrimitiveEntity");
    ASSERT_NE(nullptr, entity);
    auto* primitive = static_cast<Actor*>(entity)->AddComponent<FailingProxyPrimitiveComponent>();
    ASSERT_NE(nullptr, primitive);

    RenderProxySceneBridge bridge;
    RenderProxySnapshot snapshot;
    RenderProxySceneBridgeResult result;
    EXPECT_FALSE(bridge.BuildSnapshot(&world, snapshot, &result));

    EXPECT_TRUE(result.requiresLegacyFallback);
    EXPECT_EQ(RenderProxySceneBridgeFallbackReason::PrimitiveProxyCreationFailed, result.fallbackReason);
    EXPECT_EQ(entity->GetHandle(), result.fallbackOwnerId);
    EXPECT_TRUE(snapshot.primitives.empty());

    world.Shutdown();
}

TEST(RenderSceneValidation, RenderProxyBridgeReflectsTransformUpdates)
{
    World world;
    world.Initialize();

    auto* entity = CreateEntity(world, "TransformProxyEntity");
    ASSERT_NE(nullptr, entity);

    auto mesh = MakeMeshResource(1204);
    auto* primitive = static_cast<Actor*>(entity)->AddComponent<StaticMeshComponent>();
    ASSERT_NE(nullptr, primitive);
    EXPECT_TRUE(primitive->AttachToComponent(entity->GetRootComponent()));
    primitive->SetMesh(mesh);

    RenderProxySceneBridge bridge;
    RenderProxySnapshot snapshot;
    RenderProxySceneBridgeResult result;

    entity->SetPosition(Vec3(1.0f, 0.0f, 0.0f));
    ASSERT_TRUE(bridge.BuildSnapshot(&world, snapshot, &result));
    ASSERT_EQ(static_cast<size_t>(1), snapshot.primitives.size());
    EXPECT_EQ(Vec3(1.0f, 0.0f, 0.0f), Vec3(snapshot.primitives[0].worldMatrix[3]));

    entity->SetPosition(Vec3(5.0f, 0.0f, 0.0f));
    ASSERT_TRUE(bridge.BuildSnapshot(&world, snapshot, &result));
    ASSERT_EQ(static_cast<size_t>(1), snapshot.primitives.size());
    EXPECT_EQ(Vec3(5.0f, 0.0f, 0.0f), Vec3(snapshot.primitives[0].worldMatrix[3]));

    world.Shutdown();
}

TEST(RenderSceneValidation, RenderSceneCollectFromWorldUsesMeshRendererProxyWhenPrimitiveHasNoData)
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

TEST(RenderSceneValidation, RenderSceneCollectFromWorldDoesNotDuplicateWhenStaticMeshPrimitiveRenders)
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

TEST(RenderSceneValidation, RenderSceneCollectFromWorldSkipsInactivePrimitiveOwners)
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

TEST(RenderSceneValidation, RenderSceneCollectFromWorldDoesNotUseMeshRendererWhenPrimitiveIsHidden)
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
