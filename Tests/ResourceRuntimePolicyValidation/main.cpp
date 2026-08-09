#include "Core/Diagnostics/ContentHash.h"
#include "Core/Log.h"
#include "Geometry/Asset/Material.h"
#include "Geometry/Asset/Mesh.h"
#include "Resource/RenderUploadRequestBuilder.h"
#include "Resource/ResourceManager.h"
#include "Resource/ResourceSubsystem.h"
#include "Resource/RuntimeResourcePolicy.h"
#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/MeshResource.h"
#include "Resource/Types/ModelResource.h"
#include "Resource/Types/ShaderResource.h"
#include "Resource/Types/TextureResource.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace RVX;
using namespace RVX::Resource;

namespace
{
    namespace fs = std::filesystem;

    void ExpectEquivalentExistingPath(const std::string& actualPath,
                                      const fs::path& expectedPath)
    {
        std::error_code error;
        const bool equivalent =
            fs::equivalent(fs::path(actualPath), expectedPath, error);
        EXPECT_FALSE(error)
            << "filesystem equivalence failed for actual='" << actualPath
            << "' expected='" << expectedPath.string()
            << "': " << error.message();
        EXPECT_TRUE(equivalent)
            << "paths do not identify the same existing file: actual='"
            << actualPath << "' expected='" << expectedPath.string() << "'";
    }

    class LogEnvironment : public ::testing::Environment
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

    [[maybe_unused]] const auto* g_logEnvironment =
        ::testing::AddGlobalTestEnvironment(new LogEnvironment);

    class RecordingResource final : public IResource
    {
    public:
        explicit RecordingResource(ResourceType type)
            : m_type(type)
        {
        }

        ResourceType GetType() const override { return m_type; }

        const char* GetTypeName() const override
        {
            return GetResourceTypeName(m_type);
        }

    private:
        ResourceType m_type = ResourceType::Unknown;
    };

    class RecordingLoader final : public IResourceLoader
    {
    public:
        explicit RecordingLoader(ResourceType type)
            : m_type(type)
        {
        }

        ResourceType GetResourceType() const override { return m_type; }

        std::vector<std::string> GetSupportedExtensions() const override
        {
            return {".png", ".rva"};
        }

        IResource* Load(const std::string& path) override
        {
            ++loadCount;
            lastPath = path;
            return new RecordingResource(m_type);
        }

        ResourceType m_type = ResourceType::Unknown;
        uint32 loadCount = 0;
        std::string lastPath;
    };

    class BlockingLoader final : public IResourceLoader
    {
    public:
        BlockingLoader(std::promise<void>* started, std::shared_future<void> release)
            : m_started(started)
            , m_release(std::move(release))
        {
        }

        ResourceType GetResourceType() const override { return ResourceType::Texture; }

        std::vector<std::string> GetSupportedExtensions() const override
        {
            return {".png"};
        }

        IResource* Load(const std::string& path) override
        {
            loadCount.fetch_add(1, std::memory_order_relaxed);
            lastPath = path;
            loaderThreadId = std::this_thread::get_id();
            if (m_started)
            {
                m_started->set_value();
            }
            m_release.wait();
            return new RecordingResource(ResourceType::Texture);
        }

        std::atomic<uint32> loadCount{0};
        std::string lastPath;
        std::thread::id loaderThreadId;

    private:
        std::promise<void>* m_started = nullptr;
        std::shared_future<void> m_release;
    };

    class ResourceManagerTestGuard
    {
    public:
        explicit ResourceManagerTestGuard(const ResourceManagerConfig& config)
        {
            auto& manager = ResourceManager::Get();
            if (manager.IsInitialized())
            {
                manager.Shutdown();
            }
            manager.Initialize(config);
        }

        ~ResourceManagerTestGuard()
        {
            auto& manager = ResourceManager::Get();
            if (manager.IsInitialized())
            {
                manager.Shutdown();
            }
        }
    };

    fs::path MakeTempDirectory(const std::string& name)
    {
        const auto suffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        fs::path directory = fs::temp_directory_path() / ("RVX_ResourceRuntimePolicy_" + name + "_" + suffix);
        fs::create_directories(directory);
        return directory;
    }

    void WriteTextFile(const fs::path& path, const std::string& text)
    {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        ASSERT_TRUE(file.is_open());
        file << text;
    }

    std::string ReadTextFile(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return {};
        }

        return std::string(std::istreambuf_iterator<char>(file),
                           std::istreambuf_iterator<char>());
    }

    fs::path FindRepositoryRoot()
    {
        fs::path cursor = fs::current_path();
        for (uint32 i = 0; i < 8; ++i)
        {
            if (fs::exists(cursor / "CMakeLists.txt") &&
                fs::exists(cursor / "Resource/Include/Resource/ResourceManager.h"))
            {
                return cursor;
            }

            if (!cursor.has_parent_path() || cursor == cursor.parent_path())
                break;

            cursor = cursor.parent_path();
        }

        return {};
    }

    void WriteShaderArtifact(const fs::path& path,
                             const std::vector<uint8>& bytecode,
                             const std::string& glsl,
                             const std::string& msl)
    {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        ASSERT_TRUE(file.is_open());
        file << "RVX_SHADER_PREBAKE_V1\n";
        file << "source=Shaders/Fullscreen.ps.hlsl\n";
        file << "backend=DirectX 12\n";
        file << "stage=Pixel\n";
        file << "entry=main\n";
        file << "targetProfile=ps_6_0\n";
        file << "bytecodeSize=" << bytecode.size() << "\n";
        file << "glslSize=" << glsl.size() << "\n";
        file << "mslSize=" << msl.size() << "\n";
        file << "reflectionResources=3\n";
        file << "sourceHash=123456789\n";
        file << "RVX_SHADER_BYTECODE_BEGIN\n";
        file.write(reinterpret_cast<const char*>(bytecode.data()),
                   static_cast<std::streamsize>(bytecode.size()));
        file << "\nRVX_SHADER_GLSL_BEGIN\n";
        file.write(glsl.data(), static_cast<std::streamsize>(glsl.size()));
        file << "\nRVX_SHADER_MSL_BEGIN\n";
        file.write(msl.data(), static_cast<std::streamsize>(msl.size()));
        file << "\nRVX_SHADER_PREBAKE_END\n";
        ASSERT_TRUE(file.good());
    }
} // namespace

TEST(ResourceRuntimePolicyValidation, RejectsSourceAssetsWhenCookedArtifactsAreRequired)
{
    ResourceManagerConfig config;
    config.asyncThreadCount = 0;
    config.runtimePolicy.mode = ResourceRuntimeMode::CookedRuntime;
    config.runtimePolicy.allowSourceAssetReads = false;
    config.runtimePolicy.requireCookedArtifacts = true;

    ResourceManagerTestGuard guard(config);
    auto& manager = ResourceManager::Get();

    auto loader = std::make_unique<RecordingLoader>(ResourceType::Texture);
    RecordingLoader* loaderPtr = loader.get();
    manager.RegisterLoader(ResourceType::Texture, std::move(loader));

    EXPECT_EQ(manager.LoadResource("source://textures/albedo.png"), nullptr);
    EXPECT_EQ(loaderPtr->loadCount, 0u);

    const ResourceLoadDiagnostic diagnostic = manager.GetLastLoadDiagnostic();
    EXPECT_TRUE(diagnostic.attempted);
    EXPECT_FALSE(diagnostic.success);
    EXPECT_EQ(diagnostic.domain, ResourceLoadDomain::SourceAsset);
    EXPECT_EQ(diagnostic.failure, ResourceLoadFailureCode::CookedArtifactRequired);
    EXPECT_TRUE(diagnostic.sourceAssetRead);
    EXPECT_FALSE(diagnostic.cookedArtifactRead);
    EXPECT_FALSE(diagnostic.runtimePackageRead);
    EXPECT_NE(diagnostic.message.find("Cooked artifact access is required"), std::string::npos);

    const std::string diagnosticJson = manager.ExportLastLoadDiagnosticJson();
    EXPECT_EQ(diagnosticJson, ExportResourceLoadDiagnosticJson(diagnostic));
    EXPECT_NE(diagnosticJson.find("\"schemaVersion\": 1"), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"schemaId\": \"RVX.Resource.LoadDiagnostic\""), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"id\": \"resourceLoadDiagnosticJson\""), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"kind\": \"ResourceLoadDiagnosticJson\""), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"contentType\": \"application/json\""), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"contentHash\": \"\""), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"relativePath\": \"\""), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"attempted\": true"), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"success\": false"), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"domain\": \"SourceAsset\""), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"failure\": \"CookedArtifactRequired\""), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"requestedPath\": \"source://textures/albedo.png\""), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"message\": \"Cooked artifact access is required by the active resource policy.\""),
              std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"sourceAssetRead\": true"), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"cookedArtifactRead\": false"), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"runtimePackageRead\": false"), std::string::npos);
    EXPECT_FALSE(manager.SaveLastLoadDiagnosticJson(nullptr));
    EXPECT_FALSE(manager.SaveLastLoadDiagnosticJson(""));

    const fs::path diagnosticPath =
        fs::temp_directory_path() /
        ("RVX_ResourceLoadDiagnostic_Denied_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    const std::string diagnosticPathString = diagnosticPath.string();
    ASSERT_TRUE(manager.SaveLastLoadDiagnosticJson(diagnosticPathString.c_str()));
    EXPECT_EQ(ReadTextFile(diagnosticPath), diagnosticJson);
    std::error_code removeError;
    fs::remove(diagnosticPath, removeError);
}

TEST(ResourceRuntimePolicyValidation, MapsCookedArtifactsThroughCookedRoot)
{
    const fs::path root = MakeTempDirectory("CookedRoot");
    const fs::path cookedPath = root / "textures" / "albedo.rva";
    WriteTextFile(cookedPath, "RVX_TEXTURE_PREBAKE_V1\n");

    ResourceManagerConfig config;
    config.asyncThreadCount = 0;
    config.runtimePolicy.mode = ResourceRuntimeMode::CookedRuntime;
    config.runtimePolicy.allowSourceAssetReads = false;
    config.runtimePolicy.requireCookedArtifacts = true;
    config.runtimePolicy.cookedRoot = root.string();

    ResourceManagerTestGuard guard(config);
    auto& manager = ResourceManager::Get();

    auto loader = std::make_unique<RecordingLoader>(ResourceType::Texture);
    RecordingLoader* loaderPtr = loader.get();
    manager.RegisterLoader(ResourceType::Texture, std::move(loader));

    IResource* resource = manager.LoadResource("cooked://textures/albedo.rva");
    ASSERT_NE(resource, nullptr);
    EXPECT_EQ(loaderPtr->loadCount, 1u);
    ExpectEquivalentExistingPath(loaderPtr->lastPath, cookedPath);

    const ResourceLoadDiagnostic diagnostic = manager.GetLastLoadDiagnostic();
    EXPECT_TRUE(diagnostic.success);
    EXPECT_EQ(diagnostic.domain, ResourceLoadDomain::CookedArtifact);
    EXPECT_EQ(diagnostic.failure, ResourceLoadFailureCode::None);
    EXPECT_FALSE(diagnostic.sourceAssetRead);
    EXPECT_TRUE(diagnostic.cookedArtifactRead);
    EXPECT_FALSE(diagnostic.runtimePackageRead);
    ExpectEquivalentExistingPath(diagnostic.resolvedPath, cookedPath);

    const std::string diagnosticJson = manager.ExportLastLoadDiagnosticJson();
    EXPECT_EQ(diagnosticJson, ExportResourceLoadDiagnosticJson(diagnostic));
    EXPECT_NE(diagnosticJson.find("\"schemaId\": \"RVX.Resource.LoadDiagnostic\""), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"kind\": \"ResourceLoadDiagnosticJson\""), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"success\": true"), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"domain\": \"CookedArtifact\""), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"failure\": \"None\""), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"requestedPath\": \"cooked://textures/albedo.rva\""), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"resolvedPath\": "), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"message\": \"Resource loaded successfully.\""), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"sourceAssetRead\": false"), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"cookedArtifactRead\": true"), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"runtimePackageRead\": false"), std::string::npos);

    const fs::path diagnosticPath = root / "diagnostics" / "load-success.json";
    fs::create_directories(diagnosticPath.parent_path());
    const std::string diagnosticPathString = diagnosticPath.string();
    ASSERT_TRUE(SaveResourceLoadDiagnosticJson(diagnosticPathString.c_str(), diagnostic));
    EXPECT_EQ(ReadTextFile(diagnosticPath), diagnosticJson);

    std::error_code removeError;
    fs::remove_all(root, removeError);
}

