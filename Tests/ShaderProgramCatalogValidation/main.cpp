#include "Common/DeterministicShaderCompiler.h"
#include "Core/Log.h"
#include "ShaderContracts/ShaderArtifact.h"
#include "Tools/ShaderProgramCatalog.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    class TemporaryDirectory
    {
    public:
        explicit TemporaryDirectory(const char* name)
        {
            const auto stamp = std::chrono::steady_clock::now()
                .time_since_epoch().count();
            m_path = fs::temp_directory_path() /
                (std::string(name) + "_" + std::to_string(stamp));
            std::error_code error;
            fs::create_directories(m_path, error);
            EXPECT_FALSE(error) << error.message();
        }

        ~TemporaryDirectory()
        {
            std::error_code error;
            fs::remove_all(m_path, error);
        }

        const fs::path& Path() const noexcept { return m_path; }

    private:
        fs::path m_path;
    };

    class ScopedLog
    {
    public:
        ScopedLog() { RVX::Log::Initialize(); }
        ~ScopedLog() { RVX::Log::Shutdown(); }
    };

    bool WriteText(const fs::path& path, std::string_view text)
    {
        std::error_code error;
        fs::create_directories(path.parent_path(), error);
        if (error)
        {
            return false;
        }
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        return file.good();
    }

    std::string ReadText(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file),
                std::istreambuf_iterator<char>()};
    }

    bool ContainsIdentifierCall(
        std::string_view source, std::string_view identifier)
    {
        size_t offset = 0;
        while ((offset = source.find(identifier, offset)) !=
               std::string_view::npos)
        {
            const bool leftBoundary = offset == 0 ||
                (!std::isalnum(static_cast<unsigned char>(source[offset - 1])) &&
                 source[offset - 1] != '_');
            size_t next = offset + identifier.size();
            while (next < source.size() &&
                   std::isspace(static_cast<unsigned char>(source[next])))
            {
                ++next;
            }
            if (leftBoundary && next < source.size() && source[next] == '(')
            {
                return true;
            }
            offset += identifier.size();
        }
        return false;
    }

    fs::path RepositoryCatalogPath()
    {
        return fs::path(RVX_SOURCE_DIR) /
            "Source/Runtime/Render/Shaders/Programs.rvxshadercatalog.json";
    }

    fs::path RepositoryShaderRoot()
    {
        return fs::path(RVX_SOURCE_DIR) / "Source/Runtime/Render/Shaders";
    }
} // namespace

TEST(ShaderProgramCatalogValidation,
     RepositoryCatalogReferencesExistingProgramsAndUniqueArtifacts)
{
    std::vector<RVX::Tools::ShaderProgramCatalogEntry> entries;
    std::string error;
    ASSERT_TRUE(RVX::Tools::LoadShaderProgramCatalog(
        RepositoryCatalogPath(), entries, error)) << error;
    ASSERT_GE(entries.size(), 80u);

    const std::array backends = {
        RVX::RHIBackendType::DX11,
        RVX::RHIBackendType::DX12,
        RVX::RHIBackendType::Vulkan,
        RVX::RHIBackendType::Metal,
        RVX::RHIBackendType::OpenGL,
    };
    std::map<RVX::RHIBackendType, size_t> backendCounts;
    std::set<std::string> outputIdentities;
    std::map<std::string, std::string> sourceCache;
    for (const RVX::Tools::ShaderProgramCatalogEntry& entry : entries)
    {
        const fs::path sourcePath =
            RepositoryShaderRoot() / entry.logicalAssetId;
        ASSERT_TRUE(fs::is_regular_file(sourcePath)) << sourcePath;
        const std::string key = sourcePath.generic_string();
        const std::string& source = sourceCache.try_emplace(
            key, ReadText(sourcePath)).first->second;
        ASSERT_FALSE(source.empty()) << sourcePath;
        EXPECT_TRUE(ContainsIdentifierCall(source, entry.entryPoint))
            << sourcePath << ": " << entry.entryPoint;

        for (RVX::RHIBackendType backend : backends)
        {
            if (std::find(
                    entry.backends.begin(), entry.backends.end(), backend) ==
                entry.backends.end())
            {
                continue;
            }
            RVX::ShaderProgramRequest request;
            ASSERT_TRUE(RVX::Tools::ResolveShaderProgramCatalogEntry(
                entry, backend, request, error)) << error;
            const std::string identity =
                RVX::BuildShaderArtifactRelativePath(request).generic_string();
            EXPECT_TRUE(outputIdentities.insert(identity).second) << identity;
            ++backendCounts[backend];
        }
    }
    for (RVX::RHIBackendType backend : backends)
    {
        EXPECT_GT(backendCounts[backend], 0u);
    }
}

