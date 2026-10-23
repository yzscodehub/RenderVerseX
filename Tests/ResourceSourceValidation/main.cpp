#include "AssetImport/SourceResourceLoaders.h"
#include "Core/Log.h"
#include "EngineDevelopmentComposition/EngineDevelopmentComposition.h"
#include "AssetImport/ModelLoader.h"
#include "AssetImport/TextureLoader.h"
#include "Resource/Loader/CookedModelLoader.h"
#include "Resource/Loader/CookedTextureLoader.h"
#include "Resource/ResourceManager.h"
#include "Resource/ResourceSubsystem.h"
#include "ResourceSource/PollingResourceChangeMonitor.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>

namespace
{
    namespace fs = std::filesystem;

    class LogEnvironment final : public ::testing::Environment
    {
    public:
        void SetUp() override { RVX::Log::Initialize(); }
        void TearDown() override { RVX::Log::Shutdown(); }
    };

    [[maybe_unused]] const auto* g_logEnvironment =
        ::testing::AddGlobalTestEnvironment(new LogEnvironment);

    class TemporaryDirectory final
    {
    public:
        TemporaryDirectory()
        {
            const auto suffix = std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count());
            m_path = fs::temp_directory_path() /
                ("RVX_ResourceSourceValidation_" + suffix);
            fs::create_directories(m_path);
        }

        ~TemporaryDirectory()
        {
            std::error_code error;
            fs::remove_all(m_path, error);
        }

        [[nodiscard]] const fs::path& Get() const noexcept { return m_path; }

    private:
        fs::path m_path;
    };

    void WriteFile(const fs::path& path, const std::string& text)
    {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.is_open());
        file << text;
        ASSERT_TRUE(file.good());
    }
} // namespace

TEST(ResourceSourceValidation, RuntimeCatalogRejectsSourceTextureAndModel)
{
    auto dependencies =
        RVX::Resource::CreateRuntimeResourceManagerDependencies();
    std::string sealError;
    ASSERT_TRUE(dependencies.loaderCatalog.Seal(sealError)) << sealError;

    const auto sourceTexture = dependencies.loaderCatalog.Resolve({
        RVX::ResourceType::Texture,
        RVX::ResourceLoadDomain::Source,
        "assets/albedo.png"});
    EXPECT_EQ(sourceTexture.status,
              RVX::Resource::ResourceLoaderResolveStatus::NotFound);

    const auto cookedTexture = dependencies.loaderCatalog.Resolve({
        RVX::ResourceType::Texture,
        RVX::ResourceLoadDomain::Cooked,
        "cooked/albedo.rva"});
    ASSERT_TRUE(cookedTexture.Succeeded());
    EXPECT_NE(dynamic_cast<RVX::Resource::CookedTextureLoader*>(
                  cookedTexture.loader.get()),
              nullptr);

    const auto sourceModel = dependencies.loaderCatalog.Resolve({
        RVX::ResourceType::Model,
        RVX::ResourceLoadDomain::Source,
        "assets/scene.gltf"});
    EXPECT_EQ(sourceModel.status,
              RVX::Resource::ResourceLoaderResolveStatus::NotFound);

    const auto packageModel = dependencies.loaderCatalog.Resolve({
        RVX::ResourceType::Model,
        RVX::ResourceLoadDomain::Package,
        "package/scene.rva"});
    ASSERT_TRUE(packageModel.Succeeded());
    EXPECT_NE(dynamic_cast<RVX::Resource::CookedModelLoader*>(
                  packageModel.loader.get()),
              nullptr);
}