TEST(ResourceRuntimePolicyValidation, RejectsRuntimeSourceAndCookedPathsWithoutMountedRootOrBasePath)
{
    ResourceRuntimePolicy cookedPolicy;
    cookedPolicy.mode = ResourceRuntimeMode::CookedRuntime;
    cookedPolicy.allowSourceAssetReads = false;
    cookedPolicy.requireCookedArtifacts = true;

    const ResourcePathResolution cookedResolution =
        ResolveRuntimeResourcePath(cookedPolicy, "", "cooked://textures/albedo.rva");
    EXPECT_FALSE(cookedResolution.allowed);
    EXPECT_EQ(cookedResolution.domain, ResourceLoadDomain::CookedArtifact);
    EXPECT_EQ(cookedResolution.failure, ResourceLoadFailureCode::ResourceRootMissing);
    EXPECT_FALSE(cookedResolution.sourceAssetRead);
    EXPECT_TRUE(cookedResolution.cookedArtifactRead);
    EXPECT_NE(cookedResolution.diagnosticMessage.find("root is not mounted"), std::string::npos);

    ResourceRuntimePolicy sourcePolicy;
    sourcePolicy.mode = ResourceRuntimeMode::CookedRuntime;
    sourcePolicy.allowSourceAssetReads = true;
    sourcePolicy.requireCookedArtifacts = false;

    const ResourcePathResolution sourceResolution =
        ResolveRuntimeResourcePath(sourcePolicy, "", "source://textures/albedo.png");
    EXPECT_FALSE(sourceResolution.allowed);
    EXPECT_EQ(sourceResolution.domain, ResourceLoadDomain::SourceAsset);
    EXPECT_EQ(sourceResolution.failure, ResourceLoadFailureCode::ResourceRootMissing);
    EXPECT_TRUE(sourceResolution.sourceAssetRead);
    EXPECT_FALSE(sourceResolution.cookedArtifactRead);
    EXPECT_NE(sourceResolution.diagnosticMessage.find("root is not mounted"), std::string::npos);
}

TEST(ResourceRuntimePolicyValidation, ReportsMissingRuntimePackageRoot)
{
    ResourceManagerConfig config;
    config.asyncThreadCount = 0;
    config.runtimePolicy.mode = ResourceRuntimeMode::PackagedRuntime;
    config.runtimePolicy.allowSourceAssetReads = false;
    config.runtimePolicy.requireRuntimePackage = true;

    ResourceManagerTestGuard guard(config);
    auto& manager = ResourceManager::Get();

    auto loader = std::make_unique<RecordingLoader>(ResourceType::Shader);
    RecordingLoader* loaderPtr = loader.get();
    manager.RegisterLoader(ResourceType::Shader, std::move(loader));

    EXPECT_EQ(manager.LoadResource("package://Base/shaders/basic.rva"), nullptr);
    EXPECT_EQ(loaderPtr->loadCount, 0u);

    const ResourceLoadDiagnostic diagnostic = manager.GetLastLoadDiagnostic();
    EXPECT_TRUE(diagnostic.attempted);
    EXPECT_FALSE(diagnostic.success);
    EXPECT_EQ(diagnostic.domain, ResourceLoadDomain::RuntimePackage);
    EXPECT_EQ(diagnostic.failure, ResourceLoadFailureCode::PackageRootMissing);
    EXPECT_FALSE(diagnostic.sourceAssetRead);
    EXPECT_FALSE(diagnostic.cookedArtifactRead);
    EXPECT_TRUE(diagnostic.runtimePackageRead);
}

TEST(ResourceRuntimePolicyValidation, MapsRuntimePackageEntriesThroughMountedPackageRoot)
{
    const fs::path root = MakeTempDirectory("PackageRoot");
    const fs::path packageEntryPath = root / "Base" / "shaders" / "basic.rva";
    WriteTextFile(packageEntryPath, "RVX_SHADER_PREBAKE_V1\n");

    ResourceManagerConfig config;
    config.asyncThreadCount = 0;
    config.runtimePolicy.mode = ResourceRuntimeMode::PackagedRuntime;
    config.runtimePolicy.allowSourceAssetReads = false;
    config.runtimePolicy.requireRuntimePackage = true;
    config.runtimePolicy.packageRoot = root.string();

    ResourceManagerTestGuard guard(config);
    auto& manager = ResourceManager::Get();

    auto loader = std::make_unique<RecordingLoader>(ResourceType::Shader);
    RecordingLoader* loaderPtr = loader.get();
    manager.RegisterLoader(ResourceType::Shader, std::move(loader));

    IResource* resource = manager.LoadResource("package://Base/shaders/basic.rva");
    ASSERT_NE(resource, nullptr);
    EXPECT_EQ(loaderPtr->loadCount, 1u);
    ExpectEquivalentExistingPath(loaderPtr->lastPath, packageEntryPath);

    const ResourceLoadDiagnostic diagnostic = manager.GetLastLoadDiagnostic();
    EXPECT_TRUE(diagnostic.success);
    EXPECT_EQ(diagnostic.domain, ResourceLoadDomain::RuntimePackage);
    EXPECT_EQ(diagnostic.failure, ResourceLoadFailureCode::None);
    EXPECT_FALSE(diagnostic.sourceAssetRead);
    EXPECT_FALSE(diagnostic.cookedArtifactRead);
    EXPECT_TRUE(diagnostic.runtimePackageRead);
    ExpectEquivalentExistingPath(diagnostic.resolvedPath, packageEntryPath);

    std::error_code removeError;
    fs::remove_all(root, removeError);
}

TEST(ResourceRuntimePolicyValidation, MapsRuntimePackageEntriesThroughMountTablePriority)
{
    const fs::path lowRoot = MakeTempDirectory("PackageMountLow");
    const fs::path highRoot = MakeTempDirectory("PackageMountHigh");
    const fs::path lowArtifactPath = lowRoot / "compiled" / "basic.rva";
    const fs::path highArtifactPath = highRoot / "compiled" / "basic.rva";
    WriteTextFile(lowArtifactPath, "RVX_SHADER_PREBAKE_V1\nlow\n");
    WriteTextFile(highArtifactPath, "RVX_SHADER_PREBAKE_V1\nhigh\n");
    const std::string highHash = Diagnostics::ComputeFileContentHash(highArtifactPath);

    ResourceManagerConfig config;
    config.asyncThreadCount = 0;
    config.runtimePolicy.mode = ResourceRuntimeMode::PackagedRuntime;
    config.runtimePolicy.allowSourceAssetReads = false;
    config.runtimePolicy.requireRuntimePackage = true;
    config.runtimePolicy.packageMounts = {
        ResourcePackageMount{
            "Base",
            0,
            lowRoot.string(),
            {ResourcePackageArtifact{"shaders/basic.rva", "compiled/basic.rva", ""}}},
        ResourcePackageMount{
            "Base",
            10,
            highRoot.string(),
            {ResourcePackageArtifact{"shaders/basic.rva", "compiled/basic.rva", highHash}}},
    };

    ResourceManagerTestGuard guard(config);
    auto& manager = ResourceManager::Get();

    auto loader = std::make_unique<RecordingLoader>(ResourceType::Shader);
    RecordingLoader* loaderPtr = loader.get();
    manager.RegisterLoader(ResourceType::Shader, std::move(loader));

    IResource* resource = manager.LoadResource("package://Base/shaders/basic.rva");
    ASSERT_NE(resource, nullptr);
    EXPECT_EQ(loaderPtr->loadCount, 1u);
    ExpectEquivalentExistingPath(loaderPtr->lastPath, highArtifactPath);

    const ResourceLoadDiagnostic diagnostic = manager.GetLastLoadDiagnostic();
    EXPECT_TRUE(diagnostic.success);
    EXPECT_EQ(diagnostic.domain, ResourceLoadDomain::RuntimePackage);
    EXPECT_EQ(diagnostic.failure, ResourceLoadFailureCode::None);
    EXPECT_EQ(diagnostic.packageName, "Base");
    EXPECT_EQ(diagnostic.packageMountPriority, 10);
    EXPECT_EQ(diagnostic.packageLogicalPath, "shaders/basic.rva");
    EXPECT_EQ(diagnostic.packageArtifactPath, "compiled/basic.rva");
    EXPECT_EQ(diagnostic.packageExpectedContentHash, highHash);
    EXPECT_EQ(diagnostic.packageActualContentHash, highHash);
    EXPECT_TRUE(diagnostic.packageHashChecked);
    EXPECT_TRUE(diagnostic.packageHashMatched);

    const std::string diagnosticJson = manager.ExportLastLoadDiagnosticJson();
    EXPECT_NE(diagnosticJson.find("\"packageName\": \"Base\""), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"packageMountPriority\": 10"), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"packageHashMatched\": true"), std::string::npos);

    std::error_code removeError;
    fs::remove_all(lowRoot, removeError);
    fs::remove_all(highRoot, removeError);
}

TEST(ResourceRuntimePolicyValidation, RejectsMissingRuntimePackageMount)
{
    const fs::path root = MakeTempDirectory("PackageMountMissing");

    ResourceManagerConfig config;
    config.asyncThreadCount = 0;
    config.runtimePolicy.mode = ResourceRuntimeMode::PackagedRuntime;
    config.runtimePolicy.allowSourceAssetReads = false;
    config.runtimePolicy.requireRuntimePackage = true;
    config.runtimePolicy.packageMounts = {
        ResourcePackageMount{"Other", 0, root.string(), {}},
    };

    ResourceManagerTestGuard guard(config);
    auto& manager = ResourceManager::Get();

    auto loader = std::make_unique<RecordingLoader>(ResourceType::Shader);
    RecordingLoader* loaderPtr = loader.get();
    manager.RegisterLoader(ResourceType::Shader, std::move(loader));

    EXPECT_EQ(manager.LoadResource("package://Base/shaders/basic.rva"), nullptr);
    EXPECT_EQ(loaderPtr->loadCount, 0u);

    const ResourceLoadDiagnostic diagnostic = manager.GetLastLoadDiagnostic();
    EXPECT_TRUE(diagnostic.attempted);
    EXPECT_FALSE(diagnostic.success);
    EXPECT_EQ(diagnostic.domain, ResourceLoadDomain::RuntimePackage);
    EXPECT_EQ(diagnostic.failure, ResourceLoadFailureCode::PackageMountMissing);
    EXPECT_EQ(diagnostic.packageName, "Base");
    EXPECT_EQ(diagnostic.packageLogicalPath, "shaders/basic.rva");

    std::error_code removeError;
    fs::remove_all(root, removeError);
}

TEST(ResourceRuntimePolicyValidation, RejectsMissingRuntimePackageArtifact)
{
    const fs::path root = MakeTempDirectory("PackageArtifactMissing");

    ResourceManagerConfig config;
    config.asyncThreadCount = 0;
    config.runtimePolicy.mode = ResourceRuntimeMode::PackagedRuntime;
    config.runtimePolicy.allowSourceAssetReads = false;
    config.runtimePolicy.requireRuntimePackage = true;
    config.runtimePolicy.packageMounts = {
        ResourcePackageMount{
            "Base",
            0,
            root.string(),
            {ResourcePackageArtifact{"shaders/other.rva", "compiled/other.rva", ""}}},
    };

    ResourceManagerTestGuard guard(config);
    auto& manager = ResourceManager::Get();

    auto loader = std::make_unique<RecordingLoader>(ResourceType::Shader);
    RecordingLoader* loaderPtr = loader.get();
    manager.RegisterLoader(ResourceType::Shader, std::move(loader));

    EXPECT_EQ(manager.LoadResource("package://Base/shaders/basic.rva"), nullptr);
    EXPECT_EQ(loaderPtr->loadCount, 0u);

    const ResourceLoadDiagnostic diagnostic = manager.GetLastLoadDiagnostic();
    EXPECT_TRUE(diagnostic.attempted);
    EXPECT_FALSE(diagnostic.success);
    EXPECT_EQ(diagnostic.domain, ResourceLoadDomain::RuntimePackage);
    EXPECT_EQ(diagnostic.failure, ResourceLoadFailureCode::PackageArtifactMissing);
    EXPECT_EQ(diagnostic.packageName, "Base");
    EXPECT_EQ(diagnostic.packageLogicalPath, "shaders/basic.rva");

    std::error_code removeError;
    fs::remove_all(root, removeError);
}