TEST(ShaderProgramCatalogValidation,
     SymbolicBackendDefinesResolveToExplicitPermutationValues)
{
    RVX::Tools::ShaderProgramCatalogEntry entry;
    entry.logicalAssetId = "PostProcess/Test.hlsl";
    entry.stage = RVX::RHIShaderStage::Pixel;
    entry.entryPoint = "PSMain";
    entry.backends = {
        RVX::RHIBackendType::DX12,
        RVX::RHIBackendType::Vulkan,
    };
    entry.defines = {{"RVX_FULLSCREEN_UV_Y_MATCHES_CLIP_Y", "$backend.clipY"}};

    RVX::ShaderProgramRequest request;
    std::string error;
    ASSERT_TRUE(RVX::Tools::ResolveShaderProgramCatalogEntry(
        entry, RVX::RHIBackendType::DX12, request, error)) << error;
    ASSERT_EQ(request.defines.size(), 1u);
    EXPECT_EQ(request.defines.front().value, "0");

    ASSERT_TRUE(RVX::Tools::ResolveShaderProgramCatalogEntry(
        entry, RVX::RHIBackendType::Vulkan, request, error)) << error;
    ASSERT_EQ(request.defines.size(), 1u);
    EXPECT_EQ(request.defines.front().value, "1");

    entry.defines.front().value = "$backend.unknown";
    EXPECT_FALSE(RVX::Tools::ResolveShaderProgramCatalogEntry(
        entry, RVX::RHIBackendType::DX12, request, error));
    EXPECT_NE(error.find("unknown symbolic define"), std::string::npos);
}

TEST(ShaderProgramCatalogValidation,
     GPUCullingRuntimeProgramsAreCoveredByTheCookCatalog)
{
    std::vector<RVX::Tools::ShaderProgramCatalogEntry> entries;
    std::string error;
    ASSERT_TRUE(RVX::Tools::LoadShaderProgramCatalog(
        RepositoryCatalogPath(), entries, error)) << error;

    std::set<std::string> cookedPrograms;
    for (const RVX::Tools::ShaderProgramCatalogEntry& entry : entries)
    {
        RVX::ShaderProgramRequest request;
        if (RVX::Tools::ResolveShaderProgramCatalogEntry(
                entry, RVX::RHIBackendType::DX12, request, error))
        {
            cookedPrograms.insert(
                RVX::BuildShaderArtifactRelativePath(request).generic_string());
        }
    }

    for (const char* entryPoint : {
             "CSFrustumCull", "CSCompactDraws", "CSFinalizeDrawGroups"})
    {
        RVX::ShaderProgramRequest request;
        request.logicalAssetId = "GPUDriven/GPUCulling.hlsl";
        request.stage = RVX::RHIShaderStage::Compute;
        request.entryPoint = entryPoint;
        request.backend = RVX::RHIBackendType::DX12;
        EXPECT_TRUE(cookedPrograms.contains(
            RVX::BuildShaderArtifactRelativePath(request).generic_string()))
            << entryPoint;
    }
}

