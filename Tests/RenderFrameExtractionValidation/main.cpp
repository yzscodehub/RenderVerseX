#include "Core/Camera/Camera.h"
#include "Core/Log.h"
#include "Geometry/Asset/Material.h"
#include "Geometry/Asset/Mesh.h"
#include "RenderContracts/FeatureRenderSnapshot.h"
#include "RenderExtraction/RenderFrameExtractor.h"
#include "Resource/ResourceSubsystem.h"
#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/MeshResource.h"
#include "Resource/Types/TextureResource.h"
#include "Scene/Actor.h"
#include "Scene/Component.h"
#include "Scene/Components/LightComponent.h"
#include "Scene/Components/SkyboxComponent.h"
#include "Scene/Components/StaticMeshComponent.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneManager.h"
#include "World/World.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace RVX;

namespace
{
    namespace fs = std::filesystem;

    class FakeResourceGateway final : public IRenderResourceGateway
    {
    public:
        RenderResourceReserveResult ReserveResource(
            AssetId assetId,
            RenderResourceKind kind) noexcept override
        {
            const auto found = m_assets.find(assetId);
            if (found != m_assets.end())
            {
                return {RenderResourceReserveCode::Existing,
                        found->second,
                        QueryResourceStatus(found->second)};
            }

            const RenderResourceHandle handle{m_nextSlot++, 1};
            m_assets.emplace(assetId, handle);
            m_kinds.emplace(handle, kind);
            m_statuses.emplace(
                handle,
                RenderResourceStatus{RenderResourceStatusCode::Current,
                                     RenderResourcePublicState::Reserved,
                                     RenderResourceFailureCode::None});
            return {RenderResourceReserveCode::Reserved,
                    handle,
                    m_statuses.at(handle)};
        }

        RenderUploadEnqueueResult TryEnqueueUpload(
            const ResourceUploadRequestRef& request) noexcept override
        {
            if (request == nullptr ||
                m_statuses.find(request->GetHandle()) == m_statuses.end())
            {
                return {RenderUploadEnqueueCode::InvalidRequest};
            }
            m_statuses[request->GetHandle()].state =
                RenderResourcePublicState::UploadQueued;
            return {RenderUploadEnqueueCode::Accepted};
        }

        RenderReleaseResult RequestRelease(
            RenderResourceHandle handle) noexcept override
        {
            const auto found = m_statuses.find(handle);
            if (found == m_statuses.end())
            {
                return {RenderReleaseCode::StaleGeneration};
            }
            found->second.state = RenderResourcePublicState::Evicting;
            return {RenderReleaseCode::Accepted};
        }

        RenderResourceStatus QueryResourceStatus(
            RenderResourceHandle handle) const noexcept override
        {
            const auto found = m_statuses.find(handle);
            if (found == m_statuses.end())
            {
                return {RenderResourceStatusCode::StaleGeneration,
                        RenderResourcePublicState::Released,
                        RenderResourceFailureCode::None};
            }
            return found->second;
        }

    private:
        uint32 m_nextSlot = 1;
        std::unordered_map<AssetId, RenderResourceHandle, AssetIdHash>
            m_assets;
        std::unordered_map<RenderResourceHandle,
                           RenderResourceKind,
                           RenderResourceHandleHash>
            m_kinds;
        std::unordered_map<RenderResourceHandle,
                           RenderResourceStatus,
                           RenderResourceHandleHash>
            m_statuses;
    };

    class TestMeshLoader final : public Resource::IResourceLoader
    {
    public:
        ResourceType GetResourceType() const override
        {
            return ResourceType::Mesh;
        }

        std::vector<std::string> GetSupportedExtensions() const override
        {
            return {".obj"};
        }