TEST(ResourceRuntimePolicyValidation, RejectsRuntimePackageArtifactHashMismatch)
{
    const fs::path root = MakeTempDirectory("PackageArtifactHashMismatch");
    const fs::path artifactPath = root / "compiled" / "basic.rva";
    WriteTextFile(artifactPath, "RVX_SHADER_PREBAKE_V1\nhash mismatch\n");

    ResourceManagerConfig config;
    config.asyncThreadCount = 0;
    config.runtimePolicy.mode = ResourceRuntimeMode::PackagedRuntime;
    config.runtimePolicy.allowSourceAssetReads = false;
    config.runtimePolicy.requireRuntimePackage = true;
    config.runtimePolicy.packageMounts = {
        ResourcePackageMount{
            "Base",
            0,
            root.string(),
            {ResourcePackageArtifact{"shaders/basic.rva", "compiled/basic.rva", "0000000000000000"}}},
    };

    ResourceManagerTestGuard guard(config);
    auto& manager = ResourceManager::Get();

    auto loader = std::make_unique<RecordingLoader>(ResourceType::Shader);
    RecordingLoader* loaderPtr = loader.get();
    manager.RegisterLoader(ResourceType::Shader, std::move(loader));

    EXPECT_EQ(manager.LoadResource("package://Base/shaders/basic.rva"), nullptr);
    EXPECT_EQ(loaderPtr->loadCount, 0u);

    const ResourceLoadDiagnostic diagnostic = manager.GetLastLoadDiagnostic();
    EXPECT_TRUE(diagnostic.attempted);
    EXPECT_FALSE(diagnostic.success);
    EXPECT_EQ(diagnostic.domain, ResourceLoadDomain::RuntimePackage);
    EXPECT_EQ(diagnostic.failure, ResourceLoadFailureCode::PackageArtifactHashMismatch);
    EXPECT_EQ(diagnostic.packageName, "Base");
    EXPECT_EQ(diagnostic.packageLogicalPath, "shaders/basic.rva");
    EXPECT_EQ(diagnostic.packageExpectedContentHash, "0000000000000000");
    EXPECT_FALSE(diagnostic.packageActualContentHash.empty());
    EXPECT_TRUE(diagnostic.packageHashChecked);
    EXPECT_FALSE(diagnostic.packageHashMatched);

    const std::string diagnosticJson = manager.ExportLastLoadDiagnosticJson();
    EXPECT_NE(diagnosticJson.find("\"failure\": \"PackageArtifactHashMismatch\""), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"packageHashChecked\": true"), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"packageHashMatched\": false"), std::string::npos);

    std::error_code removeError;
    fs::remove_all(root, removeError);
}

TEST(ResourceRuntimePolicyValidation, RejectsRuntimePackagePathsEscapingMountedRoot)
{
    const fs::path root = MakeTempDirectory("PackageRootEscape");

    ResourceManagerConfig config;
    config.asyncThreadCount = 0;
    config.runtimePolicy.mode = ResourceRuntimeMode::PackagedRuntime;
    config.runtimePolicy.allowSourceAssetReads = false;
    config.runtimePolicy.requireRuntimePackage = true;
    config.runtimePolicy.packageRoot = root.string();

    ResourceManagerTestGuard guard(config);
    auto& manager = ResourceManager::Get();

    auto loader = std::make_unique<RecordingLoader>(ResourceType::Shader);
    RecordingLoader* loaderPtr = loader.get();
    manager.RegisterLoader(ResourceType::Shader, std::move(loader));

    EXPECT_EQ(manager.LoadResource("package://Base/../../outside.rva"), nullptr);
    EXPECT_EQ(loaderPtr->loadCount, 0u);

    const ResourceLoadDiagnostic diagnostic = manager.GetLastLoadDiagnostic();
    EXPECT_TRUE(diagnostic.attempted);
    EXPECT_FALSE(diagnostic.success);
    EXPECT_EQ(diagnostic.domain, ResourceLoadDomain::RuntimePackage);
    EXPECT_EQ(diagnostic.failure, ResourceLoadFailureCode::PathEscapesRoot);
    EXPECT_FALSE(diagnostic.sourceAssetRead);
    EXPECT_FALSE(diagnostic.cookedArtifactRead);
    EXPECT_TRUE(diagnostic.runtimePackageRead);
    EXPECT_NE(diagnostic.message.find("escapes"), std::string::npos);

    const std::string diagnosticJson = manager.ExportLastLoadDiagnosticJson();
    EXPECT_NE(diagnosticJson.find("\"failure\": \"PathEscapesRoot\""), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"runtimePackageRead\": true"), std::string::npos);

    std::error_code removeError;
    fs::remove_all(root, removeError);
}

TEST(ResourceRuntimePolicyValidation, RejectsCookedAndSourcePathsEscapingMountedRoots)
{
    const fs::path sourceRoot = MakeTempDirectory("SourceRootEscape");
    const fs::path cookedRoot = MakeTempDirectory("CookedRootEscape");

    ResourceRuntimePolicy sourcePolicy;
    sourcePolicy.mode = ResourceRuntimeMode::Editor;
    sourcePolicy.allowSourceAssetReads = true;
    sourcePolicy.sourceRoot = sourceRoot.string();

    const ResourcePathResolution sourceResolution =
        ResolveRuntimeResourcePath(sourcePolicy, "", "source://../outside.png");
    EXPECT_FALSE(sourceResolution.allowed);
    EXPECT_EQ(sourceResolution.domain, ResourceLoadDomain::SourceAsset);
    EXPECT_EQ(sourceResolution.failure, ResourceLoadFailureCode::PathEscapesRoot);
    EXPECT_TRUE(sourceResolution.sourceAssetRead);
    EXPECT_NE(sourceResolution.diagnosticMessage.find("escapes"), std::string::npos);

    ResourceRuntimePolicy cookedPolicy;
    cookedPolicy.mode = ResourceRuntimeMode::CookedRuntime;
    cookedPolicy.allowSourceAssetReads = false;
    cookedPolicy.requireCookedArtifacts = true;
    cookedPolicy.cookedRoot = cookedRoot.string();

    const ResourcePathResolution cookedResolution =
        ResolveRuntimeResourcePath(cookedPolicy, "", "cooked://../outside.rva");
    EXPECT_FALSE(cookedResolution.allowed);
    EXPECT_EQ(cookedResolution.domain, ResourceLoadDomain::CookedArtifact);
    EXPECT_EQ(cookedResolution.failure, ResourceLoadFailureCode::PathEscapesRoot);
    EXPECT_TRUE(cookedResolution.cookedArtifactRead);
    EXPECT_NE(cookedResolution.diagnosticMessage.find("escapes"), std::string::npos);

    std::error_code removeError;
    fs::remove_all(sourceRoot, removeError);
    fs::remove_all(cookedRoot, removeError);
}

TEST(ResourceRuntimePolicyValidation, RejectsMountedRootSymlinkEscapesWhenSupported)
{
    const fs::path sourceRoot = MakeTempDirectory("SourceRootSymlinkEscape");
    const fs::path outsideRoot = MakeTempDirectory("OutsideSourceRootSymlinkTarget");
    const fs::path outsideAssetDirectory = outsideRoot / "assets";
    fs::create_directories(outsideAssetDirectory);

    const fs::path escapedAssetPath = outsideAssetDirectory / "secret.png";
    {
        std::ofstream file(escapedAssetPath, std::ios::binary);
        file << "not actually an image";
    }

    const fs::path symlinkPath = sourceRoot / "external";
    std::error_code linkError;
    fs::create_directory_symlink(outsideRoot, symlinkPath, linkError);
    if (linkError)
    {
        std::error_code removeError;
        fs::remove_all(sourceRoot, removeError);
        fs::remove_all(outsideRoot, removeError);
        GTEST_SKIP() << "Directory symlinks are unavailable in this environment: " << linkError.message();
    }

    ResourceRuntimePolicy policy;
    policy.mode = ResourceRuntimeMode::Editor;
    policy.allowSourceAssetReads = true;
    policy.sourceRoot = sourceRoot.string();

    const ResourcePathResolution resolution =
        ResolveRuntimeResourcePath(policy, "", "source://external/assets/secret.png");
    EXPECT_FALSE(resolution.allowed);
    EXPECT_EQ(resolution.domain, ResourceLoadDomain::SourceAsset);
    EXPECT_EQ(resolution.failure, ResourceLoadFailureCode::PathEscapesRoot);
    EXPECT_TRUE(resolution.sourceAssetRead);
    EXPECT_NE(resolution.diagnosticMessage.find("outside"), std::string::npos);

    std::error_code removeError;
    fs::remove(symlinkPath, removeError);
    fs::remove_all(sourceRoot, removeError);
    fs::remove_all(outsideRoot, removeError);
}