TEST(ResourceSourceValidation, DevelopmentCompositionAddsExactSourceLoaders)
{
    auto dependencies = RVX::CreateDevelopmentResourceManagerDependencies();
    std::string sealError;
    ASSERT_TRUE(dependencies.loaderCatalog.Seal(sealError)) << sealError;
    ASSERT_NE(dependencies.changeMonitor, nullptr);

    const auto sourceTexture = dependencies.loaderCatalog.Resolve({
        RVX::ResourceType::Texture,
        RVX::ResourceLoadDomain::Source,
        "assets/albedo.png"});
    ASSERT_TRUE(sourceTexture.Succeeded());
    EXPECT_EQ(sourceTexture.matchCount, 1u);
    EXPECT_NE(dynamic_cast<RVX::AssetImport::SourceTextureLoader*>(
                  sourceTexture.loader.get()),
              nullptr);

    const auto sourceModel = dependencies.loaderCatalog.Resolve({
        RVX::ResourceType::Model,
        RVX::ResourceLoadDomain::Source,
        "assets/scene.glb"});
    ASSERT_TRUE(sourceModel.Succeeded());
    EXPECT_EQ(sourceModel.matchCount, 1u);
    EXPECT_NE(dynamic_cast<RVX::AssetImport::SourceModelLoader*>(
                  sourceModel.loader.get()),
              nullptr);

    const auto cookedModel = dependencies.loaderCatalog.Resolve({
        RVX::ResourceType::Model,
        RVX::ResourceLoadDomain::Cooked,
        "cooked/scene.rva"});
    EXPECT_TRUE(cookedModel.Succeeded());
    EXPECT_EQ(cookedModel.matchCount, 1u);
}

TEST(ResourceSourceValidation, DevelopmentEngineBundleIsCompleteAndMoveOnly)
{
    static_assert(!std::is_copy_constructible_v<RVX::EngineRuntimeDependencies>);
    static_assert(
        std::is_nothrow_move_constructible_v<RVX::EngineRuntimeDependencies>);

    RVX::EngineRuntimeDependencies dependencies =
        RVX::CreateDevelopmentEngineRuntimeDependencies();
    ASSERT_TRUE(dependencies.resourceManager.has_value());
    std::string sealError;
    ASSERT_TRUE(
        dependencies.resourceManager->loaderCatalog.Seal(sealError))
        << sealError;
    EXPECT_TRUE(dependencies.shaderPrograms);
    EXPECT_NE(dependencies.resourceManager->changeMonitor, nullptr);

    const auto sourceTexture = dependencies.resourceManager->loaderCatalog.Resolve({
        RVX::ResourceType::Texture,
        RVX::ResourceLoadDomain::Source,
        "assets/albedo.png"});
    EXPECT_TRUE(sourceTexture.Succeeded());
    EXPECT_EQ(sourceTexture.matchCount, 1u);
}

TEST(ResourceSourceValidation, MonitorOnlyReportsObservedSourceChanges)
{
    TemporaryDirectory root;
    const fs::path source = root.Get() / "asset.txt";
    WriteFile(source, "before");

    RVX::ResourceSource::PollingResourceChangeMonitor monitor;
    monitor.Watch(17, source.string());
    EXPECT_TRUE(monitor.Poll().empty());

    std::error_code error;
    const fs::file_time_type original = fs::last_write_time(source, error);
    ASSERT_FALSE(error);
    WriteFile(source, "after");
    fs::last_write_time(source, original + std::chrono::seconds(2), error);
    ASSERT_FALSE(error);

    const std::vector<RVX::ResourceSourceChange> changes = monitor.Poll();
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes.front().resourceId, 17u);
    EXPECT_TRUE(fs::equivalent(changes.front().path, source, error));
    EXPECT_FALSE(error);

    monitor.Unwatch(17);
    fs::last_write_time(source, original + std::chrono::seconds(4), error);
    ASSERT_FALSE(error);
    EXPECT_TRUE(monitor.Poll().empty());
}

TEST(ResourceSourceValidation, DevelopmentSubsystemReceivesChangeMonitor)
{
    TemporaryDirectory root;
    RVX::Resource::ResourceSubsystem subsystem;
    RVX::Resource::ResourceManagerConfig config;
    config.asyncThreadCount = 0;
    config.enableHotReload = true;
    config.runtimePolicy.mode = RVX::Resource::ResourceRuntimeMode::Editor;
    config.runtimePolicy.allowSourceAssetReads = true;
    config.runtimePolicy.sourceRoot = root.Get().string();

    subsystem.Initialize(
        config,
        RVX::CreateDevelopmentResourceManagerDependencies());
    EXPECT_TRUE(subsystem.GetManager().IsInitialized());
    EXPECT_TRUE(subsystem.GetManager().IsHotReloadEnabled());
    EXPECT_TRUE(
        subsystem.GetManager().GetHotReloadDiagnostic().watcherInitialized);
    subsystem.Deinitialize();
    EXPECT_FALSE(subsystem.GetManager().IsInitialized());
}