        IResource* Load(const std::string&) override
        {
            auto mesh = std::make_shared<Mesh>();
            mesh->SetPositions({{-1.0f, -1.0f, 0.0f},
                                {1.0f, -1.0f, 0.0f},
                                {0.0f, 1.0f, 0.0f}});
            mesh->SetIndices(std::vector<uint16>{0, 1, 2});
            mesh->SetBoundingBox({-1.0f, -1.0f, 0.0f},
                                 {1.0f, 1.0f, 0.0f});
            auto* resource = new Resource::MeshResource();
            resource->SetMesh(std::move(mesh));
            resource->SetBounds(
                AABB({-1.0f, -1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}));
            return resource;
        }
    };

    class TestMaterialLoader final : public Resource::IResourceLoader
    {
    public:
        ResourceType GetResourceType() const override
        {
            return ResourceType::Material;
        }

        std::vector<std::string> GetSupportedExtensions() const override
        {
            return {".mat"};
        }

        IResource* Load(const std::string&) override
        {
            auto source = std::make_shared<Material>("frame-material");
            source->SetAlphaMode(Material::AlphaMode::Blend);
            auto* resource = new Resource::MaterialResource();
            resource->SetMaterialData(std::move(source));
            return resource;
        }
    };

    class LoadedTextureResource final : public Resource::TextureResource
    {
    public:
        void MarkLoaded()
        {
            SetState(ResourceState::Loaded);
        }
    };

    class CompleteFeatureProvider final : public Component,
                                          public IRenderFeatureSnapshotProvider
    {
    public:
        const char* GetTypeName() const override
        {
            return "CompleteFeatureProvider";
        }

        bool AppendRenderFeatureSnapshot(
            RenderFeatureSnapshot& snapshot) const override
        {
            ParticleRenderSnapshotItem particle;
            particle.instanceId = 501;
            particle.systemId = 502;
            particle.systemName = "owned-particle";
            particle.worldBounds = AABB(Vec3{-1.0f}, Vec3{1.0f});
            snapshot.particles.items.push_back(std::move(particle));

            WaterRenderSnapshotItem water;
            water.componentId = 601;
            water.worldBounds = AABB(Vec3{-2.0f}, Vec3{2.0f});
            snapshot.water.items.push_back(std::move(water));

            TerrainRenderSnapshotItem terrain;
            terrain.componentId = 701;
            terrain.worldBounds = AABB(Vec3{-3.0f}, Vec3{3.0f});
            snapshot.terrain.items.push_back(std::move(terrain));
            return true;
        }
    };

    class IncompleteFeatureProvider final
        : public Component,
          public IRenderFeatureSnapshotProvider
    {
    public:
        const char* GetTypeName() const override
        {
            return "IncompleteFeatureProvider";
        }

        bool AppendRenderFeatureSnapshot(
            RenderFeatureSnapshot&) const override
        {
            return false;
        }
    };

    fs::path MakeTempDirectory(const char* suffix)
    {
        const fs::path root =
            fs::temp_directory_path() /
            (std::string("rvx-frame-extraction-") + suffix);
        std::error_code error;
        fs::remove_all(root, error);
        fs::create_directories(root, error);
        return root;
    }

    void WritePlaceholder(const fs::path& path)
    {
        fs::create_directories(path.parent_path());
        std::ofstream stream(path);
        stream << "fixture";
    }

    Resource::ResourceManagerConfig MakeConfig(const fs::path& root)
    {
        Resource::ResourceManagerConfig config;
        config.asyncThreadCount = 0;
        config.runtimePolicy.mode = Resource::ResourceRuntimeMode::Editor;
        config.runtimePolicy.allowSourceAssetReads = true;
        config.runtimePolicy.sourceRoot = root.string();
        return config;
    }