TEST(ResourceRuntimePolicyValidation, CookedShaderArtifactExposesStableRuntimeContract)
{
    const fs::path root = MakeTempDirectory("ShaderContract");
    const fs::path shaderPath = root / "shaders" / "fullscreen.rva";
    const std::vector<uint8> bytecode = {0x44, 0x58, 0x42, 0x43, 0x00, 0x01, 0x02, 0x03};
    const std::string glsl = "#version 450\nvoid main() {}\n";
    const std::string msl = "fragment float4 main0() { return float4(1.0); }\n";
    WriteShaderArtifact(shaderPath, bytecode, glsl, msl);

    ResourceManagerConfig config;
    config.asyncThreadCount = 0;
    config.runtimePolicy.mode = ResourceRuntimeMode::CookedRuntime;
    config.runtimePolicy.allowSourceAssetReads = false;
    config.runtimePolicy.requireCookedArtifacts = true;
    config.runtimePolicy.cookedRoot = root.string();

    ResourceManagerTestGuard guard(config);
    auto& manager = ResourceManager::Get();

    IResource* resource = manager.LoadResource("cooked://shaders/fullscreen.rva");
    ASSERT_NE(resource, nullptr);
    ASSERT_EQ(resource->GetType(), ResourceType::Shader);

    auto* shader = dynamic_cast<ShaderResource*>(resource);
    ASSERT_NE(shader, nullptr);
    EXPECT_EQ(shader->GetMetadata().schemaVersion, RVX_SHADER_RESOURCE_METADATA_SCHEMA_VERSION);
    EXPECT_EQ(shader->GetBackend(), ShaderBackendType::DX12);
    EXPECT_EQ(shader->GetStage(), ShaderStage::Pixel);
    EXPECT_EQ(shader->GetEntryPoint(), "main");
    EXPECT_EQ(shader->GetTargetProfile(), "ps_6_0");
    EXPECT_EQ(shader->GetSourceHash(), 123456789u);
    EXPECT_EQ(shader->GetReflectionResourceCount(), 3u);

    const ShaderRuntimeContract& contract = shader->GetRuntimeContract();
    EXPECT_EQ(contract.schemaVersion, RVX_SHADER_RUNTIME_CONTRACT_SCHEMA_VERSION);
    EXPECT_TRUE(contract.valid);
    EXPECT_EQ(contract.status, ShaderRuntimeContractStatus::Valid);
    EXPECT_EQ(contract.sourceHash, 123456789u);
    EXPECT_EQ(contract.reflectionResourceCount, 3u);
    EXPECT_EQ(contract.bytecodeSize, bytecode.size());
    EXPECT_EQ(contract.glslSourceSize, glsl.size());
    EXPECT_EQ(contract.mslSourceSize, msl.size());
    EXPECT_NE(contract.payloadHash, 0u);
    EXPECT_NE(contract.contractHash, 0u);
    EXPECT_EQ(shader->GetRuntimeContractHash(), contract.contractHash);
    EXPECT_TRUE(shader->HasValidRuntimeContract());
    EXPECT_NE(contract.cacheKey.find("backend=DX12"), std::string::npos);
    EXPECT_NE(contract.cacheKey.find("stage=Pixel"), std::string::npos);
    EXPECT_NE(contract.cacheKey.find("entry=main"), std::string::npos);
    EXPECT_NE(contract.cacheKey.find("targetProfile=ps_6_0"), std::string::npos);
    EXPECT_NE(contract.cacheKey.find("reflectionResources=3"), std::string::npos);
    EXPECT_NE(contract.cacheKey.find("bytecodeSize=8"), std::string::npos);

    const std::string contractJson = shader->ExportRuntimeContractJson();
    EXPECT_NE(contractJson.find("\"schemaVersion\": 1"), std::string::npos);
    EXPECT_NE(contractJson.find("\"schemaId\": \"RVX.Resource.ShaderRuntimeContract\""),
              std::string::npos);
    EXPECT_NE(contractJson.find("\"id\": \"shaderRuntimeContractJson\""), std::string::npos);
    EXPECT_NE(contractJson.find("\"kind\": \"ShaderRuntimeContractJson\""), std::string::npos);
    EXPECT_NE(contractJson.find("\"contentType\": \"application/json\""), std::string::npos);
    EXPECT_NE(contractJson.find("\"contentHash\": \"\""), std::string::npos);
    EXPECT_NE(contractJson.find("\"relativePath\": \"\""), std::string::npos);
    EXPECT_NE(contractJson.find("\"resource\": {"), std::string::npos);
    EXPECT_NE(contractJson.find("\"type\": \"Shader\""), std::string::npos);
    EXPECT_NE(contractJson.find("\"metadata\": {"), std::string::npos);
    EXPECT_NE(contractJson.find("\"sourcePath\": \"Shaders/Fullscreen.ps.hlsl\""), std::string::npos);
    EXPECT_NE(contractJson.find("\"backend\": \"DX12\""), std::string::npos);
    EXPECT_NE(contractJson.find("\"stage\": \"Pixel\""), std::string::npos);
    EXPECT_NE(contractJson.find("\"entryPoint\": \"main\""), std::string::npos);
    EXPECT_NE(contractJson.find("\"targetProfile\": \"ps_6_0\""), std::string::npos);
    EXPECT_NE(contractJson.find("\"sourceHash\": 123456789"), std::string::npos);
    EXPECT_NE(contractJson.find("\"reflectionResourceCount\": 3"), std::string::npos);
    EXPECT_NE(contractJson.find("\"contract\": {"), std::string::npos);
    EXPECT_NE(contractJson.find("\"valid\": true"), std::string::npos);
    EXPECT_NE(contractJson.find("\"status\": \"Valid\""), std::string::npos);
    EXPECT_NE(contractJson.find("\"diagnosticMessage\": \"Shader runtime contract is valid.\""),
              std::string::npos);
    EXPECT_NE(contractJson.find("\"cacheKey\": "), std::string::npos);
    EXPECT_NE(contractJson.find("\"contractHash\": " + std::to_string(contract.contractHash)),
              std::string::npos);
    EXPECT_NE(contractJson.find("\"payloadHash\": " + std::to_string(contract.payloadHash)),
              std::string::npos);
    EXPECT_NE(contractJson.find("\"bytecodeSize\": 8"), std::string::npos);
    EXPECT_NE(contractJson.find("\"glslSourceSize\": " + std::to_string(glsl.size())),
              std::string::npos);
    EXPECT_NE(contractJson.find("\"mslSourceSize\": " + std::to_string(msl.size())),
              std::string::npos);
    EXPECT_FALSE(shader->SaveRuntimeContractJson(nullptr));
    EXPECT_FALSE(shader->SaveRuntimeContractJson(""));

    const fs::path contractJsonPath = root / "shaders" / "fullscreen.shader-contract.json";
    const std::string contractJsonPathString = contractJsonPath.string();
    ASSERT_TRUE(shader->SaveRuntimeContractJson(contractJsonPathString.c_str()));
    EXPECT_EQ(ReadTextFile(contractJsonPath), contractJson);

    const ResourceLoadDiagnostic diagnostic = manager.GetLastLoadDiagnostic();
    EXPECT_TRUE(diagnostic.success);
    EXPECT_EQ(diagnostic.domain, ResourceLoadDomain::CookedArtifact);
    EXPECT_EQ(diagnostic.failure, ResourceLoadFailureCode::None);
    EXPECT_TRUE(diagnostic.cookedArtifactRead);
    EXPECT_FALSE(diagnostic.sourceAssetRead);

    std::error_code removeError;
    fs::remove_all(root, removeError);
}

TEST(ResourceRuntimePolicyValidation, ShaderRuntimeContractJsonReportsInvalidContracts)
{
    ShaderResource shader;
    shader.SetId(701);
    shader.SetName("InvalidShader");
    shader.SetPath("cooked://shaders/invalid.rva");

    ShaderMetadata metadata;
    metadata.backend = ShaderBackendType::DX12;
    metadata.stage = ShaderStage::Pixel;
    metadata.entryPoint = "main";
    metadata.targetProfile = "ps_6_0";
    metadata.sourceHash = 42;
    metadata.reflectionResourceCount = 1;
    shader.SetData({0x44, 0x58, 0x42, 0x43}, {}, {}, metadata);

    const ShaderRuntimeContract& contract = shader.GetRuntimeContract();
    EXPECT_FALSE(contract.valid);
    EXPECT_EQ(contract.status, ShaderRuntimeContractStatus::MissingSourcePath);
    EXPECT_NE(contract.payloadHash, 0u);
    EXPECT_EQ(contract.contractHash, 0u);
    EXPECT_TRUE(contract.cacheKey.empty());

    const std::string contractJson = shader.ExportRuntimeContractJson();
    EXPECT_NE(contractJson.find("\"schemaId\": \"RVX.Resource.ShaderRuntimeContract\""),
              std::string::npos);
    EXPECT_NE(contractJson.find("\"id\": \"shaderRuntimeContractJson\""), std::string::npos);
    EXPECT_NE(contractJson.find("\"kind\": \"ShaderRuntimeContractJson\""), std::string::npos);
    EXPECT_NE(contractJson.find("\"name\": \"InvalidShader\""), std::string::npos);
    EXPECT_NE(contractJson.find("\"path\": \"cooked://shaders/invalid.rva\""), std::string::npos);
    EXPECT_NE(contractJson.find("\"sourcePath\": \"\""), std::string::npos);
    EXPECT_NE(contractJson.find("\"backend\": \"DX12\""), std::string::npos);
    EXPECT_NE(contractJson.find("\"stage\": \"Pixel\""), std::string::npos);
    EXPECT_NE(contractJson.find("\"valid\": false"), std::string::npos);
    EXPECT_NE(contractJson.find("\"status\": \"MissingSourcePath\""), std::string::npos);
    EXPECT_NE(contractJson.find("\"diagnosticMessage\": \"Shader runtime contract is missing source path metadata.\""),
              std::string::npos);
    EXPECT_NE(contractJson.find("\"cacheKey\": \"\""), std::string::npos);
    EXPECT_NE(contractJson.find("\"contractHash\": 0"), std::string::npos);
    EXPECT_NE(contractJson.find("\"payloadHash\": " + std::to_string(contract.payloadHash)),
              std::string::npos);
    EXPECT_NE(contractJson.find("\"bytecodeSize\": 4"), std::string::npos);
}

TEST(ResourceRuntimePolicyValidation, AppModeBuildsEditorPreviewAndRuntimeResourcePolicies)
{
    const ResourceManagerConfig editorConfig =
        MakeResourceManagerConfigForAppMode(AppMode::Editor);
    const ResourceRuntimePolicy editorPolicy =
        editorConfig.runtimePolicy;
    EXPECT_EQ(editorPolicy.mode, ResourceRuntimeMode::Editor);
    EXPECT_TRUE(editorPolicy.allowSourceAssetReads);
    EXPECT_FALSE(editorPolicy.requireCookedArtifacts);
    EXPECT_FALSE(editorPolicy.requireRuntimePackage);
    EXPECT_TRUE(editorConfig.enableHotReload);

    const ResourceManagerConfig previewConfig =
        MakeResourceManagerConfigForAppMode(AppMode::Preview);
    const ResourceRuntimePolicy previewPolicy =
        previewConfig.runtimePolicy;
    EXPECT_EQ(previewPolicy.mode, ResourceRuntimeMode::Editor);
    EXPECT_TRUE(previewPolicy.allowSourceAssetReads);
    EXPECT_FALSE(previewPolicy.requireCookedArtifacts);
    EXPECT_FALSE(previewPolicy.requireRuntimePackage);
    EXPECT_TRUE(previewConfig.enableHotReload);

    const ResourceManagerConfig runtimeConfig =
        MakeResourceManagerConfigForAppMode(AppMode::Runtime);
    const ResourceRuntimePolicy runtimePolicy =
        runtimeConfig.runtimePolicy;
    EXPECT_EQ(runtimePolicy.mode, ResourceRuntimeMode::CookedRuntime);
    EXPECT_FALSE(runtimePolicy.allowSourceAssetReads);
    EXPECT_TRUE(runtimePolicy.requireCookedArtifacts);
    EXPECT_FALSE(runtimePolicy.requireRuntimePackage);
    EXPECT_FALSE(runtimeConfig.enableHotReload);

    const ResourceManagerConfig pieConfig =
        MakeResourceManagerConfigForAppMode(AppMode::PlayInEditor);
    const ResourceRuntimePolicy piePolicy =
        pieConfig.runtimePolicy;
    EXPECT_EQ(piePolicy.mode, ResourceRuntimeMode::CookedRuntime);
    EXPECT_FALSE(piePolicy.allowSourceAssetReads);
    EXPECT_TRUE(piePolicy.requireCookedArtifacts);
    EXPECT_FALSE(piePolicy.requireRuntimePackage);
    EXPECT_FALSE(pieConfig.enableHotReload);

    const ResourceManagerConfig cookConfig =
        MakeResourceManagerConfigForAppMode(AppMode::Cook);
    const ResourceRuntimePolicy cookPolicy =
        cookConfig.runtimePolicy;
    EXPECT_EQ(cookPolicy.mode, ResourceRuntimeMode::Editor);
    EXPECT_TRUE(cookPolicy.allowSourceAssetReads);
    EXPECT_FALSE(cookPolicy.requireCookedArtifacts);
    EXPECT_FALSE(cookPolicy.requireRuntimePackage);
    EXPECT_FALSE(cookConfig.enableHotReload);

    const ResourceManagerConfig testConfig =
        MakeResourceManagerConfigForAppMode(AppMode::Test);
    const ResourceRuntimePolicy testPolicy =
        testConfig.runtimePolicy;
    EXPECT_EQ(testPolicy.mode, ResourceRuntimeMode::Editor);
    EXPECT_TRUE(testPolicy.allowSourceAssetReads);
    EXPECT_FALSE(testPolicy.requireCookedArtifacts);
    EXPECT_FALSE(testPolicy.requireRuntimePackage);
    EXPECT_FALSE(testConfig.enableHotReload);

    const ResourcePathResolution runtimeSource =
        ResolveRuntimeResourcePath(runtimePolicy, "", "source://textures/albedo.png");
    EXPECT_FALSE(runtimeSource.allowed);
    EXPECT_EQ(runtimeSource.failure, ResourceLoadFailureCode::CookedArtifactRequired);

    const ResourcePathResolution editorSource =
        ResolveRuntimeResourcePath(editorPolicy, "", "source://textures/albedo.png");
    EXPECT_TRUE(editorSource.allowed);
    EXPECT_EQ(editorSource.domain, ResourceLoadDomain::SourceAsset);

    const ResourcePathResolution cookSource =
        ResolveRuntimeResourcePath(cookPolicy, "", "source://textures/albedo.png");
    EXPECT_TRUE(cookSource.allowed);
    EXPECT_EQ(cookSource.domain, ResourceLoadDomain::SourceAsset);

    const ResourcePathResolution testSource =
        ResolveRuntimeResourcePath(testPolicy, "", "source://textures/albedo.png");
    EXPECT_TRUE(testSource.allowed);
    EXPECT_EQ(testSource.domain, ResourceLoadDomain::SourceAsset);
}

