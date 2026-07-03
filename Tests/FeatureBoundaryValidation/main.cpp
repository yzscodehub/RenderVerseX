#include "Core/Log.h"
#include "RenderContracts/FeatureRenderSnapshot.h"
#include "RenderContracts/TerrainRenderSnapshot.h"
#include "RenderContracts/WaterRenderSnapshot.h"
#include "RenderExtraction/RenderFeatureSceneBridge.h"
#include "Scene/Component.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneManager.h"
#include "Terrain/TerrainComponent.h"
#include "Water/WaterComponent.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace RVX;

namespace
{
    class TestParticleFeatureProvider final : public Component, public IRenderFeatureSnapshotProvider
    {
    public:
        const char* GetTypeName() const override { return "TestParticleFeatureProvider"; }

        bool AppendRenderFeatureSnapshot(RenderFeatureSnapshot& outSnapshot) const override
        {
            ParticleRenderSnapshotItem item;
            item.instanceId = 1001;
            item.systemId = 2002;
            item.systemName = "ContractParticle";
            item.position = Vec3(4.0f, 5.0f, 6.0f);
            item.renderMode = ParticleRenderSnapshotMode::Billboard;
            item.blendMode = ParticleRenderSnapshotBlendMode::AlphaBlend;
            item.simulationBackend = ParticleRenderSnapshotSimulationBackend::CPU;
            item.payloadStatus = ParticleRenderSnapshotPayloadStatus::MetadataOnly;
            item.aliveParticleCount = 12;
            item.maxParticleCount = 64;
            item.visible = true;
            item.simulationSupported = true;
            item.renderPayloadAvailable = false;
            item.sortingSupported = false;
            item.renderPayloadReason =
                "Particle snapshot contains metadata only; Render-owned particle draw data extraction is not connected";
            item.sortingReason =
                "Particle sorting is deferred to Render-owned feature passes";

            outSnapshot.particles.metadata.totalAliveParticles += item.aliveParticleCount;
            outSnapshot.particles.items.push_back(item);
            return true;
        }
    };

    std::filesystem::path FindSourcePath(const std::filesystem::path& relativePath)
    {
        std::filesystem::path cursor = std::filesystem::current_path();
        for (uint32 i = 0; i < 8; ++i)
        {
            const std::filesystem::path candidate = cursor / relativePath;
            if (std::filesystem::exists(candidate))
            {
                return candidate;
            }

            if (!cursor.has_parent_path() || cursor == cursor.parent_path())
                break;

            cursor = cursor.parent_path();
        }

        return {};
    }