    RenderFrameExtractionInput MakeInput(
        uint64 sequence,
        World* world,
        Resource::ResourceSubsystem* resources)
    {
        RenderFrameExtractionInput input;
        input.sequence = sequence;
        input.worldRevision = 17;
        input.temporalEpoch = 23;
        input.explicitDiscontinuity = true;
        input.world = world;
        input.resources = resources;
        input.view.viewportWidth = 1920;
        input.view.viewportHeight = 1080;
        input.view.nearPlane = 0.25f;
        input.view.farPlane = 2500.0f;
        input.view.absoluteTime = 12.5f;
        input.view.deltaTime = 1.0f / 60.0f;
        input.view.exposure = 1.25f;
        input.settings.renderScale = 0.75f;
        input.settings.debugView = 9;
        input.settings.postProcess.enableSSR = false;
        input.settings.shadows.atlasResolution = 2048;
        input.settings.gpuCulling.maxVisibleObjects = 8192;
        input.captureRequest.requestId = 77;
        input.captureRequest.kind = RenderFrameCaptureKind::Color;
        input.captureRequest.width = 640;
        input.captureRequest.height = 360;
        input.captureRequest.includeAlpha = true;
        return input;
    }

    SceneEntity* CreateEntity(World& world, const std::string& name)
    {
        SceneManager* sceneManager = world.GetSceneManager();
        const auto handle = sceneManager->CreateEntity(name);
        return sceneManager->GetEntity(handle);
    }

    class ExtractionFixture final
    {
    public:
        explicit ExtractionFixture(const char* suffix)
            : root(MakeTempDirectory(suffix))
        {
            subsystem.SetRenderResourceGateway(&gateway);
            subsystem.Initialize(MakeConfig(root));
            subsystem.RegisterLoader(
                ResourceType::Mesh,
                std::make_unique<TestMeshLoader>());
            subsystem.RegisterLoader(
                ResourceType::Material,
                std::make_unique<TestMaterialLoader>());
        }

        ~ExtractionFixture()
        {
            subsystem.BeginRenderShutdown();
            subsystem.Deinitialize();
            std::error_code error;
            fs::remove_all(root, error);
        }