TEST(ResourceRuntimePolicyValidation, ResourceAsyncPathStaysOnCoreJobSystemContracts)
{
    const fs::path repoRoot = FindRepositoryRoot();
    ASSERT_FALSE(repoRoot.empty());

    const std::string managerHeader =
        ReadTextFile(repoRoot / "Resource/Include/Resource/ResourceManager.h");
    const std::string managerSource =
        ReadTextFile(repoRoot / "Resource/Private/ResourceManager.cpp");
    const std::string resourceSource =
        ReadTextFile(repoRoot / "Resource/Private/IResource.cpp");
    ASSERT_FALSE(managerHeader.empty());
    ASSERT_FALSE(managerSource.empty());
    ASSERT_FALSE(resourceSource.empty());

    EXPECT_NE(managerHeader.find("JobSubmissionDesc desc"), std::string::npos);
    EXPECT_NE(managerHeader.find("desc.category = \"Resource.LoadAsync\""), std::string::npos);
    EXPECT_NE(managerHeader.find("JobCompletionDispatch::MainThread"), std::string::npos);
    EXPECT_NE(managerHeader.find("JobSystem::Get().SubmitWithResult"), std::string::npos);
    EXPECT_NE(managerHeader.find("JobSystem::Get().Submit"), std::string::npos);
    EXPECT_NE(managerSource.find("JobSystem::Get().ProcessMainThreadCompletions()"), std::string::npos);

    EXPECT_EQ(managerHeader.find("std::thread"), std::string::npos);
    EXPECT_EQ(managerHeader.find("ThreadPool"), std::string::npos);
    EXPECT_EQ(managerHeader.find("condition_variable"), std::string::npos);
    EXPECT_EQ(managerSource.find("std::thread"), std::string::npos);
    EXPECT_EQ(managerSource.find("ThreadPool"), std::string::npos);
    EXPECT_EQ(managerSource.find("std::this_thread::sleep_for"), std::string::npos);
    EXPECT_EQ(managerSource.find("std::this_thread::yield"), std::string::npos);

    EXPECT_NE(resourceSource.find("m_stateCondition.wait("), std::string::npos);
    EXPECT_NE(resourceSource.find("m_stateCondition.wait_for("), std::string::npos);
    EXPECT_EQ(resourceSource.find("std::this_thread::sleep_for"), std::string::npos);
    EXPECT_EQ(resourceSource.find("std::this_thread::yield"), std::string::npos);
}

TEST(ResourceRuntimePolicyValidation, LoadAsyncRunsInlineWhenAsyncDisabledAndJobSystemMissing)
{
    JobSystem::Get().Shutdown();

    const fs::path root = MakeTempDirectory("InlineAsync");
    const fs::path sourcePath = root / "textures" / "inline.png";
    WriteTextFile(sourcePath, "source texture placeholder");

    ResourceManagerConfig config;
    config.asyncThreadCount = 0;
    config.runtimePolicy.mode = ResourceRuntimeMode::Editor;
    config.runtimePolicy.allowSourceAssetReads = true;
    config.runtimePolicy.sourceRoot = root.string();

    ResourceManagerTestGuard guard(config);
    auto& manager = ResourceManager::Get();

    auto loader = std::make_unique<RecordingLoader>(ResourceType::Texture);
    RecordingLoader* loaderPtr = loader.get();
    manager.RegisterLoader(ResourceType::Texture, std::move(loader));

    std::future<ResourceHandle<RecordingResource>> future =
        manager.LoadAsync<RecordingResource>("source://textures/inline.png");

    EXPECT_EQ(future.wait_for(std::chrono::seconds(0)), std::future_status::ready);
    ResourceHandle<RecordingResource> handle = future.get();
    EXPECT_TRUE(handle.IsValid());
    EXPECT_EQ(loaderPtr->loadCount, 1u);
    ExpectEquivalentExistingPath(loaderPtr->lastPath, sourcePath);
    EXPECT_EQ(manager.GetStats().pendingLoads, static_cast<size_t>(0));

    std::error_code removeError;
    fs::remove_all(root, removeError);
}

TEST(ResourceRuntimePolicyValidation, LoadAsyncUsesCoreJobSystemWhenEnabled)
{
    JobSystem::Get().Shutdown();

    const fs::path root = MakeTempDirectory("AsyncJobSystem");
    const fs::path sourcePath = root / "textures" / "async.png";
    WriteTextFile(sourcePath, "source texture placeholder");

    ResourceManagerConfig config;
    config.asyncThreadCount = 1;
    config.runtimePolicy.mode = ResourceRuntimeMode::Editor;
    config.runtimePolicy.allowSourceAssetReads = true;
    config.runtimePolicy.sourceRoot = root.string();

    std::promise<void> releaseLoadPromise;
    std::shared_future<void> releaseLoadFuture = releaseLoadPromise.get_future().share();
    releaseLoadPromise.set_value();

    ResourceManagerTestGuard guard(config);
    auto& manager = ResourceManager::Get();

    auto loader = std::make_unique<BlockingLoader>(nullptr, releaseLoadFuture);
    BlockingLoader* loaderPtr = loader.get();
    manager.RegisterLoader(ResourceType::Texture, std::move(loader));

    std::future<ResourceHandle<RecordingResource>> future =
        manager.LoadAsync<RecordingResource>("source://textures/async.png");
    ASSERT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    ResourceHandle<RecordingResource> handle = future.get();
    EXPECT_TRUE(handle.IsValid());
    EXPECT_EQ(loaderPtr->loadCount.load(std::memory_order_relaxed), 1u);
    ExpectEquivalentExistingPath(loaderPtr->lastPath, sourcePath);
    EXPECT_NE(loaderPtr->loaderThreadId, std::this_thread::get_id());
    EXPECT_EQ(manager.GetStats().pendingLoads, static_cast<size_t>(0));

    std::error_code removeError;
    fs::remove_all(root, removeError);
}

TEST(ResourceRuntimePolicyValidation, LoadAsyncCallbackDispatchesOnlyFromProcessCompletedLoadsThread)
{
    JobSystem::Get().Shutdown();

    const fs::path root = MakeTempDirectory("AsyncCallbackPump");
    const fs::path sourcePath = root / "textures" / "async.png";
    WriteTextFile(sourcePath, "source texture placeholder");

    ResourceManagerConfig config;
    config.asyncThreadCount = 0;
    config.runtimePolicy.mode = ResourceRuntimeMode::Editor;
    config.runtimePolicy.allowSourceAssetReads = true;
    config.runtimePolicy.sourceRoot = root.string();

    ResourceManagerTestGuard guard(config);
    auto& manager = ResourceManager::Get();

    auto loader = std::make_unique<RecordingLoader>(ResourceType::Texture);
    RecordingLoader* loaderPtr = loader.get();
    manager.RegisterLoader(ResourceType::Texture, std::move(loader));

    const std::thread::id pumpThreadId = std::this_thread::get_id();
    bool callbackCalled = false;
    std::thread::id callbackThreadId;

    manager.LoadAsync<RecordingResource>(
        "source://textures/async.png",
        [&](ResourceHandle<RecordingResource> handle)
        {
            callbackThreadId = std::this_thread::get_id();
            callbackCalled = true;
            EXPECT_TRUE(handle.IsValid());
        });

    EXPECT_EQ(loaderPtr->loadCount, 1u);
    ExpectEquivalentExistingPath(loaderPtr->lastPath, sourcePath);
    EXPECT_FALSE(callbackCalled);
    EXPECT_EQ(manager.GetStats().pendingLoads, static_cast<size_t>(1));

    manager.ProcessCompletedLoads();
    EXPECT_TRUE(callbackCalled);
    EXPECT_EQ(callbackThreadId, pumpThreadId);
    EXPECT_EQ(manager.GetStats().pendingLoads, static_cast<size_t>(0));

    std::error_code removeError;
    fs::remove_all(root, removeError);
}

TEST(ResourceRuntimePolicyValidation, ResourceManagerDoesNotOwnPreexistingJobSystem)
{
    JobSystem::Get().Shutdown();
    JobSystem::Get().Initialize(1);
    ASSERT_TRUE(JobSystem::Get().IsInitialized());

    ResourceManagerConfig config;
    config.asyncThreadCount = 2;
    config.runtimePolicy.mode = ResourceRuntimeMode::Editor;
    config.runtimePolicy.allowSourceAssetReads = true;

    {
        ResourceManagerTestGuard guard(config);
        EXPECT_TRUE(JobSystem::Get().IsInitialized());
    }

    EXPECT_TRUE(JobSystem::Get().IsInitialized());
    JobSystem::Get().Shutdown();
}

TEST(ResourceRuntimePolicyValidation, HotReloadRejectedByCookedRuntimePolicy)
{
    ResourceManagerConfig config;
    config.asyncThreadCount = 0;
    config.enableHotReload = true;
    config.runtimePolicy.mode = ResourceRuntimeMode::CookedRuntime;
    config.runtimePolicy.allowSourceAssetReads = false;
    config.runtimePolicy.requireCookedArtifacts = true;

    ResourceManagerTestGuard guard(config);
    auto& manager = ResourceManager::Get();

    EXPECT_FALSE(manager.IsHotReloadEnabled());

    const ResourceHotReloadDiagnostic diagnostic = manager.GetHotReloadDiagnostic();
    EXPECT_TRUE(diagnostic.requested);
    EXPECT_FALSE(diagnostic.enabled);
    EXPECT_TRUE(diagnostic.sourceAssetAccessRequired);
    EXPECT_EQ(diagnostic.status, ResourceHotReloadStatus::UnsupportedRuntimePolicy);
    EXPECT_STREQ(GetResourceHotReloadStatusName(diagnostic.status), "UnsupportedRuntimePolicy");
    EXPECT_NE(diagnostic.message.find("requires source asset reads"), std::string::npos);
    EXPECT_EQ(diagnostic.watchedFileCount, static_cast<size_t>(0));
    EXPECT_EQ(diagnostic.registeredResourceCount, static_cast<size_t>(0));

    const std::string diagnosticJson = manager.ExportHotReloadDiagnosticJson();
    EXPECT_EQ(diagnosticJson, ExportResourceHotReloadDiagnosticJson(diagnostic));
    EXPECT_NE(diagnosticJson.find("\"schemaVersion\": 1"), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"schemaId\": \"RVX.Resource.HotReloadDiagnostic\""),
              std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"id\": \"resourceHotReloadDiagnosticJson\""),
              std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"kind\": \"ResourceHotReloadDiagnosticJson\""),
              std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"contentType\": \"application/json\""), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"contentHash\": \"\""), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"relativePath\": \"\""), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"requested\": true"), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"enabled\": false"), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"sourceAssetAccessRequired\": true"), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"watcherInitialized\": false"), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"status\": \"UnsupportedRuntimePolicy\""), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"statusCode\": 2"), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"watchedFileCount\": 0"), std::string::npos);
    EXPECT_NE(diagnosticJson.find("\"registeredResourceCount\": 0"), std::string::npos);
    EXPECT_FALSE(manager.SaveHotReloadDiagnosticJson(nullptr));
    EXPECT_FALSE(SaveResourceHotReloadDiagnosticJson("", diagnostic));

    const fs::path diagnosticRoot = MakeTempDirectory("HotReloadRejectedDiagnostic");
    const fs::path diagnosticPath = diagnosticRoot / "hot-reload-diagnostic.json";
    const std::string diagnosticPathString = diagnosticPath.string();
    ASSERT_TRUE(manager.SaveHotReloadDiagnosticJson(diagnosticPathString.c_str()));
    EXPECT_EQ(ReadTextFile(diagnosticPath), diagnosticJson);

    std::error_code removeError;
    fs::remove_all(diagnosticRoot, removeError);
}