TEST(ShaderProgramCatalogValidation,
     ParticleBillboardProgramsAreCookedForEveryRasterBackend)
{
    std::vector<RVX::Tools::ShaderProgramCatalogEntry> entries;
    std::string error;
    ASSERT_TRUE(RVX::Tools::LoadShaderProgramCatalog(
        RepositoryCatalogPath(), entries, error)) << error;

    const fs::path sourcePath =
        RepositoryShaderRoot() / "Particle" / "ParticleBillboard.hlsl";
    ASSERT_TRUE(fs::is_regular_file(sourcePath)) << sourcePath;
    const std::string source = ReadText(sourcePath);
    EXPECT_NE(source.find("../Include/FullscreenTriangle.hlsli"),
              std::string::npos);
    EXPECT_NE(source.find("gAuxiliaryTexture.Load"), std::string::npos)
        << "D32 soft depth must use an exact integer texel fetch";
    EXPECT_EQ(source.find("gAuxiliaryTexture.SampleLevel"),
              std::string::npos)
        << "D32 soft depth must not linearly filter across discontinuities";

    const std::array expectedBackends = {
        RVX::RHIBackendType::DX11,
        RVX::RHIBackendType::DX12,
        RVX::RHIBackendType::Vulkan,
        RVX::RHIBackendType::Metal,
        RVX::RHIBackendType::OpenGL,
    };

    const auto findProgram = [&entries](RVX::RHIShaderStage stage,
                                        std::string_view entryPoint)
        -> const RVX::Tools::ShaderProgramCatalogEntry*
    {
        const auto it = std::find_if(
            entries.begin(), entries.end(),
            [stage, entryPoint](const RVX::Tools::ShaderProgramCatalogEntry& entry)
            {
                return entry.logicalAssetId == "Particle/ParticleBillboard.hlsl" &&
                       entry.stage == stage && entry.entryPoint == entryPoint;
            });
        return it == entries.end() ? nullptr : &*it;
    };

    for (const auto [stage, entryPoint] : {
             std::pair{RVX::RHIShaderStage::Vertex, std::string_view{"VSMain"}},
             std::pair{RVX::RHIShaderStage::Pixel, std::string_view{"PSMain"}},
             std::pair{RVX::RHIShaderStage::Pixel,
                       std::string_view{"PSDistortionMain"}},
             std::pair{RVX::RHIShaderStage::Vertex,
                       std::string_view{"DistortionCompositeVS"}},
             std::pair{RVX::RHIShaderStage::Pixel,
                       std::string_view{"DistortionCompositePS"}}})
    {
        const RVX::Tools::ShaderProgramCatalogEntry* program =
            findProgram(stage, entryPoint);
        ASSERT_NE(program, nullptr) << entryPoint;
        const auto clipYDefine = std::find_if(
            program->defines.begin(), program->defines.end(),
            [](const RVX::ShaderMacro& define)
            {
                return define.name == "RVX_FULLSCREEN_UV_Y_MATCHES_CLIP_Y" &&
                       define.value == "$backend.clipY";
            });
        ASSERT_NE(clipYDefine, program->defines.end())
            << entryPoint
            << " includes the shared fullscreen helper and therefore needs the backend clip-Y permutation";

        for (const RVX::RHIBackendType backend : expectedBackends)
        {
            EXPECT_NE(std::find(program->backends.begin(), program->backends.end(), backend),
                      program->backends.end())
                << entryPoint;

            RVX::ShaderProgramRequest request;
            ASSERT_TRUE(RVX::Tools::ResolveShaderProgramCatalogEntry(
                *program, backend, request, error)) << error;
            EXPECT_EQ(request.logicalAssetId, "Particle/ParticleBillboard.hlsl");
            EXPECT_EQ(request.stage, stage);
            EXPECT_EQ(request.entryPoint, entryPoint);
            EXPECT_EQ(request.backend, backend);
            const auto resolvedClipYDefine = std::find_if(
                request.defines.begin(), request.defines.end(),
                [](const RVX::ShaderMacro& define)
                {
                    return define.name == "RVX_FULLSCREEN_UV_Y_MATCHES_CLIP_Y";
                });
            ASSERT_NE(resolvedClipYDefine, request.defines.end()) << entryPoint;
            EXPECT_TRUE(resolvedClipYDefine->value == "0" ||
                        resolvedClipYDefine->value == "1");
        }
    }
}