        FakeResourceGateway gateway;
        Resource::ResourceSubsystem subsystem;
        fs::path root;
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

TEST(RenderFrameExtractionValidation, ExtractsCompleteOwnedPacketValues)
{
    ExtractionFixture fixture("complete");
    WritePlaceholder(fixture.root / "frame.obj");
    WritePlaceholder(fixture.root / "frame.mat");
    auto mesh = fixture.subsystem.Load<Resource::MeshResource>(
        "source://frame.obj");
    auto material = fixture.subsystem.Load<Resource::MaterialResource>(
        "source://frame.mat");
    ASSERT_TRUE(mesh.IsValid());
    ASSERT_TRUE(material.IsValid());
    fixture.subsystem.Tick(0.0f);

    World world;
    world.Initialize();
    Camera* camera = world.CreateCamera("ExtractionCamera");
    ASSERT_NE(camera, nullptr);
    camera->SetPerspective(1.0f, 16.0f / 9.0f, 0.25f, 2500.0f);
    camera->SetPosition({0.0f, 2.0f, 8.0f});
    camera->LookAt({0.0f, 0.0f, 0.0f});
    world.SetActiveCamera(camera);

    SceneEntity* primitiveEntity = CreateEntity(world, "Primitive");
    ASSERT_NE(primitiveEntity, nullptr);
    auto* primitive = static_cast<Actor*>(primitiveEntity)
                          ->AddComponent<StaticMeshComponent>();
    ASSERT_NE(primitive, nullptr);
    ASSERT_TRUE(primitive->AttachToComponent(
        primitiveEntity->GetRootComponent()));
    primitive->SetMesh(mesh);
    primitive->SetMaterial(0, material);
    primitive->SetLayerMask(0x55U);

    SceneEntity* lightEntity = CreateEntity(world, "Light");
    ASSERT_NE(lightEntity, nullptr);
    auto* light = lightEntity->AddComponent<LightComponent>();
    ASSERT_NE(light, nullptr);
    light->SetLightType(LightType::Point);
    light->SetIntensity(4.0f);
    light->SetCastsShadow(true);

    SceneEntity* featureEntity = CreateEntity(world, "Features");
    ASSERT_NE(featureEntity, nullptr);
    ASSERT_NE(featureEntity->AddComponent<CompleteFeatureProvider>(), nullptr);

    auto* unresolvedTexture = new LoadedTextureResource();
    unresolvedTexture->SetId(9001);
    Resource::TextureMetadata textureMetadata;
    textureMetadata.width = 1;
    textureMetadata.height = 1;
    textureMetadata.arrayLayers = 6;
    textureMetadata.isCubemap = true;
    unresolvedTexture->SetData(std::vector<uint8>(24, 255), textureMetadata);
    unresolvedTexture->MarkLoaded();
    Resource::ResourceHandle<Resource::TextureResource> unresolvedHandle(
        unresolvedTexture);

    SceneEntity* skyEntity = CreateEntity(world, "Sky");
    ASSERT_NE(skyEntity, nullptr);
    auto* sky = skyEntity->AddComponent<SkyboxComponent>();
    ASSERT_NE(sky, nullptr);
    sky->SetCubemap(SceneTextureHandle(unresolvedHandle));
    sky->SetExposure(1.75f);
    sky->SetRotation(0.5f);

    RenderFrameExtractor extractor;
    RenderFrameExtractionResult extraction =
        extractor.Extract(MakeInput(41, &world, &fixture.subsystem));
    ASSERT_TRUE(extraction.IsComplete());
    ASSERT_NE(extraction.packet, nullptr);
    EXPECT_EQ(extraction.sealCode, RenderFrameSealCode::Sealed);

    std::unique_ptr<const RenderFramePacket> packet =
        std::move(extraction.packet);
    world.Shutdown();
    mesh.Reset();
    material.Reset();
    unresolvedHandle.Reset();

    EXPECT_EQ(packet->GetHeader().sequence, 41U);
    EXPECT_EQ(packet->GetHeader().worldRevision, 17U);
    EXPECT_EQ(packet->GetHeader().temporalEpoch, 23U);
    EXPECT_TRUE(packet->GetHeader().explicitDiscontinuity);
    ASSERT_EQ(packet->GetPrimitives().size(), 1U);
    EXPECT_TRUE(packet->GetPrimitives()[0].mesh.IsValid());
    EXPECT_TRUE(packet->GetPrimitives()[0].material.IsValid());
    EXPECT_EQ(packet->GetPrimitives()[0].layerMask, 0x55U);
    ASSERT_EQ(packet->GetLights().size(), 1U);
    EXPECT_EQ(packet->GetLights()[0].type, RenderLightType::Point);
    EXPECT_FLOAT_EQ(packet->GetLights()[0].intensity, 4.0f);
    EXPECT_TRUE(packet->GetLights()[0].castsShadows);
    EXPECT_FALSE(packet->GetLights()[0].shadowResource.IsValid());
    EXPECT_FALSE(packet->GetSky().skyTexture.IsValid());
    EXPECT_FLOAT_EQ(packet->GetSky().intensity, 1.75f);
    EXPECT_FLOAT_EQ(packet->GetSky().rotationRadians, 0.5f);
    EXPECT_FALSE(packet->GetEnvironment().irradianceTexture.IsValid());
    EXPECT_EQ(packet->GetFeatures().particles.items[0].systemName,
              "owned-particle");
    EXPECT_EQ(packet->GetFeatures().water.items.size(), 1U);
    EXPECT_EQ(packet->GetFeatures().terrain.items.size(), 1U);
    EXPECT_FLOAT_EQ(packet->GetSettings().renderScale, 0.75f);
    EXPECT_EQ(packet->GetSettings().debugView, 9U);
    EXPECT_EQ(packet->GetCaptureRequest().requestId, 77U);
    EXPECT_EQ(packet->GetCaptureRequest().kind,
              RenderFrameCaptureKind::Color);
    EXPECT_EQ(packet->GetView().viewportWidth, 1920U);
    EXPECT_FLOAT_EQ(packet->GetView().absoluteTime, 12.5f);
    EXPECT_FLOAT_EQ(packet->GetView().deltaTime, 1.0f / 60.0f);
}

TEST(RenderFrameExtractionValidation, RejectsNullWorldAndMissingCamera)
{
    ExtractionFixture fixture("camera");
    RenderFrameExtractor extractor;
    RenderFrameExtractionInput input =
        MakeInput(1, nullptr, &fixture.subsystem);
    RenderFrameExtractionResult nullWorld = extractor.Extract(input);
    EXPECT_EQ(nullWorld.code, RenderFrameExtractionResultCode::NullWorld);
    EXPECT_EQ(nullWorld.packet, nullptr);

    World world;
    world.Initialize();
    input.world = &world;
    RenderFrameExtractionResult missingCamera = extractor.Extract(input);
    EXPECT_EQ(missingCamera.code,
              RenderFrameExtractionResultCode::MissingCamera);
    EXPECT_EQ(missingCamera.packet, nullptr);
    world.Shutdown();
}

TEST(RenderFrameExtractionValidation, RejectsIncompleteProviderWithoutPacket)
{
    ExtractionFixture fixture("provider");
    World world;
    world.Initialize();
    ASSERT_NE(world.CreateCamera("Main"), nullptr);
    SceneEntity* entity = CreateEntity(world, "Incomplete");
    ASSERT_NE(entity, nullptr);
    ASSERT_NE(entity->AddComponent<IncompleteFeatureProvider>(), nullptr);

    RenderFrameExtractor extractor;
    RenderFrameExtractionResult result =
        extractor.Extract(MakeInput(2, &world, &fixture.subsystem));
    EXPECT_EQ(result.code,
              RenderFrameExtractionResultCode::FeatureExtractionFailed);
    EXPECT_EQ(result.packet, nullptr);
    EXPECT_FALSE(result.diagnostics.complete);
    world.Shutdown();
}

TEST(RenderFrameExtractionValidation, EnforcesStrictCompletedSequence)
{
    ExtractionFixture fixture("sequence");
    World world;
    world.Initialize();
    ASSERT_NE(world.CreateCamera("Main"), nullptr);

    RenderFrameExtractor extractor;
    ASSERT_TRUE(
        extractor.Extract(MakeInput(8, &world, &fixture.subsystem))
            .IsComplete());
    RenderFrameExtractionResult repeated =
        extractor.Extract(MakeInput(8, &world, &fixture.subsystem));
    EXPECT_EQ(repeated.code,
              RenderFrameExtractionResultCode::NonMonotonicSequence);
    EXPECT_EQ(repeated.packet, nullptr);
    ASSERT_TRUE(
        extractor.Extract(MakeInput(9, &world, &fixture.subsystem))
            .IsComplete());
    EXPECT_EQ(extractor.GetLastCompletedSequence(), 9U);
    world.Shutdown();
}

TEST(RenderFrameExtractionValidation, ValidatesSettingsAndCaptureBeforeSeal)
{
    ExtractionFixture fixture("controls");
    World world;
    world.Initialize();
    ASSERT_NE(world.CreateCamera("Main"), nullptr);
    RenderFrameExtractor extractor;

    RenderFrameExtractionInput invalidSettings =
        MakeInput(1, &world, &fixture.subsystem);
    invalidSettings.settings.renderScale = 0.0f;
    RenderFrameExtractionResult settingsResult =
        extractor.Extract(invalidSettings);
    EXPECT_EQ(settingsResult.code,
              RenderFrameExtractionResultCode::InvalidSettings);
    EXPECT_EQ(settingsResult.packet, nullptr);

    RenderFrameExtractionInput invalidCapture =
        MakeInput(1, &world, &fixture.subsystem);
    invalidCapture.captureRequest.width = 0;
    RenderFrameExtractionResult captureResult =
        extractor.Extract(invalidCapture);
    EXPECT_EQ(captureResult.code,
              RenderFrameExtractionResultCode::InvalidCaptureRequest);
    EXPECT_EQ(captureResult.packet, nullptr);
    world.Shutdown();
}