    std::string ReadSourceFile(const std::filesystem::path& relativePath)
    {
        const std::filesystem::path sourcePath = FindSourcePath(relativePath);
        if (sourcePath.empty())
            return {};

        std::ifstream stream(sourcePath, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }

    void EnsureLogInitialized()
    {
        static bool initialized = false;
        if (!initialized)
        {
            Log::Initialize();
            initialized = true;
        }
    }
} // namespace

TEST(FeatureBoundaryValidation, WaterComponentPublicHeaderDoesNotExposeRenderOrRHI)
{
    const std::string waterHeader = ReadSourceFile("Water/Include/Water/WaterComponent.h");
    ASSERT_FALSE(waterHeader.empty());

    EXPECT_EQ(waterHeader.find("#include \"Render/"), std::string::npos);
    EXPECT_EQ(waterHeader.find("#include <Render/"), std::string::npos);
    EXPECT_EQ(waterHeader.find("#include \"RHI/"), std::string::npos);
    EXPECT_EQ(waterHeader.find("#include <RHI/"), std::string::npos);
    EXPECT_EQ(waterHeader.find("IRHIDevice"), std::string::npos);
    EXPECT_EQ(waterHeader.find("InitializeGPU"), std::string::npos);
    EXPECT_NE(waterHeader.find("WaterRenderSnapshot"), std::string::npos);
}

TEST(FeatureBoundaryValidation, WaterModuleDoesNotIncludeOrLinkRHI)
{
    const std::filesystem::path waterRoot = FindSourcePath("Water");
    ASSERT_FALSE(waterRoot.empty());

    for (const auto& entry : std::filesystem::recursive_directory_iterator(waterRoot))
    {
        if (!entry.is_regular_file())
            continue;

        const std::filesystem::path extension = entry.path().extension();
        if (extension != ".h" && extension != ".cpp")
            continue;

        std::ifstream stream(entry.path(), std::ios::binary);
        const std::string source{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        EXPECT_EQ(source.find("#include \"RHI/"), std::string::npos) << entry.path().string();
        EXPECT_EQ(source.find("#include <RHI/"), std::string::npos) << entry.path().string();
    }

    const std::string waterCMake = ReadSourceFile("Water/CMakeLists.txt");
    ASSERT_FALSE(waterCMake.empty());
    EXPECT_EQ(waterCMake.find("RVX_RHI"), std::string::npos);
}

TEST(FeatureBoundaryValidation, WaterComponentBuildsRenderSnapshotWithoutGPUHandles)
{
    EnsureLogInitialized();

    SceneEntity entity("WaterSnapshotEntity");
    entity.SetPosition(Vec3(4.0f, 2.0f, -3.0f));

    auto* water = entity.AddComponent<WaterComponent>();
    ASSERT_NE(water, nullptr);

    WaterSettings settings;
    settings.size = Vec2(320.0f, 180.0f);
    settings.depth = 12.5f;
    settings.resolution = 64;
    settings.surfaceType = WaterSurfaceType::River;
    settings.simulationType = WaterSimulationType::FFT;
    settings.enableReflection = true;
    settings.enableRefraction = false;
    settings.enableCaustics = true;
    settings.enableUnderwaterEffects = false;
    settings.enableFoam = true;
    water->SetSettings(settings);

    WaterRenderSnapshot snapshot;
    EXPECT_TRUE(water->BuildRenderSnapshot(snapshot));

    const WaterRenderSnapshotMetadata metadata = snapshot.GetMetadata();
    EXPECT_EQ(metadata.schemaVersion, RVX_WATER_RENDER_SNAPSHOT_SCHEMA_VERSION);
    EXPECT_EQ(metadata.status, WaterRenderSnapshotStatus::Complete);
    EXPECT_TRUE(metadata.complete);
    EXPECT_EQ(metadata.itemCount, 1u);

    ASSERT_EQ(snapshot.items.size(), 1u);
    const WaterRenderSnapshotItem& item = snapshot.items.front();
    EXPECT_EQ(item.componentId, entity.GetHandle());
    EXPECT_FLOAT_EQ(item.worldPosition.x, 4.0f);
    EXPECT_FLOAT_EQ(item.worldPosition.y, 2.0f);
    EXPECT_FLOAT_EQ(item.worldPosition.z, -3.0f);
    EXPECT_FLOAT_EQ(item.size.x, 320.0f);
    EXPECT_FLOAT_EQ(item.size.y, 180.0f);
    EXPECT_FLOAT_EQ(item.depth, 12.5f);
    EXPECT_EQ(item.resolution, 64u);
    EXPECT_EQ(item.surfaceType, WaterRenderSnapshotSurfaceType::River);
    EXPECT_EQ(item.simulationType, WaterRenderSnapshotSimulationType::FFT);
    EXPECT_TRUE(item.reflectionEnabled);
    EXPECT_FALSE(item.refractionEnabled);
    EXPECT_TRUE(item.causticsEnabled);
    EXPECT_FALSE(item.underwaterEffectsEnabled);
    EXPECT_TRUE(item.foamEnabled);
    EXPECT_FALSE(item.gpuInitialized);
    EXPECT_TRUE(item.cpuSimulationAvailable);
    EXPECT_FALSE(item.renderGpuPathAvailable);
    EXPECT_FALSE(item.gpuInitializationReason.empty());
    EXPECT_FALSE(item.renderPathReason.empty());
    EXPECT_FALSE(item.simulationFallbackReason.empty());
}

TEST(FeatureBoundaryValidation, TerrainComponentPublicHeaderDoesNotExposeRenderOrRHI)
{
    const std::string terrainHeader = ReadSourceFile("Terrain/Include/Terrain/TerrainComponent.h");
    ASSERT_FALSE(terrainHeader.empty());

    EXPECT_EQ(terrainHeader.find("#include \"Render/"), std::string::npos);
    EXPECT_EQ(terrainHeader.find("#include <Render/"), std::string::npos);
    EXPECT_EQ(terrainHeader.find("#include \"RHI/"), std::string::npos);
    EXPECT_EQ(terrainHeader.find("#include <RHI/"), std::string::npos);
    EXPECT_EQ(terrainHeader.find("IRHIDevice"), std::string::npos);
    EXPECT_EQ(terrainHeader.find("InitializeGPU"), std::string::npos);
    EXPECT_NE(terrainHeader.find("TerrainRenderSnapshot"), std::string::npos);
}

TEST(FeatureBoundaryValidation, TerrainModuleDoesNotIncludeOrLinkRHI)
{
    const std::filesystem::path terrainRoot = FindSourcePath("Terrain");
    ASSERT_FALSE(terrainRoot.empty());

    for (const auto& entry : std::filesystem::recursive_directory_iterator(terrainRoot))
    {
        if (!entry.is_regular_file())
            continue;

        const std::filesystem::path extension = entry.path().extension();
        if (extension != ".h" && extension != ".cpp")
            continue;

        std::ifstream stream(entry.path(), std::ios::binary);
        const std::string source{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        EXPECT_EQ(source.find("#include \"RHI/"), std::string::npos) << entry.path().string();
        EXPECT_EQ(source.find("#include <RHI/"), std::string::npos) << entry.path().string();
    }

    const std::string terrainCMake = ReadSourceFile("Terrain/CMakeLists.txt");
    ASSERT_FALSE(terrainCMake.empty());
    EXPECT_EQ(terrainCMake.find("RVX_RHI"), std::string::npos);
}

TEST(FeatureBoundaryValidation, TerrainComponentBuildsRenderSnapshotWithoutGPUHandles)
{
    EnsureLogInitialized();

    SceneEntity entity("TerrainSnapshotEntity");
    entity.SetPosition(Vec3(-8.0f, 1.5f, 6.0f));

    auto* terrain = entity.AddComponent<TerrainComponent>();
    ASSERT_NE(terrain, nullptr);

    TerrainSettings settings;
    settings.size = Vec3(512.0f, 96.0f, 256.0f);
    settings.lodBias = -0.5f;
    settings.patchSize = 16;
    settings.maxLODLevels = 5;
    settings.castShadows = false;
    settings.receiveShadows = true;
    terrain->SetSettings(settings);
    terrain->SetCollisionEnabled(false);

    TerrainRenderSnapshot snapshot;
    EXPECT_TRUE(terrain->BuildRenderSnapshot(snapshot));

    const TerrainRenderSnapshotMetadata metadata = snapshot.GetMetadata();
    EXPECT_EQ(metadata.schemaVersion, RVX_TERRAIN_RENDER_SNAPSHOT_SCHEMA_VERSION);
    EXPECT_EQ(metadata.status, TerrainRenderSnapshotStatus::Complete);
    EXPECT_TRUE(metadata.complete);
    EXPECT_EQ(metadata.itemCount, 1u);

    ASSERT_EQ(snapshot.items.size(), 1u);
    const TerrainRenderSnapshotItem& item = snapshot.items.front();
    EXPECT_EQ(item.componentId, entity.GetHandle());
    EXPECT_FLOAT_EQ(item.worldPosition.x, -8.0f);
    EXPECT_FLOAT_EQ(item.worldPosition.y, 1.5f);
    EXPECT_FLOAT_EQ(item.worldPosition.z, 6.0f);
    EXPECT_FLOAT_EQ(item.size.x, 512.0f);
    EXPECT_FLOAT_EQ(item.size.y, 96.0f);
    EXPECT_FLOAT_EQ(item.size.z, 256.0f);
    EXPECT_FLOAT_EQ(item.lodBias, -0.5f);
    EXPECT_EQ(item.patchSize, 16u);
    EXPECT_EQ(item.maxLODLevels, 5u);
    EXPECT_FALSE(item.castsShadow);
    EXPECT_TRUE(item.receivesShadow);
    EXPECT_FALSE(item.collisionEnabled);
    EXPECT_FALSE(item.hasHeightmap);
    EXPECT_FALSE(item.heightmapValid);
    EXPECT_FALSE(item.hasMaterial);
    EXPECT_FALSE(item.gpuInitialized);
    EXPECT_FALSE(item.cpuDataAvailable);
    EXPECT_FALSE(item.renderGpuPathAvailable);
    EXPECT_FALSE(item.renderPathReason.empty());
    EXPECT_FALSE(item.heightmapDiagnostic.empty());
    EXPECT_FALSE(item.materialDiagnostic.empty());
    EXPECT_FALSE(item.lodDiagnostic.empty());
}

TEST(FeatureBoundaryValidation, RenderFeatureSceneBridgeCollectsFeatureSnapshotsThroughProviderContract)
{
    EnsureLogInitialized();

    SceneManager sceneManager;
    sceneManager.Initialize();

    SceneEntity* particleEntity = sceneManager.GetEntity(sceneManager.CreateEntity("ExtractedParticles"));
    ASSERT_NE(particleEntity, nullptr);
    auto* particles = particleEntity->AddComponent<TestParticleFeatureProvider>();
    ASSERT_NE(particles, nullptr);

    SceneEntity* waterEntity = sceneManager.GetEntity(sceneManager.CreateEntity("ExtractedWater"));
    ASSERT_NE(waterEntity, nullptr);
    waterEntity->SetPosition(Vec3(10.0f, 0.0f, 20.0f));
    auto* water = waterEntity->AddComponent<WaterComponent>();
    ASSERT_NE(water, nullptr);
    WaterSettings waterSettings;
    waterSettings.size = Vec2(64.0f, 32.0f);
    waterSettings.surfaceType = WaterSurfaceType::Lake;
    water->SetSettings(waterSettings);

    SceneEntity* terrainEntity = sceneManager.GetEntity(sceneManager.CreateEntity("ExtractedTerrain"));
    ASSERT_NE(terrainEntity, nullptr);
    terrainEntity->SetPosition(Vec3(-20.0f, 0.0f, -10.0f));
    auto* terrain = terrainEntity->AddComponent<TerrainComponent>();
    ASSERT_NE(terrain, nullptr);
    TerrainSettings terrainSettings;
    terrainSettings.size = Vec3(128.0f, 24.0f, 96.0f);
    terrainSettings.patchSize = 16;
    terrain->SetSettings(terrainSettings);

    RenderFeatureSceneBridge bridge;
    RenderFeatureSnapshot snapshot;
    RenderFeatureSceneBridgeResult result;
    EXPECT_TRUE(bridge.BuildSnapshot(&sceneManager, snapshot, &result));

    const RenderFeatureSnapshotMetadata metadata = snapshot.GetMetadata();
    EXPECT_EQ(metadata.schemaVersion, RVX_RENDER_FEATURE_SNAPSHOT_SCHEMA_VERSION);
    EXPECT_EQ(metadata.status, RenderFeatureSnapshotStatus::Complete);
    EXPECT_TRUE(metadata.complete);
    EXPECT_EQ(metadata.providerCount, 3u);
    EXPECT_EQ(metadata.skippedProviderCount, 0u);
    EXPECT_EQ(metadata.particleItemCount, 1u);
    EXPECT_EQ(metadata.waterItemCount, 1u);
    EXPECT_EQ(metadata.terrainItemCount, 1u);

    EXPECT_TRUE(result.usedProviderPath);
    EXPECT_FALSE(result.requiresLegacyFallback);
    EXPECT_EQ(result.fallbackReason, RenderFeatureSceneBridgeFallbackReason::None);
    EXPECT_EQ(result.providerCount, 3u);
    EXPECT_EQ(result.particleItemCount, 1u);
    EXPECT_EQ(result.waterItemCount, 1u);
    EXPECT_EQ(result.terrainItemCount, 1u);

    ASSERT_EQ(snapshot.particles.items.size(), 1u);
    EXPECT_EQ(snapshot.particles.items.front().instanceId, 1001u);
    EXPECT_EQ(snapshot.particles.items.front().systemName, "ContractParticle");
    EXPECT_EQ(snapshot.particles.items.front().aliveParticleCount, 12u);
    EXPECT_EQ(snapshot.particles.items.front().simulationBackend, ParticleRenderSnapshotSimulationBackend::CPU);
    EXPECT_EQ(snapshot.particles.items.front().payloadStatus, ParticleRenderSnapshotPayloadStatus::MetadataOnly);
    EXPECT_FALSE(snapshot.particles.items.front().renderPayloadAvailable);
    EXPECT_FALSE(snapshot.particles.items.front().sortingSupported);
    EXPECT_NE(snapshot.particles.items.front().renderPayloadReason.find("Render-owned"), std::string::npos);
    EXPECT_NE(snapshot.particles.items.front().sortingReason.find("Render-owned"), std::string::npos);

    ASSERT_EQ(snapshot.water.items.size(), 1u);
    EXPECT_EQ(snapshot.water.items.front().componentId, waterEntity->GetHandle());
    EXPECT_EQ(snapshot.water.items.front().surfaceType, WaterRenderSnapshotSurfaceType::Lake);

    ASSERT_EQ(snapshot.terrain.items.size(), 1u);
    EXPECT_EQ(snapshot.terrain.items.front().componentId, terrainEntity->GetHandle());
    EXPECT_EQ(snapshot.terrain.items.front().patchSize, 16u);

    sceneManager.Shutdown();
}