TEST(ShaderProgramCatalogValidation,
     CatalogCookWritesReadableContentAddressedV2Artifact)
{
    ScopedLog log;
    TemporaryDirectory temporary("rvx_shader_catalog_cook");
    const fs::path sourceRoot = temporary.Path() / "Source";
    const fs::path outputRoot = temporary.Path() / "Cooked";
    const fs::path catalogPath = temporary.Path() / "Programs.json";
    ASSERT_TRUE(WriteText(
        sourceRoot / "Test.ps.hlsl",
        "float4 PSMain() : SV_Target0 { return 1.0; }\n"));
    ASSERT_TRUE(WriteText(
        catalogPath,
        R"({"schema":"RVX_SHADER_PROGRAM_CATALOG","version":2,"programs":[{"asset":"Test.ps.hlsl","stage":"Pixel","entry":"PSMain","backends":["DX12"],"defines":[{"name":"QUALITY","value":"2"}]}]})"));

    const RVX::Tools::ShaderProgramCatalogCookResult result =
        RVX::Tools::CookShaderProgramCatalog(
            {catalogPath, sourceRoot, outputRoot, RVX::RHIBackendType::DX12},
            &RVX::Tests::CreateDeterministicShaderCompiler);
    ASSERT_TRUE(result.success) << result.error;
    ASSERT_EQ(result.catalogProgramCount, 1u);
    ASSERT_EQ(result.cookedProgramCount, 1u);
    ASSERT_EQ(result.outputPaths.size(), 1u);

    RVX::ShaderArtifact artifact;
    std::string error;
    ASSERT_TRUE(RVX::ReadShaderArtifactV2(
        result.outputPaths.front(), artifact, error)) << error;
    EXPECT_EQ(artifact.request.logicalAssetId, "Test.ps.hlsl");
    EXPECT_EQ(artifact.request.entryPoint, "PSMain");
    EXPECT_EQ(artifact.request.stage, RVX::RHIShaderStage::Pixel);
    EXPECT_EQ(artifact.request.backend, RVX::RHIBackendType::DX12);
    ASSERT_EQ(artifact.request.defines.size(), 1u);
    EXPECT_EQ(artifact.request.defines.front().name, "QUALITY");
    EXPECT_EQ(artifact.request.defines.front().value, "2");
    EXPECT_EQ(artifact.targetProfile, "ps_6_0");
    EXPECT_EQ(artifact.runtimeEntryPoint, "PSMain");
    EXPECT_FALSE(artifact.code.empty());
    EXPECT_TRUE(artifact.reflection.valid);
}

TEST(ShaderProgramCatalogValidation, InvalidCatalogFieldsFailClosed)
{
    TemporaryDirectory temporary("rvx_shader_catalog_invalid");
    const fs::path catalogPath = temporary.Path() / "Programs.json";
    ASSERT_TRUE(WriteText(
        catalogPath,
        R"({"schema":"RVX_SHADER_PROGRAM_CATALOG","version":2,"programs":[{"asset":"Test.hlsl","stage":"Pixel","entry":"PSMain","backends":["DX12"],"unexpected":true}]})"));
    std::vector<RVX::Tools::ShaderProgramCatalogEntry> entries;
    std::string error;
    EXPECT_FALSE(RVX::Tools::LoadShaderProgramCatalog(
        catalogPath, entries, error));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(entries.empty());
}