TEST(ResourceRuntimePolicyValidation, EditorHotReloadTracksSourceResourceLoads)
{
    const fs::path root = MakeTempDirectory("EditorHotReload");
    const fs::path sourcePath = root / "textures" / "albedo.png";
    WriteTextFile(sourcePath, "source texture placeholder");

    ResourceManagerConfig config;
    config.asyncThreadCount = 0;
    config.enableHotReload = true;
    config.runtimePolicy.mode = ResourceRuntimeMode::Editor;
    config.runtimePolicy.allowSourceAssetReads = true;
    config.runtimePolicy.sourceRoot = root.string();

    {
        ResourceManagerTestGuard guard(config);
        auto& manager = ResourceManager::Get();

        EXPECT_TRUE(manager.IsHotReloadEnabled());
        EXPECT_EQ(manager.GetHotReloadDiagnostic().status, ResourceHotReloadStatus::Active);

        auto loader = std::make_unique<RecordingLoader>(ResourceType::Texture);
        RecordingLoader* loaderPtr = loader.get();
        manager.RegisterLoader(ResourceType::Texture, std::move(loader));

        IResource* resource = manager.LoadResource("source://textures/albedo.png");
        ASSERT_NE(resource, nullptr);
        EXPECT_EQ(loaderPtr->loadCount, 1u);
        ExpectEquivalentExistingPath(loaderPtr->lastPath, sourcePath);

        const ResourceHotReloadDiagnostic diagnostic = manager.GetHotReloadDiagnostic();
        EXPECT_TRUE(diagnostic.requested);
        EXPECT_TRUE(diagnostic.enabled);
        EXPECT_TRUE(diagnostic.watcherInitialized);
        EXPECT_EQ(diagnostic.status, ResourceHotReloadStatus::Active);
        EXPECT_STREQ(GetResourceHotReloadStatusName(diagnostic.status), "Active");
        EXPECT_EQ(diagnostic.registeredResourceCount, static_cast<size_t>(1));
        EXPECT_EQ(diagnostic.watchedFileCount, static_cast<size_t>(1));
        EXPECT_NE(diagnostic.message.find("tracking loaded source assets"), std::string::npos);

        const std::string diagnosticJson = manager.ExportHotReloadDiagnosticJson();
        EXPECT_EQ(diagnosticJson, ExportResourceHotReloadDiagnosticJson(diagnostic));
        EXPECT_NE(diagnosticJson.find("\"schemaId\": \"RVX.Resource.HotReloadDiagnostic\""),
                  std::string::npos);
        EXPECT_NE(diagnosticJson.find("\"id\": \"resourceHotReloadDiagnosticJson\""),
                  std::string::npos);
        EXPECT_NE(diagnosticJson.find("\"kind\": \"ResourceHotReloadDiagnosticJson\""),
                  std::string::npos);
        EXPECT_NE(diagnosticJson.find("\"requested\": true"), std::string::npos);
        EXPECT_NE(diagnosticJson.find("\"enabled\": true"), std::string::npos);
        EXPECT_NE(diagnosticJson.find("\"watcherInitialized\": true"), std::string::npos);
        EXPECT_NE(diagnosticJson.find("\"status\": \"Active\""), std::string::npos);
        EXPECT_NE(diagnosticJson.find("\"statusCode\": 1"), std::string::npos);
        EXPECT_NE(diagnosticJson.find("\"watchedFileCount\": 1"), std::string::npos);
        EXPECT_NE(diagnosticJson.find("\"registeredResourceCount\": 1"), std::string::npos);

        const fs::path diagnosticPath = root / "diagnostics" / "hot-reload-diagnostic.json";
        fs::create_directories(diagnosticPath.parent_path());
        const std::string diagnosticPathString = diagnosticPath.string();
        ASSERT_TRUE(SaveResourceHotReloadDiagnosticJson(diagnosticPathString.c_str(), diagnostic));
        EXPECT_EQ(ReadTextFile(diagnosticPath), diagnosticJson);

        manager.CheckForChanges();
        EXPECT_EQ(manager.GetHotReloadDiagnostic().status, ResourceHotReloadStatus::Active);
    }

    std::error_code removeError;
    fs::remove_all(root, removeError);
}

namespace
{
    class RenderTextureLoader final : public IResourceLoader
    {
    public:
        ResourceType GetResourceType() const override
        {
            return ResourceType::Texture;
        }

        std::vector<std::string> GetSupportedExtensions() const override
        {
            return {".png"};
        }

        IResource* Load(const std::string&) override
        {
            auto* texture = new TextureResource();
            TextureMetadata metadata;
            metadata.width = 2;
            metadata.height = 2;
            metadata.format = TextureFormat::RGBA8;
            texture->SetData(std::vector<uint8>(16, ++loadValue), metadata);
            return texture;
        }

        uint8 loadValue = 0;
    };

    class FakeResourceGateway final : public IRenderResourceGateway
    {
    public:
        RenderResourceReserveResult ReserveResource(
            AssetId assetId,
            RenderResourceKind kind) noexcept override
        {
            (void)kind;
            ++reserveAttempts;
            if (shuttingDown)
            {
                return {RenderResourceReserveCode::ShuttingDown, {}, {}};
            }
            const auto existing = assets.find(assetId);
            if (existing != assets.end())
            {
                RenderResourceReserveResult result;
                result.code = RenderResourceReserveCode::Existing;
                result.handle = existing->second;
                result.status = QueryResourceStatus(result.handle);
                return result;
            }

            const RenderResourceHandle handle{nextSlot++, 1};
            assets.emplace(assetId, handle);
            handles.emplace(handle, assetId);
            statuses.emplace(handle,
                             RenderResourceStatus{
                                 RenderResourceStatusCode::Current,
                                 RenderResourcePublicState::Reserved,
                                 RenderResourceFailureCode::None});
            RenderResourceReserveResult result;
            result.code = RenderResourceReserveCode::Reserved;
            result.handle = handle;
            result.status = statuses.at(handle);
            return result;
        }

        RenderUploadEnqueueResult TryEnqueueUpload(
            const ResourceUploadRequestRef& request) noexcept override
        {
            ++enqueueAttempts;
            if (shuttingDown)
                return {RenderUploadEnqueueCode::ShuttingDown};
            if (pressureCount != 0)
            {
                --pressureCount;
                return {pressureCode};
            }
            if (!request)
                return {RenderUploadEnqueueCode::InvalidRequest};

            auto status = statuses.find(request->GetHandle());
            if (status == statuses.end())
                return {RenderUploadEnqueueCode::StaleGeneration};
            status->second.state = RenderResourcePublicState::UploadQueued;
            acceptedRequests[request->GetHandle()] = request;
            ++acceptedCount;
            return {RenderUploadEnqueueCode::Accepted};
        }

        RenderReleaseResult RequestRelease(
            RenderResourceHandle handle) noexcept override
        {
            ++releaseAttempts;
            if (shuttingDown)
                return {RenderReleaseCode::ShuttingDown};
            auto status = statuses.find(handle);
            if (status == statuses.end())
                return {RenderReleaseCode::StaleGeneration};
            if (status->second.state == RenderResourcePublicState::Evicting ||
                status->second.state == RenderResourcePublicState::Released)
            {
                return {RenderReleaseCode::AlreadyPending};
            }
            status->second.state = RenderResourcePublicState::Evicting;
            const AssetId asset = handles.at(handle);
            assets.erase(asset);
            return {RenderReleaseCode::Accepted};
        }

        RenderResourceStatus QueryResourceStatus(
            RenderResourceHandle handle) const noexcept override
        {
            const auto status = statuses.find(handle);
            if (status == statuses.end())
            {
                return {RenderResourceStatusCode::StaleGeneration,
                        RenderResourcePublicState::Released,
                        RenderResourceFailureCode::None};
            }
            return status->second;
        }

        void PublishTerminal(RenderResourceHandle handle,
                             RenderResourcePublicState state,
                             RenderResourceFailureCode failure =
                                 RenderResourceFailureCode::None)
        {
            auto& status = statuses.at(handle);
            status.state = state;
            status.failure = failure;
            acceptedRequests.erase(handle);
        }

        std::weak_ptr<const ResourceUploadRequest> GetAcceptedRequest(
            RenderResourceHandle handle) const
        {
            const auto request = acceptedRequests.find(handle);
            return request == acceptedRequests.end()
                       ? std::weak_ptr<const ResourceUploadRequest>{}
                       : std::weak_ptr<const ResourceUploadRequest>{request->second};
        }

        uint32 nextSlot = 1;
        uint32 pressureCount = 0;
        RenderUploadEnqueueCode pressureCode =
            RenderUploadEnqueueCode::QueueFullByCount;
        uint32 reserveAttempts = 0;
        uint32 enqueueAttempts = 0;
        uint32 acceptedCount = 0;
        uint32 releaseAttempts = 0;
        bool shuttingDown = false;
        std::unordered_map<AssetId, RenderResourceHandle, AssetIdHash> assets;
        std::unordered_map<RenderResourceHandle,
                           AssetId,
                           RenderResourceHandleHash> handles;
        std::unordered_map<RenderResourceHandle,
                           RenderResourceStatus,
                           RenderResourceHandleHash> statuses;
        std::unordered_map<RenderResourceHandle,
                           ResourceUploadRequestRef,
                           RenderResourceHandleHash> acceptedRequests;
    };

    ResourceManagerConfig MakeRenderResourceTestConfig(const fs::path& root)
    {
        ResourceManagerConfig config;
        config.asyncThreadCount = 0;
        config.runtimePolicy.mode = ResourceRuntimeMode::Editor;
        config.runtimePolicy.allowSourceAssetReads = true;
        config.runtimePolicy.sourceRoot = root.string();
        return config;
    }
} // namespace

