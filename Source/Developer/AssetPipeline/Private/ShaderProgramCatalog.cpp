/** @file ShaderProgramCatalog.cpp @brief Strict shader program catalog cooker */

#include "Tools/ShaderProgramCatalog.h"

#include "ShaderContracts/ShaderArtifact.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>
#include <set>
#include <system_error>
#include <utility>

namespace RVX::Tools
{
namespace
{
    using Json = nlohmann::json;

    bool IsConcreteBackend(RHIBackendType backend) noexcept
    {
        return backend >= RHIBackendType::DX11 &&
            backend <= RHIBackendType::OpenGL;
    }

    bool ParseBackend(std::string_view value, RHIBackendType& outBackend)
    {
        if (value == "DX11") outBackend = RHIBackendType::DX11;
        else if (value == "DX12") outBackend = RHIBackendType::DX12;
        else if (value == "Vulkan") outBackend = RHIBackendType::Vulkan;
        else if (value == "Metal") outBackend = RHIBackendType::Metal;
        else if (value == "OpenGL") outBackend = RHIBackendType::OpenGL;
        else return false;
        return true;
    }

    bool ParseStage(std::string_view value, RHIShaderStage& outStage)
    {
        if (value == "Vertex") outStage = RHIShaderStage::Vertex;
        else if (value == "Hull") outStage = RHIShaderStage::Hull;
        else if (value == "Domain") outStage = RHIShaderStage::Domain;
        else if (value == "Geometry") outStage = RHIShaderStage::Geometry;
        else if (value == "Pixel") outStage = RHIShaderStage::Pixel;
        else if (value == "Compute") outStage = RHIShaderStage::Compute;
        else if (value == "Mesh") outStage = RHIShaderStage::Mesh;
        else if (value == "Amplification") outStage = RHIShaderStage::Amplification;
        else if (value == "RayGeneration") outStage = RHIShaderStage::RayGeneration;
        else if (value == "AnyHit") outStage = RHIShaderStage::AnyHit;
        else if (value == "ClosestHit") outStage = RHIShaderStage::ClosestHit;
        else if (value == "Miss") outStage = RHIShaderStage::Miss;
        else if (value == "Intersection") outStage = RHIShaderStage::Intersection;
        else if (value == "Callable") outStage = RHIShaderStage::Callable;
        else return false;
        return true;
    }

    bool IsCanonicalLogicalAssetId(std::string_view value)
    {
        if (value.empty() || value.find('\\') != std::string_view::npos)
        {
            return false;
        }
        const std::filesystem::path path{std::string(value)};
        if (path.is_absolute() || path.has_root_path() ||
            path.generic_string() != value)
        {
            return false;
        }
        for (const std::filesystem::path& component : path)
        {
            if (component.empty() || component == "." || component == "..")
            {
                return false;
            }
        }
        return true;
    }

    bool HasOnlyKeys(
        const Json& object,
        std::initializer_list<const char*> allowed)
    {
        std::set<std::string> names;
        for (const char* name : allowed)
        {
            names.insert(name);
        }
        for (auto entry = object.cbegin(); entry != object.cend(); ++entry)
        {
            if (!names.contains(entry.key()))
            {
                return false;
            }
        }
        return true;
    }

