#include "Core/Log.h"
#include "Resource/ResourceManager.h"
#include "Resource/RuntimeResourcePolicy.h"
#include "Resource/Types/ShaderResource.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace RVX;
using namespace RVX::Resource;

namespace
{
    namespace fs = std::filesystem;

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
    EXPECT_EQ(loaderPtr->lastPath, cookedPath.string());

    const ResourceLoadDiagnostic diagnostic = manager.GetLastLoadDiagnostic();
    EXPECT_TRUE(diagnostic.success);
    EXPECT_EQ(diagnostic.domain, ResourceLoadDomain::CookedArtifact);
    EXPECT_EQ(diagnostic.failure, ResourceLoadFailureCode::None);
    EXPECT_FALSE(diagnostic.sourceAssetRead);
    EXPECT_TRUE(diagnostic.cookedArtifactRead);
    EXPECT_FALSE(diagnostic.runtimePackageRead);
    EXPECT_EQ(diagnostic.resolvedPath, cookedPath.string());

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
    EXPECT_EQ(loaderPtr->lastPath, packageEntryPath.string());

    const ResourceLoadDiagnostic diagnostic = manager.GetLastLoadDiagnostic();
    EXPECT_TRUE(diagnostic.success);
    EXPECT_EQ(diagnostic.domain, ResourceLoadDomain::RuntimePackage);
    EXPECT_EQ(diagnostic.failure, ResourceLoadFailureCode::None);
    EXPECT_FALSE(diagnostic.sourceAssetRead);
    EXPECT_FALSE(diagnostic.cookedArtifactRead);
    EXPECT_TRUE(diagnostic.runtimePackageRead);
    EXPECT_EQ(diagnostic.resolvedPath, packageEntryPath.string());

    std::error_code removeError;
    fs::remove_all(root, removeError);
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
    const ResourceRuntimePolicy editorPolicy =
        MakeResourceRuntimePolicyForAppMode(AppMode::Editor);
    EXPECT_EQ(editorPolicy.mode, ResourceRuntimeMode::Editor);
    EXPECT_TRUE(editorPolicy.allowSourceAssetReads);
    EXPECT_FALSE(editorPolicy.requireCookedArtifacts);
    EXPECT_FALSE(editorPolicy.requireRuntimePackage);

    const ResourceRuntimePolicy previewPolicy =
        MakeResourceRuntimePolicyForAppMode(AppMode::Preview);
    EXPECT_EQ(previewPolicy.mode, ResourceRuntimeMode::Editor);
    EXPECT_TRUE(previewPolicy.allowSourceAssetReads);
    EXPECT_FALSE(previewPolicy.requireCookedArtifacts);
    EXPECT_FALSE(previewPolicy.requireRuntimePackage);

    const ResourceRuntimePolicy runtimePolicy =
        MakeResourceRuntimePolicyForAppMode(AppMode::Runtime);
    EXPECT_EQ(runtimePolicy.mode, ResourceRuntimeMode::CookedRuntime);
    EXPECT_FALSE(runtimePolicy.allowSourceAssetReads);
    EXPECT_TRUE(runtimePolicy.requireCookedArtifacts);
    EXPECT_FALSE(runtimePolicy.requireRuntimePackage);

    const ResourceRuntimePolicy piePolicy =
        MakeResourceRuntimePolicyForAppMode(AppMode::PlayInEditor);
    EXPECT_EQ(piePolicy.mode, ResourceRuntimeMode::CookedRuntime);
    EXPECT_FALSE(piePolicy.allowSourceAssetReads);
    EXPECT_TRUE(piePolicy.requireCookedArtifacts);
    EXPECT_FALSE(piePolicy.requireRuntimePackage);

    const ResourcePathResolution runtimeSource =
        ResolveRuntimeResourcePath(runtimePolicy, "", "source://textures/albedo.png");
    EXPECT_FALSE(runtimeSource.allowed);
    EXPECT_EQ(runtimeSource.failure, ResourceLoadFailureCode::CookedArtifactRequired);

    const ResourcePathResolution editorSource =
        ResolveRuntimeResourcePath(editorPolicy, "", "source://textures/albedo.png");
    EXPECT_TRUE(editorSource.allowed);
    EXPECT_EQ(editorSource.domain, ResourceLoadDomain::SourceAsset);
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
        EXPECT_EQ(loaderPtr->lastPath, sourcePath.string());

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