TEST(ResourceRuntimePolicyValidation, RenderUploadBuilderOwnsMeshAndTextureBytes)
{
    MeshResource meshResource;
    meshResource.SetId(101);
    auto mesh = std::make_shared<Mesh>();
    const std::vector<Vec3> positions = {
        {-1.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f}};
    mesh->SetPositions(positions);
    mesh->SetNormals(std::vector<Vec3>(3, Vec3{0.0f, 0.0f, 1.0f}));
    mesh->SetUVs({{0.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 1.0f}});
    mesh->SetIndices(std::vector<uint16>{0, 1, 2});
    mesh->SetBoundingBox({-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f});
    meshResource.SetMesh(mesh);

    const RenderUploadRequestBuildResult meshBuild =
        RenderUploadRequestBuilder::Build(
            meshResource, {1, 1}, 1, {});
    ASSERT_EQ(meshBuild.code, RenderUploadRequestBuildCode::Built);
    ASSERT_NE(meshBuild.request, nullptr);
    const auto meshPayload =
        std::get<MeshUploadPayload>(meshBuild.request->GetPayload());
    ASSERT_FALSE(meshPayload.bytes.empty());
    EXPECT_FALSE(meshPayload.createInfo.hasTangentBasis);
    ASSERT_EQ(meshPayload.tangentRange.stride, sizeof(Vec4));
    ASSERT_EQ(meshPayload.tangentRange.size,
              positions.size() * sizeof(Vec4));
    ASSERT_LE(meshPayload.tangentRange.offset + meshPayload.tangentRange.size,
              meshPayload.bytes.size());
    for (size_t vertexIndex = 0; vertexIndex < positions.size(); ++vertexIndex)
    {
        Vec4 tangent{};
        std::memcpy(&tangent,
                    meshPayload.bytes.data() + meshPayload.tangentRange.offset +
                        vertexIndex * meshPayload.tangentRange.stride,
                    sizeof(tangent));
        EXPECT_FLOAT_EQ(tangent.x, 1.0f);
        EXPECT_FLOAT_EQ(tangent.y, 0.0f);
        EXPECT_FLOAT_EQ(tangent.z, 0.0f);
        EXPECT_FLOAT_EQ(tangent.w, 1.0f);
    }
    const std::vector<uint8> ownedMeshBytes = meshPayload.bytes;

    mesh->SetPositions(std::vector<Vec3>(3, Vec3{42.0f}));
    EXPECT_EQ(std::get<MeshUploadPayload>(meshBuild.request->GetPayload()).bytes,
              ownedMeshBytes);

    const std::vector<Vec4> authoredTangents = {
        {0.0f, 1.0f, 0.0f, -1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {0.0f, 1.0f, 0.0f, 1.0f}};
    mesh->SetTangents(authoredTangents);
    const RenderUploadRequestBuildResult authoredMeshBuild =
        RenderUploadRequestBuilder::Build(meshResource, {3, 1}, 3, {});
    ASSERT_EQ(authoredMeshBuild.code, RenderUploadRequestBuildCode::Built);
    const auto& authoredMeshPayload =
        std::get<MeshUploadPayload>(authoredMeshBuild.request->GetPayload());
    EXPECT_TRUE(authoredMeshPayload.createInfo.hasTangentBasis);
    ASSERT_EQ(authoredMeshPayload.tangentRange.stride, sizeof(Vec4));
    ASSERT_EQ(authoredMeshPayload.tangentRange.size,
              authoredTangents.size() * sizeof(Vec4));
    for (size_t vertexIndex = 0; vertexIndex < authoredTangents.size();
         ++vertexIndex)
    {
        Vec4 tangent{};
        std::memcpy(&tangent,
                    authoredMeshPayload.bytes.data() +
                        authoredMeshPayload.tangentRange.offset +
                        vertexIndex * authoredMeshPayload.tangentRange.stride,
                    sizeof(tangent));
        EXPECT_FLOAT_EQ(tangent.x, authoredTangents[vertexIndex].x);
        EXPECT_FLOAT_EQ(tangent.y, authoredTangents[vertexIndex].y);
        EXPECT_FLOAT_EQ(tangent.z, authoredTangents[vertexIndex].z);
        EXPECT_FLOAT_EQ(tangent.w, authoredTangents[vertexIndex].w);
    }

    TextureResource texture;
    texture.SetId(102);
    TextureMetadata metadata;
    metadata.width = 2;
    metadata.height = 2;
    metadata.mipLevels = 1;
    metadata.format = TextureFormat::RGBA8;
    texture.SetData(std::vector<uint8>(16, 7), metadata);
    const RenderUploadRequestBuildResult textureBuild =
        RenderUploadRequestBuilder::Build(texture, {2, 1}, 2, {});
    ASSERT_EQ(textureBuild.code, RenderUploadRequestBuildCode::Built);
    const auto& texturePayload =
        std::get<TextureUploadPayload>(textureBuild.request->GetPayload());
    ASSERT_EQ(texturePayload.bytes.size(), 16U);
    ASSERT_EQ(texturePayload.subresources.size(), 1U);
    EXPECT_EQ(texturePayload.subresources.front().rowPitch, 8U);
    EXPECT_EQ(texturePayload.subresources.front().slicePitch, 16U);
    texture.SetData(std::vector<uint8>(16, 99), metadata);
    EXPECT_EQ(std::get<TextureUploadPayload>(textureBuild.request->GetPayload())
                  .bytes.front(),
              7U);
}

TEST(ResourceRuntimePolicyValidation, RenderUploadBuilderResolvesMaterialTextureHandles)
{
    auto* texture = new TextureResource();
    texture->SetId(201);
    TextureMetadata metadata;
    metadata.width = 1;
    metadata.height = 1;
    texture->SetData(std::vector<uint8>(4, 255), metadata);

    MaterialResource material;
    material.SetId(202);
    auto source = std::make_shared<Material>("owned-material");
    source->SetBaseColorTexture(TextureInfo("albedo.png"));
    material.SetMaterialData(source);
    material.SetTexture("albedo", ResourceHandle<TextureResource>(texture));

    const RenderResourceHandle textureHandle{7, 3};
    RenderUploadRequestBuildResult build =
        RenderUploadRequestBuilder::Build(
            material,
            {8, 2},
            3,
            [textureHandle](AssetId assetId, RenderResourceKind kind)
            {
                EXPECT_EQ(assetId, (AssetId{201}));
                EXPECT_EQ(kind, RenderResourceKind::Texture);
                return textureHandle;
            });

    material.SetTexture("albedo", {});
    material.SetMaterialData({});
    source.reset();

    ASSERT_EQ(build.code, RenderUploadRequestBuildCode::Built);
    ASSERT_NE(build.request, nullptr);
    const auto& payload =
        std::get<MaterialUploadPayload>(build.request->GetPayload());
    ASSERT_EQ(payload.textureBindings.size(), 1U);
    EXPECT_EQ(payload.textureBindings.front().texture, textureHandle);
    EXPECT_EQ(build.request->GetDependencies(),
              std::vector<RenderResourceHandle>{textureHandle});
    EXPECT_NE(payload.sourceData.textureFlags, 0U);
}

TEST(ResourceRuntimePolicyValidation, RenderUploadBuilderDescribesCubemapMipStorageExactly)
{
    TextureResource texture;
    texture.SetId(203);
    TextureMetadata metadata;
    metadata.width = 2;
    metadata.height = 2;
    metadata.mipLevels = 2;
    metadata.arrayLayers = 6;
    metadata.format = TextureFormat::RGBA8;
    metadata.isCubemap = true;
    texture.SetData(std::vector<uint8>(120, 11), metadata);

    const RenderUploadRequestBuildResult build =
        RenderUploadRequestBuilder::Build(texture, {9, 1}, 4, {});
    ASSERT_EQ(build.code, RenderUploadRequestBuildCode::Built);
    const auto& payload =
        std::get<TextureUploadPayload>(build.request->GetPayload());
    ASSERT_EQ(payload.subresources.size(), 12U);
    EXPECT_EQ(payload.subresources[0].mipLevel, 0U);
    EXPECT_EQ(payload.subresources[0].arrayLayer, 0U);
    EXPECT_EQ(payload.subresources[5].bytes.offset, 80U);
    EXPECT_EQ(payload.subresources[6].mipLevel, 1U);
    EXPECT_EQ(payload.subresources[6].arrayLayer, 0U);
    EXPECT_EQ(payload.subresources[6].bytes.offset, 96U);
    EXPECT_EQ(payload.subresources[11].bytes.offset, 116U);
    EXPECT_EQ(payload.subresources[11].rowPitch, 4U);
    EXPECT_EQ(payload.subresources[11].slicePitch, 4U);
}

TEST(ResourceRuntimePolicyValidation, ResourceLifecycleEventsDrainOnlyFromUpdatePump)
{
    const fs::path root = MakeTempDirectory("LifecyclePump");
    WriteTextFile(root / "textures" / "event.png", "placeholder");
    ResourceManagerTestGuard guard(MakeRenderResourceTestConfig(root));
    auto& manager = ResourceManager::Get();
    manager.RegisterLoader(ResourceType::Texture,
                           std::make_unique<RenderTextureLoader>());

    const std::thread::id pumpThread = std::this_thread::get_id();
    std::vector<ResourceLifecycleEventType> events;
    manager.SetLifecycleEventCallback(
        [&](const ResourceLifecycleEvent& event)
        {
            EXPECT_EQ(std::this_thread::get_id(), pumpThread);
            events.push_back(event.type);
        });

    ResourceHandle<TextureResource> handle =
        manager.Load<TextureResource>("source://textures/event.png");
    ASSERT_TRUE(handle.IsValid());
    EXPECT_TRUE(events.empty());
    manager.ProcessCompletedLoads();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events.front(), ResourceLifecycleEventType::Ready);

    manager.Unload(handle.GetId());
    ASSERT_EQ(events.size(), 1U);
    manager.ProcessCompletedLoads();
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events.back(), ResourceLifecycleEventType::BeforeUnload);

    manager.SetLifecycleEventCallback({});
    std::error_code removeError;
    fs::remove_all(root, removeError);
}

TEST(ResourceRuntimePolicyValidation,
     ResourceSubsystemPublishesRuntimeCreatedTextureThroughGateway)
{
    const fs::path root = MakeTempDirectory("RuntimeCreatedPublication");
    FakeResourceGateway gateway;
    ResourceSubsystem subsystem;
    subsystem.SetRenderResourceGateway(&gateway);
    subsystem.Initialize(MakeRenderResourceTestConfig(root));

    auto* texture = new TextureResource();
    texture->SetId(301);
    TextureMetadata metadata;
    metadata.width = 1;
    metadata.height = 1;
    metadata.format = TextureFormat::RGBA8;
    texture->SetData(std::vector<uint8>{10, 20, 30, 255}, metadata);
    ResourceHandle<TextureResource> textureHandle(texture);

    EXPECT_TRUE(subsystem.PublishRenderResource(textureHandle));
    subsystem.Tick(0.0f);
    EXPECT_EQ(gateway.reserveAttempts, 1U);
    EXPECT_EQ(gateway.enqueueAttempts, 1U);
    EXPECT_EQ(gateway.acceptedCount, 1U);

    const RenderResourceResolveResult resolved =
        subsystem.ResolveRenderResource(AssetId{textureHandle.GetId()},
                                        RenderResourceKind::Texture);
    ASSERT_EQ(resolved.code, RenderResourceResolveCode::Resolved);
    gateway.PublishTerminal(resolved.handle,
                            RenderResourcePublicState::GPUReady);
    subsystem.DrainTerminalRenderRequests();
    subsystem.BeginRenderShutdown();
    subsystem.Deinitialize();

    std::error_code removeError;
    fs::remove_all(root, removeError);
}

TEST(ResourceRuntimePolicyValidation,
     ResourceSubsystemPublishesCompositeModelDependenciesRecursively)
{
    const fs::path root = MakeTempDirectory("CompositePublication");
    FakeResourceGateway gateway;
    ResourceSubsystem subsystem;
    subsystem.SetRenderResourceGateway(&gateway);
    subsystem.Initialize(MakeRenderResourceTestConfig(root));

    auto* texture = new TextureResource();
    texture->SetId(311);
    TextureMetadata textureMetadata;
    textureMetadata.width = 1;
    textureMetadata.height = 1;
    textureMetadata.format = TextureFormat::RGBA8;
    texture->SetData(std::vector<uint8>{255, 255, 255, 255},
                     textureMetadata);
    ResourceHandle<TextureResource> textureHandle(texture);

    auto* material = new MaterialResource();
    material->SetId(312);
    material->SetMaterialData(
        std::make_shared<Material>("CompositeMaterial"));
    material->SetTexture("albedo", textureHandle);
    ResourceHandle<MaterialResource> materialHandle(material);

    auto* mesh = new MeshResource();
    mesh->SetId(313);
    auto meshData = std::make_shared<Mesh>();
    meshData->SetPositions({{-1.0f, 0.0f, 0.0f},
                            {1.0f, 0.0f, 0.0f},
                            {0.0f, 1.0f, 0.0f}});
    meshData->SetNormals(std::vector<Vec3>(3, Vec3{0.0f, 0.0f, 1.0f}));
    meshData->SetUVs({{0.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 1.0f}});
    meshData->SetIndices(std::vector<uint16>{0, 1, 2});
    meshData->SetBoundingBox({-1.0f, 0.0f, 0.0f},
                             {1.0f, 1.0f, 0.0f});
    mesh->SetMesh(meshData);
    ResourceHandle<MeshResource> meshHandle(mesh);

    auto* model = new ModelResource();
    model->SetId(314);
    model->AddMesh(meshHandle);
    model->AddMaterial(materialHandle);
    ResourceHandle<ModelResource> modelHandle(model);

    ResourceManager& manager = subsystem.GetManager();
    manager.GetCache().Store(texture);
    manager.GetCache().Store(material);
    manager.GetCache().Store(mesh);

    EXPECT_TRUE(subsystem.PublishRenderResource(modelHandle));
    subsystem.Tick(0.0f);
    EXPECT_EQ(gateway.reserveAttempts, 3U);
    EXPECT_EQ(gateway.enqueueAttempts, 2U);
    EXPECT_EQ(gateway.acceptedCount, 2U);

    const RenderResourceResolveResult textureResolved =
        subsystem.ResolveRenderResource(AssetId{textureHandle.GetId()},
                                        RenderResourceKind::Texture);
    const RenderResourceResolveResult materialResolved =
        subsystem.ResolveRenderResource(AssetId{materialHandle.GetId()},
                                        RenderResourceKind::Material);
    const RenderResourceResolveResult meshResolved =
        subsystem.ResolveRenderResource(AssetId{meshHandle.GetId()},
                                        RenderResourceKind::Mesh);
    ASSERT_EQ(textureResolved.code, RenderResourceResolveCode::Resolved);
    ASSERT_EQ(materialResolved.code, RenderResourceResolveCode::Resolved);
    ASSERT_EQ(meshResolved.code, RenderResourceResolveCode::Resolved);

    EXPECT_TRUE(gateway.GetAcceptedRequest(materialResolved.handle).expired());
    gateway.PublishTerminal(textureResolved.handle,
                            RenderResourcePublicState::GPUReady);
    subsystem.Tick(0.0f);
    EXPECT_EQ(gateway.enqueueAttempts, 3U);
    EXPECT_EQ(gateway.acceptedCount, 3U);

    const auto materialRequest =
        gateway.GetAcceptedRequest(materialResolved.handle).lock();
    ASSERT_NE(materialRequest, nullptr);
    EXPECT_EQ(materialRequest->GetDependencies(),
              std::vector<RenderResourceHandle>{textureResolved.handle});

    gateway.PublishTerminal(materialResolved.handle,
                            RenderResourcePublicState::GPUReady);
    gateway.PublishTerminal(meshResolved.handle,
                            RenderResourcePublicState::GPUReady);
    subsystem.DrainTerminalRenderRequests();
    subsystem.BeginRenderShutdown();
    subsystem.Deinitialize();

    std::error_code removeError;
    fs::remove_all(root, removeError);
}

