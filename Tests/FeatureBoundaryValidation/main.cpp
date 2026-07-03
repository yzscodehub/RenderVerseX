#include "Core/Log.h"
#include "RenderContracts/WaterRenderSnapshot.h"
#include "Scene/SceneEntity.h"
#include "Water/WaterComponent.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace RVX;

namespace
{
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
}
