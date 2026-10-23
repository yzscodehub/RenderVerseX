#pragma once

/** @file ShaderProgramCatalog.h @brief Versioned shader program cook catalog */

#include "Tools/AssetPipeline.h"

#include <filesystem>
#include <string>
#include <vector>

namespace RVX::Tools
{
    inline constexpr uint32 RVX_SHADER_PROGRAM_CATALOG_VERSION = 2;
    inline constexpr const char* RVX_SHADER_PROGRAM_CATALOG_SCHEMA =
        "RVX_SHADER_PROGRAM_CATALOG";

    struct ShaderProgramCatalogEntry
    {
        std::string logicalAssetId;
        RHIShaderStage stage = RHIShaderStage::None;
        std::string entryPoint;
        std::vector<RHIBackendType> backends;
        std::vector<ShaderMacro> defines;
    };

    struct ShaderProgramCatalogCookRequest
    {
        std::filesystem::path catalogPath;
        std::filesystem::path sourceRoot;
        std::filesystem::path outputRoot;
        RHIBackendType backend = RHIBackendType::None;
    };

    struct ShaderProgramCatalogCookResult
    {
        bool success = false;
        size_t catalogProgramCount = 0;
        size_t cookedProgramCount = 0;
        size_t skippedProgramCount = 0;
        std::vector<std::filesystem::path> outputPaths;
        std::string error;
    };

    [[nodiscard]] bool LoadShaderProgramCatalog(
        const std::filesystem::path& catalogPath,
        std::vector<ShaderProgramCatalogEntry>& outEntries,
        std::string& outError);

    [[nodiscard]] bool ResolveShaderProgramCatalogEntry(
        const ShaderProgramCatalogEntry& entry,
        RHIBackendType backend,
        ShaderProgramRequest& outRequest,
        std::string& outError);

    [[nodiscard]] ShaderProgramCatalogCookResult CookShaderProgramCatalog(
        const ShaderProgramCatalogCookRequest& request,
        ShaderImporter::CompilerFactory compilerFactory = {});
} // namespace RVX::Tools