TEST(ResourceRuntimePolicyValidation, ResourceSubsystemRetriesPressureAndEnqueuesGenerationOnce)
{
    const fs::path root = MakeTempDirectory("UploadRetry");
    WriteTextFile(root / "textures" / "retry.png", "placeholder");

    FakeResourceGateway gateway;
    gateway.pressureCount = 1;
    gateway.pressureCode = RenderUploadEnqueueCode::QueueFullByCount;
    ResourceSubsystem subsystem;
    subsystem.SetRenderResourceGateway(&gateway);
    subsystem.Initialize(MakeRenderResourceTestConfig(root));
    subsystem.RegisterLoader(ResourceType::Texture,
                             std::make_unique<RenderTextureLoader>());

    ResourceHandle<TextureResource> resource =
        subsystem.Load<TextureResource>("source://textures/retry.png");
    ASSERT_TRUE(resource.IsValid());
    subsystem.Tick(0.0f);
    EXPECT_EQ(gateway.enqueueAttempts, 1U);
    EXPECT_EQ(gateway.acceptedCount, 0U);
    EXPECT_EQ(subsystem.GetRenderResourceStats().pendingUploadCount, 1U);

    gateway.pressureCount = 1;
    gateway.pressureCode = RenderUploadEnqueueCode::QueueFullByBytes;
    subsystem.Tick(0.0f);
    EXPECT_EQ(gateway.enqueueAttempts, 2U);
    EXPECT_EQ(gateway.acceptedCount, 0U);
    EXPECT_EQ(subsystem.GetRenderResourceStats().pendingUploadCount, 1U);

    subsystem.Tick(0.0f);
    EXPECT_EQ(gateway.enqueueAttempts, 3U);
    EXPECT_EQ(gateway.acceptedCount, 1U);
    const RenderResourceResolveResult resolved = subsystem.ResolveRenderResource(
        AssetId{resource.GetId()}, RenderResourceKind::Texture);
    ASSERT_EQ(resolved.code, RenderResourceResolveCode::Resolved);
    ASSERT_TRUE(resolved.handle.IsValid());
    std::weak_ptr<const ResourceUploadRequest> weak =
        gateway.GetAcceptedRequest(resolved.handle);
    ASSERT_FALSE(weak.expired());

    subsystem.Tick(0.0f);
    EXPECT_EQ(gateway.enqueueAttempts, 3U);
    gateway.PublishTerminal(resolved.handle,
                            RenderResourcePublicState::GPUReady);
    EXPECT_FALSE(weak.expired());
    subsystem.DrainTerminalRenderRequests();
    EXPECT_TRUE(weak.expired());

    subsystem.BeginRenderShutdown();
    subsystem.Deinitialize();
    std::error_code removeError;
    fs::remove_all(root, removeError);
}

TEST(ResourceRuntimePolicyValidation, AsyncWorkersNeverPublishRenderGatewayOperations)
{
    JobSystem::Get().Shutdown();
    const fs::path root = MakeTempDirectory("WorkerMarshal");
    WriteTextFile(root / "textures" / "worker.png", "placeholder");

    ResourceManagerConfig config = MakeRenderResourceTestConfig(root);
    config.asyncThreadCount = 1;
    FakeResourceGateway gateway;
    ResourceSubsystem subsystem;
    subsystem.SetRenderResourceGateway(&gateway);
    subsystem.Initialize(config);
    subsystem.RegisterLoader(ResourceType::Texture,
                             std::make_unique<RenderTextureLoader>());

    std::future<ResourceHandle<TextureResource>> future =
        subsystem.LoadAsync<TextureResource>("source://textures/worker.png");
    ASSERT_EQ(future.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    ResourceHandle<TextureResource> resource = future.get();
    ASSERT_TRUE(resource.IsValid());
    EXPECT_EQ(gateway.reserveAttempts, 0U);
    EXPECT_EQ(gateway.enqueueAttempts, 0U);

    subsystem.Tick(0.0f);
    EXPECT_EQ(gateway.reserveAttempts, 1U);
    EXPECT_EQ(gateway.enqueueAttempts, 1U);
    const RenderResourceResolveResult resolved = subsystem.ResolveRenderResource(
        AssetId{resource.GetId()}, RenderResourceKind::Texture);
    ASSERT_EQ(resolved.code, RenderResourceResolveCode::Resolved);
    gateway.PublishTerminal(resolved.handle,
                            RenderResourcePublicState::GPUReady);
    subsystem.DrainTerminalRenderRequests();
    subsystem.BeginRenderShutdown();
    subsystem.Deinitialize();
    JobSystem::Get().Shutdown();

    std::error_code removeError;
    fs::remove_all(root, removeError);
}

TEST(ResourceRuntimePolicyValidation, ResourceSubsystemRejectsWorkerGatewayMutation)
{
    const fs::path root = MakeTempDirectory("GatewayThreadIdentity");
    FakeResourceGateway updateGateway;
    FakeResourceGateway workerGateway;
    ResourceSubsystem subsystem;
    subsystem.Initialize(MakeRenderResourceTestConfig(root));
    subsystem.SetRenderResourceGateway(&updateGateway);

    std::thread worker(
        [&]
        {
            subsystem.SetRenderResourceGateway(&workerGateway);
        });
    worker.join();

    EXPECT_EQ(subsystem.GetRenderResourceStats().wrongThreadMutations, 1U);
    WriteTextFile(root / "textures" / "owner.png", "placeholder");
    subsystem.RegisterLoader(ResourceType::Texture,
                             std::make_unique<RenderTextureLoader>());
    ResourceHandle<TextureResource> resource =
        subsystem.Load<TextureResource>("source://textures/owner.png");
    ASSERT_TRUE(resource.IsValid());
    subsystem.Tick(0.0f);
    EXPECT_EQ(updateGateway.reserveAttempts, 1U);
    EXPECT_EQ(workerGateway.reserveAttempts, 0U);

    const RenderResourceResolveResult resolved = subsystem.ResolveRenderResource(
        AssetId{resource.GetId()}, RenderResourceKind::Texture);
    ASSERT_EQ(resolved.code, RenderResourceResolveCode::Resolved);
    updateGateway.PublishTerminal(resolved.handle,
                                  RenderResourcePublicState::GPUReady);
    subsystem.DrainTerminalRenderRequests();
    subsystem.BeginRenderShutdown();
    subsystem.Deinitialize();

    std::error_code removeError;
    fs::remove_all(root, removeError);
}

TEST(ResourceRuntimePolicyValidation, ResourceSubsystemReclaimsEveryTerminalRequestOnDrain)
{
    const fs::path root = MakeTempDirectory("TerminalReclaim");
    FakeResourceGateway gateway;
    ResourceSubsystem subsystem;
    subsystem.SetRenderResourceGateway(&gateway);
    subsystem.Initialize(MakeRenderResourceTestConfig(root));
    subsystem.RegisterLoader(ResourceType::Texture,
                             std::make_unique<RenderTextureLoader>());

    const RenderResourcePublicState terminalStates[] = {
        RenderResourcePublicState::GPUReady,
        RenderResourcePublicState::Failed,
        RenderResourcePublicState::Released,
        RenderResourcePublicState::Released};
    std::vector<ResourceHandle<TextureResource>> resources;
    for (uint32 index = 0; index < 4; ++index)
    {
        const std::string name = "terminal-" + std::to_string(index) + ".png";
        WriteTextFile(root / "textures" / name, "placeholder");
        ResourceHandle<TextureResource> resource =
            subsystem.Load<TextureResource>("source://textures/" + name);
        ASSERT_TRUE(resource.IsValid());
        subsystem.Tick(0.0f);
        const RenderResourceResolveResult resolved =
            subsystem.ResolveRenderResource(AssetId{resource.GetId()},
                                            RenderResourceKind::Texture);
        ASSERT_EQ(resolved.code, RenderResourceResolveCode::Resolved);
        std::weak_ptr<const ResourceUploadRequest> weak =
            gateway.GetAcceptedRequest(resolved.handle);
        ASSERT_FALSE(weak.expired());

        if (index >= 2)
        {
            subsystem.Unload(resource.GetId());
            subsystem.Tick(0.0f);
        }
        gateway.PublishTerminal(
            resolved.handle,
            terminalStates[index],
            index == 1 ? RenderResourceFailureCode::ResourceCreationFailed
                       : RenderResourceFailureCode::None);
        EXPECT_FALSE(weak.expired());
        subsystem.DrainTerminalRenderRequests();
        EXPECT_TRUE(weak.expired());
        resources.push_back(std::move(resource));
    }

    EXPECT_EQ(subsystem.GetRenderResourceStats().terminalRequestsReclaimed,
              4U);
    subsystem.BeginRenderShutdown();
    subsystem.Deinitialize();

    std::error_code removeError;
    fs::remove_all(root, removeError);
}

TEST(ResourceRuntimePolicyValidation, ResourceSubsystemReplacesAfterUnloadAndSealsShutdown)
{
    const fs::path root = MakeTempDirectory("UploadReplacement");
    WriteTextFile(root / "textures" / "replace.png", "placeholder");
    WriteTextFile(root / "textures" / "sealed.png", "placeholder");

    FakeResourceGateway gateway;
    ResourceSubsystem subsystem;
    subsystem.SetRenderResourceGateway(&gateway);
    subsystem.Initialize(MakeRenderResourceTestConfig(root));
    subsystem.RegisterLoader(ResourceType::Texture,
                             std::make_unique<RenderTextureLoader>());

    ResourceHandle<TextureResource> first =
        subsystem.Load<TextureResource>("source://textures/replace.png");
    subsystem.Tick(0.0f);
    const RenderResourceResolveResult firstResolved =
        subsystem.ResolveRenderResource(AssetId{first.GetId()},
                                        RenderResourceKind::Texture);
    ASSERT_EQ(firstResolved.code, RenderResourceResolveCode::Resolved);

    subsystem.Unload(first.GetId());
    subsystem.Tick(0.0f);
    EXPECT_EQ(gateway.releaseAttempts, 1U);
    EXPECT_EQ(subsystem.ResolveRenderResource(AssetId{first.GetId()},
                                              RenderResourceKind::Texture)
                  .code,
              RenderResourceResolveCode::NotFound);

    ResourceHandle<TextureResource> replacement =
        subsystem.Load<TextureResource>("source://textures/replace.png");
    subsystem.Tick(0.0f);
    const RenderResourceResolveResult secondResolved =
        subsystem.ResolveRenderResource(AssetId{replacement.GetId()},
                                        RenderResourceKind::Texture);
    ASSERT_EQ(secondResolved.code, RenderResourceResolveCode::Resolved);
    EXPECT_NE(secondResolved.handle, firstResolved.handle);

    gateway.PublishTerminal(firstResolved.handle,
                            RenderResourcePublicState::Released);
    subsystem.DrainTerminalRenderRequests();
    const uint32 releasesBeforeSeal = gateway.releaseAttempts;
    subsystem.BeginRenderShutdown();
    EXPECT_EQ(gateway.releaseAttempts, releasesBeforeSeal);
    EXPECT_EQ(subsystem.ResolveRenderResource(
                  AssetId{replacement.GetId()},
                  RenderResourceKind::Texture)
                  .code,
              RenderResourceResolveCode::Resolved);
    const uint32 reservesBeforeSeal = gateway.reserveAttempts;
    ResourceHandle<TextureResource> sealed =
        subsystem.Load<TextureResource>("source://textures/sealed.png");
    ASSERT_TRUE(sealed.IsValid());
    subsystem.Tick(0.0f);
    EXPECT_EQ(gateway.reserveAttempts, reservesBeforeSeal);

    gateway.PublishTerminal(secondResolved.handle,
                            RenderResourcePublicState::Released);
    subsystem.DrainTerminalRenderRequests();
    subsystem.Deinitialize();
    std::error_code removeError;
    fs::remove_all(root, removeError);
}
