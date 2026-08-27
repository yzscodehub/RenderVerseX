/** @file EngineDevelopmentComposition.cpp @brief Development shader provider */

#include "EngineDevelopmentComposition/EngineDevelopmentComposition.h"

#include "Core/Assert.h"
#include "Resource/ResourceManager.h"
#include "ResourceSource/ResourceSourceComposition.h"
#include "ShaderCompiler/ShaderManager.h"

#include <array>
#include <filesystem>
#include <utility>

namespace RVX
{
namespace
{
    [[nodiscard]] const char* ResolveTargetProfile(
        RHIBackendType backend,
        RHIShaderStage stage) noexcept
    {
        const bool legacy = backend == RHIBackendType::DX11;
        switch (stage)
        {
            case RHIShaderStage::Vertex: return legacy ? "vs_5_0" : "vs_6_0";
            case RHIShaderStage::Pixel: return legacy ? "ps_5_0" : "ps_6_0";
            case RHIShaderStage::Compute: return legacy ? "cs_5_0" : "cs_6_0";
            case RHIShaderStage::Geometry: return legacy ? "gs_5_0" : "gs_6_0";
            case RHIShaderStage::Hull: return legacy ? "hs_5_0" : "hs_6_0";
            case RHIShaderStage::Domain: return legacy ? "ds_5_0" : "ds_6_0";
            case RHIShaderStage::RayGeneration:
            case RHIShaderStage::Miss:
            case RHIShaderStage::ClosestHit:
            case RHIShaderStage::AnyHit:
            case RHIShaderStage::Intersection:
                return legacy ? nullptr : "lib_6_3";
            default:
                return nullptr;
        }
    }