    bool IsPathContained(
        const std::filesystem::path& root,
        const std::filesystem::path& path)
    {
        auto rootPart = root.begin();
        auto pathPart = path.begin();
        for (; rootPart != root.end(); ++rootPart, ++pathPart)
        {
            if (pathPart == path.end())
            {
                return false;
            }
#if defined(_WIN32)
            std::string rootText = rootPart->generic_string();
            std::string pathText = pathPart->generic_string();
            const auto toLower = [](unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            };
            std::transform(rootText.begin(), rootText.end(), rootText.begin(), toLower);
            std::transform(pathText.begin(), pathText.end(), pathText.begin(), toLower);
            if (rootText != pathText)
#else
            if (*rootPart != *pathPart)
#endif
            {
                return false;
            }
        }
        return true;
    }
} // namespace

bool LoadShaderProgramCatalog(
    const std::filesystem::path& catalogPath,
    std::vector<ShaderProgramCatalogEntry>& outEntries,
    std::string& outError)
{
    outEntries.clear();
    std::ifstream file(catalogPath, std::ios::binary);
    if (!file.is_open())
    {
        outError = "Cannot open shader program catalog: " +
            catalogPath.string();
        return false;
    }

    Json root;
    try
    {
        file >> root;
    }
    catch (const std::exception& exception)
    {
        outError = "Shader program catalog JSON is invalid: " +
            std::string(exception.what());
        return false;
    }
    if (!root.is_object() ||
        !HasOnlyKeys(root, {"schema", "version", "programs"}) ||
        root.value("schema", "") != RVX_SHADER_PROGRAM_CATALOG_SCHEMA ||
        root.value("version", 0u) != RVX_SHADER_PROGRAM_CATALOG_VERSION ||
        !root.contains("programs") || !root["programs"].is_array() ||
        root["programs"].empty())
    {
        outError = "Shader program catalog root contract is invalid";
        return false;
    }

    size_t programIndex = 0;
    for (const Json& program : root["programs"])
    {
        ShaderProgramCatalogEntry entry;
        if (!program.is_object() ||
            !HasOnlyKeys(
                program, {"asset", "stage", "entry", "backends", "defines"}) ||
            !program.contains("asset") || !program["asset"].is_string() ||
            !program.contains("stage") || !program["stage"].is_string() ||
            !program.contains("entry") || !program["entry"].is_string() ||
            !program.contains("backends") || !program["backends"].is_array() ||
            program["backends"].empty())
        {
            outError = "Shader program catalog entry " +
                std::to_string(programIndex) + " has invalid fields";
            return false;
        }
        entry.logicalAssetId = program["asset"].get<std::string>();
        entry.entryPoint = program["entry"].get<std::string>();
        const std::string stage = program["stage"].get<std::string>();
        if (!IsCanonicalLogicalAssetId(entry.logicalAssetId) ||
            entry.entryPoint.empty() || !ParseStage(stage, entry.stage))
        {
            outError = "Shader program catalog entry " +
                std::to_string(programIndex) + " has invalid identity";
            return false;
        }

        std::set<RHIBackendType> backendSet;
        for (const Json& backendJson : program["backends"])
        {
            RHIBackendType backend = RHIBackendType::None;
            if (!backendJson.is_string() ||
                !ParseBackend(backendJson.get<std::string>(), backend) ||
                !backendSet.insert(backend).second)
            {
                outError = "Shader program catalog entry " +
                    std::to_string(programIndex) +
                    " has an invalid or duplicate backend";
                return false;
            }
            entry.backends.push_back(backend);
        }

        if (program.contains("defines"))
        {
            if (!program["defines"].is_array())
            {
                outError = "Shader catalog defines must be an array";
                return false;
            }
            for (const Json& defineJson : program["defines"])
            {
                if (!defineJson.is_object() ||
                    !HasOnlyKeys(defineJson, {"name", "value"}) ||
                    !defineJson.contains("name") ||
                    !defineJson["name"].is_string() ||
                    !defineJson.contains("value") ||
                    !defineJson["value"].is_string())
                {
                    outError = "Shader catalog define contract is invalid";
                    return false;
                }
                entry.defines.push_back({
                    defineJson["name"].get<std::string>(),
                    defineJson["value"].get<std::string>(),
                });
            }
            std::string defineError;
            if (!CanonicalizeShaderMacros(entry.defines, defineError))
            {
                outError = "Shader catalog entry " +
                    std::to_string(programIndex) + ": " + defineError;
                return false;
            }
        }
        outEntries.push_back(std::move(entry));
        ++programIndex;
    }
    outError.clear();
    return true;
}

bool ResolveShaderProgramCatalogEntry(
    const ShaderProgramCatalogEntry& entry,
    RHIBackendType backend,
    ShaderProgramRequest& outRequest,
    std::string& outError)
{
    outRequest = {};
    if (!IsConcreteBackend(backend) ||
        std::find(entry.backends.begin(), entry.backends.end(), backend) ==
            entry.backends.end())
    {
        outError = "Shader catalog entry does not target the requested backend";
        return false;
    }
    outRequest.logicalAssetId = entry.logicalAssetId;
    outRequest.stage = entry.stage;
    outRequest.entryPoint = entry.entryPoint;
    outRequest.backend = backend;
    outRequest.defines = entry.defines;
    for (ShaderMacro& define : outRequest.defines)
    {
        if (define.value == "$backend.clipY")
        {
            define.value =
                backend == RHIBackendType::Vulkan ||
                    backend == RHIBackendType::OpenGL
                ? "1"
                : "0";
        }
        else if (!define.value.empty() && define.value.front() == '$')
        {
            outError = "Shader catalog contains an unknown symbolic define";
            return false;
        }
    }
    if (!CanonicalizeShaderMacros(outRequest.defines, outError))
    {
        return false;
    }
    outError.clear();
    return true;
}

ShaderProgramCatalogCookResult CookShaderProgramCatalog(
    const ShaderProgramCatalogCookRequest& request,
    ShaderImporter::CompilerFactory compilerFactory)
{
    ShaderProgramCatalogCookResult result;
    if (!IsConcreteBackend(request.backend))
    {
        result.error = "Shader catalog cook requires one concrete backend";
        return result;
    }
    std::vector<ShaderProgramCatalogEntry> entries;
    if (!LoadShaderProgramCatalog(
            request.catalogPath, entries, result.error))
    {
        return result;
    }
    result.catalogProgramCount = entries.size();

    std::error_code pathError;
    const std::filesystem::path sourceRoot =
        std::filesystem::weakly_canonical(request.sourceRoot, pathError);
    if (pathError || !std::filesystem::is_directory(sourceRoot, pathError))
    {
        result.error = "Shader catalog source root is invalid";
        return result;
    }
    std::filesystem::create_directories(request.outputRoot, pathError);
    if (pathError)
    {
        result.error = "Cannot create shader catalog output root: " +
            pathError.message();
        return result;
    }

    std::unique_ptr<ShaderImporter> importer = compilerFactory
        ? std::make_unique<ShaderImporter>(std::move(compilerFactory))
        : std::make_unique<ShaderImporter>();
    std::set<std::string> outputIdentities;
    for (size_t index = 0; index < entries.size(); ++index)
    {
        const ShaderProgramCatalogEntry& entry = entries[index];
        if (std::find(
                entry.backends.begin(), entry.backends.end(), request.backend) ==
            entry.backends.end())
        {
            ++result.skippedProgramCount;
            continue;
        }

        ShaderProgramRequest programRequest;
        std::string resolveError;
        if (!ResolveShaderProgramCatalogEntry(
                entry, request.backend, programRequest, resolveError))
        {
            result.error = "Shader catalog entry " + std::to_string(index) +
                " cannot be resolved: " + resolveError;
            return result;
        }
        const std::filesystem::path sourcePath =
            std::filesystem::weakly_canonical(
                sourceRoot / programRequest.logicalAssetId, pathError);
        if (pathError || !IsPathContained(sourceRoot, sourcePath) ||
            !std::filesystem::is_regular_file(sourcePath, pathError))
        {
            result.error = "Shader catalog source is missing or escapes root: " +
                programRequest.logicalAssetId;
            return result;
        }
        const std::filesystem::path relativeOutput =
            BuildShaderArtifactRelativePath(programRequest);
        if (!outputIdentities.insert(relativeOutput.generic_string()).second)
        {
            result.error = "Shader catalog resolves duplicate program output: " +
                relativeOutput.generic_string();
            return result;
        }

        ShaderImportOptions options;
        options.stage = programRequest.stage;
        options.targetBackend = programRequest.backend;
        options.logicalAssetId = programRequest.logicalAssetId;
        options.entryPoint = programRequest.entryPoint;
        options.defines = programRequest.defines;
        options.enableDebugInfo = false;
        options.enableOptimization = true;
        const std::filesystem::path outputPath =
            request.outputRoot / relativeOutput;
        const ImportResult import = importer->Import(
            sourcePath, outputPath, &options);
        if (!import.success)
        {
            result.error = "Shader catalog entry " + std::to_string(index) +
                " failed to cook: " + import.error;
            return result;
        }
        result.outputPaths.push_back(outputPath);
        ++result.cookedProgramCount;
    }
    if (result.cookedProgramCount == 0)
    {
        result.error = "Shader catalog selected zero programs";
        return result;
    }
    result.success = true;
    return result;
}
} // namespace RVX::Tools
