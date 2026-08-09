#include "Core/Log.h"
#include "Resource/ResourceSubsystem.h"
#include "Resource/Types/EnvironmentResource.h"
#include "Resource/Types/ModelResource.h"
#include "ResourceSceneAdapters/ResourceSceneAdapters.h"
#include "ResourceSceneAdapters/SceneAssetInstantiation.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneRuntime.h"

#include <gtest/gtest.h>

namespace
{
    class LogEnvironment final : public ::testing::Environment
    {
    public:
        void SetUp() override { RVX::Log::Initialize(); }
        void TearDown() override { RVX::Log::Shutdown(); }
    };

    [[maybe_unused]] ::testing::Environment* const g_logEnvironment =
        ::testing::AddGlobalTestEnvironment(new LogEnvironment());

    class LoadedTextureResource final : public RVX::Resource::TextureResource
    {
    public:
        void MarkLoaded() { SetState(RVX::ResourceState::Loaded); }
    };

    RVX::Resource::TextureHandle MakeTexture(RVX::ResourceId id)
    {
        auto* texture = new LoadedTextureResource();
        texture->SetId(id);
        RVX::Resource::TextureMetadata metadata;
        metadata.width = 1;
        metadata.height = 1;
        texture->SetData(std::vector<RVX::uint8>(4, 255), metadata);
        texture->MarkLoaded();
        return RVX::Resource::TextureHandle(texture);
    }
}

TEST(SceneAssetInstantiationValidation, InstantiatesAndDestroysHierarchyTransactionally)
{
    RVX::ResourceSceneAdapters::RegisterDefaults();
    RVX::Scene scene;
    ASSERT_TRUE(scene.Initialize());

    RVX::Resource::ModelResource model;
    auto root = std::make_shared<RVX::Node>("Root");
    root->AddChild(std::make_shared<RVX::Node>("Child"));
    model.SetRootNode(root);

    RVX::SceneAssetInstance instance =
        RVX::SceneAssetInstantiator::InstantiateModel(scene, model);
    ASSERT_TRUE(instance.IsValid());
    EXPECT_EQ(instance.status.lifecycle, RVX::SceneAssetLifecycle::Active);
    EXPECT_EQ(instance.status.residency, RVX::SceneAssetResidency::CPUReady);
    ASSERT_EQ(instance.actors.size(), 2U);
    EXPECT_NE(scene.ResolveActor(instance.rootActor), nullptr);

    const auto handles = instance.actors;
    EXPECT_TRUE(RVX::SceneAssetInstantiator::Destroy(scene, instance));
    for (RVX::Actor::Handle handle : handles)
        EXPECT_EQ(scene.ResolveActor(handle), nullptr);
    EXPECT_EQ(scene.GetActorCount(), 0U);
    scene.Shutdown();
}

TEST(SceneAssetInstantiationValidation, RejectsModelWithoutRootWithoutMutation)
{
    RVX::Scene scene;
    ASSERT_TRUE(scene.Initialize());
    RVX::Resource::ModelResource model;

    const RVX::SceneAssetInstance instance =
        RVX::SceneAssetInstantiator::InstantiateModel(scene, model);
    EXPECT_EQ(instance.status.lifecycle, RVX::SceneAssetLifecycle::Failed);
    EXPECT_EQ(scene.GetActorCount(), 0U);
    scene.Shutdown();
}

TEST(SceneAssetInstantiationValidation, CancelRollsBackHierarchyAndRuntimeBindings)
{
    RVX::Scene scene;
    ASSERT_TRUE(scene.Initialize());

    RVX::Resource::ModelResource model;
    auto root = std::make_shared<RVX::Node>("Root");
    root->AddChild(std::make_shared<RVX::Node>("Child"));
    model.SetRootNode(root);

    RVX::SceneAssetInstance instance =
        RVX::SceneAssetInstantiator::InstantiateModel(scene, model);
    ASSERT_TRUE(instance.IsValid());
    const auto actors = instance.actors;

    EXPECT_TRUE(RVX::SceneAssetInstantiator::Cancel(scene, instance));
    EXPECT_EQ(instance.status.lifecycle, RVX::SceneAssetLifecycle::Cancelled);
    EXPECT_FALSE(instance.rootActor.IsValid());
    EXPECT_TRUE(instance.actors.empty());
    EXPECT_NE(instance.status.diagnostic.find("cancelled"), std::string::npos);
    for (RVX::Actor::Handle actor : actors)
        EXPECT_EQ(scene.ResolveActor(actor), nullptr);
    EXPECT_EQ(scene.GetActorCount(), 0U);
    scene.Shutdown();
}

TEST(SceneAssetInstantiationValidation, FailedCpuDependencyRollsBackHierarchy)
{
    RVX::Scene scene;
    ASSERT_TRUE(scene.Initialize());

    RVX::Resource::ModelResource model;
    model.SetRootNode(std::make_shared<RVX::Node>("Root"));
    auto* unloadedMesh = new RVX::Resource::MeshResource();
    unloadedMesh->SetId(91);
    model.AddMesh(
        RVX::Resource::ResourceHandle<RVX::Resource::MeshResource>(
            unloadedMesh));

    RVX::SceneAssetInstance instance =
        RVX::SceneAssetInstantiator::InstantiateModel(scene, model);
    ASSERT_TRUE(instance.IsValid());
    ASSERT_EQ(scene.GetActorCount(), 1U);

    RVX::Resource::ResourceSubsystem resources;
    EXPECT_EQ(RVX::SceneAssetInstantiator::UpdateResidency(
                  scene, model, resources, instance).lifecycle,
              RVX::SceneAssetLifecycle::Failed);
    EXPECT_FALSE(instance.rootActor.IsValid());
    EXPECT_TRUE(instance.actors.empty());
    EXPECT_NE(instance.status.diagnostic.find("failed"), std::string::npos);
    EXPECT_EQ(scene.GetActorCount(), 0U);
    scene.Shutdown();
}

TEST(SceneAssetInstantiationValidation, EnvironmentResourceOwnsCompleteIblSet)
{
    RVX::EnvironmentResourceData data;
    data.sourcePath = "test.hdr";
    data.environment = MakeTexture(1);
    data.irradiance = MakeTexture(2);
    data.prefiltered = MakeTexture(3);
    data.brdfLUT = MakeTexture(4);
    data.environmentResolution = 1;
    data.irradianceResolution = 1;
    data.prefilteredResolution = 1;
    data.prefilteredMipLevels = 1;
    data.brdfLUTResolution = 1;

    auto* resource = new RVX::EnvironmentResource();
    RVX::EnvironmentHandle handle(resource);
    ASSERT_TRUE(resource->SetData(std::move(data)));
    EXPECT_TRUE(handle.IsLoaded());
    EXPECT_EQ(resource->GetRequiredDependencies().size(), 4U);
    EXPECT_STREQ(resource->GetTypeName(), "Environment");
}