    [[nodiscard]] std::filesystem::path ResolveShaderPath(
        const std::filesystem::path& configuredRoot,
        const std::string& logicalAssetId)
    {
        const std::filesystem::path requested(logicalAssetId);
        if (requested.is_absolute())
        {
            return requested;
        }

        if (!configuredRoot.empty())
        {
            return configuredRoot / requested;
        }

        const std::array<std::filesystem::path, 7> roots = {
            std::filesystem::path("Source") / "Runtime" / "Render" / "Shaders",
            std::filesystem::path("..") / "Source" / "Runtime" / "Render" / "Shaders",
            std::filesystem::path("../..") / "Source" / "Runtime" / "Render" / "Shaders",
            std::filesystem::path("../../..") / "Source" / "Runtime" / "Render" / "Shaders",
            std::filesystem::path("../../../..") / "Source" / "Runtime" / "Render" / "Shaders",
            std::filesystem::path("../../../../..") / "Source" / "Runtime" / "Render" / "Shaders",
            std::filesystem::current_path(),
        };
        for (const std::filesystem::path& root : roots)
        {
            const std::filesystem::path candidate = root / requested;
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error))
            {
                return candidate;
            }
        }
        return roots.front() / requested;
    }

    [[nodiscard]] uint64 HashProgramContent(
        const ShaderCompileResult& result) noexcept
    {
        uint64 hash = result.sourceInfo.combinedHash;
        hash ^= result.permutationHash + 0x9e3779b97f4a7c15ull +
            (hash << 6U) + (hash >> 2U);
        if (hash != 0)
        {
            return hash;
        }

        hash = 0xcbf29ce484222325ull;
        constexpr uint64 prime = 0x100000001b3ull;
        for (const uint8 byte : result.bytecode)
        {
            hash ^= byte;
            hash *= prime;
        }
        for (const char byte : result.glslSource)
        {
            hash ^= static_cast<uint8>(byte);
            hash *= prime;
        }
        for (const char byte : result.mslSource)
        {
            hash ^= static_cast<uint8>(byte);
            hash *= prime;
        }
        return hash;
    }

    class DevelopmentShaderProgramProvider final
        : public IShaderProgramProvider
    {
    public:
        explicit DevelopmentShaderProgramProvider(
            std::filesystem::path shaderSourceRoot)
            : m_shaderSourceRoot(std::move(shaderSourceRoot)),
              m_manager(MakeManagerConfig())
        {
        }

        ShaderProgramResult Load(
            IRHIDevice& device,
            const ShaderProgramRequest& request) override
        {
            ShaderProgramResult result;
            if (request.logicalAssetId.empty() || request.entryPoint.empty() ||
                request.stage == RHIShaderStage::None ||
                request.backend == RHIBackendType::None ||
                request.backend == RHIBackendType::Auto)
            {
                result.status = ShaderProgramStatus::InvalidRequest;
                result.diagnostic = "Shader program request is incomplete";
                return result;
            }

            const char* targetProfile =
                ResolveTargetProfile(request.backend, request.stage);
            if (targetProfile == nullptr)
            {
                result.status = ShaderProgramStatus::Unsupported;
                result.diagnostic = "Shader backend/stage combination is unsupported";
                return result;
            }

            const std::filesystem::path shaderPath = ResolveShaderPath(
                m_shaderSourceRoot, request.logicalAssetId);
            std::error_code pathError;
            if (!std::filesystem::is_regular_file(shaderPath, pathError))
            {
                result.status = ShaderProgramStatus::NotFound;
                result.diagnostic =
                    "Shader source was not found: " + shaderPath.string();
                return result;
            }

            ShaderLoadDesc load;
            load.path = shaderPath.string();
            load.entryPoint = request.entryPoint;
            load.stage = request.stage;
            load.backend = request.backend;
            load.targetProfile = targetProfile;
            load.defines = request.defines;
            load.enableDebugInfo = false;
            load.enableOptimization = true;

            ShaderLoadResult loaded = m_manager.LoadFromFile(&device, load);
            if (!loaded.compileResult.success)
            {
                result.status = ShaderProgramStatus::CompileFailed;
                result.diagnostic = loaded.compileResult.errorMessage;
                return result;
            }
            if (!loaded.shader)
            {
                result.status = ShaderProgramStatus::DeviceCreationFailed;
                result.diagnostic = "RHI rejected the compiled shader program";
                return result;
            }

            result.shader = std::move(loaded.shader);
            result.reflection = std::move(loaded.compileResult.reflection);
            result.contentHash = HashProgramContent(loaded.compileResult);
            result.status = ShaderProgramStatus::Success;
            return result;
        }

    private:
        [[nodiscard]] static ShaderManagerConfig MakeManagerConfig()
        {
            ShaderManagerConfig config;
            config.enableMemoryCache = true;
            config.enableDiskCache = false;
            config.enableAsyncCompile = false;
            config.enableHotReload = false;
            return config;
        }

        std::filesystem::path m_shaderSourceRoot;
        ShaderManager m_manager;
    };
} // namespace

ShaderProgramProviderFactory CreateDevelopmentShaderProgramProviderFactory(
    std::filesystem::path shaderSourceRoot)
{
    return [root = std::move(shaderSourceRoot)]()
    {
        return std::make_unique<DevelopmentShaderProgramProvider>(root);
    };
}

ResourceManagerDependencies CreateDevelopmentResourceManagerDependencies()
{
    ResourceManagerDependencies dependencies =
        CreateEngineResourceManagerDependencies();
    std::string error;
    const bool added = ResourceSource::AddSourceResourceDependencies(
        dependencies, error);
    RVX_ASSERT_MSG(
        added,
        "Development resource dependency composition failed: {}",
        error);
    return dependencies;
}

EngineRuntimeDependencies CreateDevelopmentEngineRuntimeDependencies(
    std::filesystem::path shaderSourceRoot)
{
    EngineRuntimeDependencies dependencies;
    dependencies.resourceManager.emplace(
        CreateDevelopmentResourceManagerDependencies());
    dependencies.shaderPrograms =
        CreateDevelopmentShaderProgramProviderFactory(
            std::move(shaderSourceRoot));
    return dependencies;
}
} // namespace RVX
